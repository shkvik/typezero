#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/destination/destination.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

VarChecker::VarChecker(Checker &checker) : checker_(checker) {}

void VarChecker::checkVarDecl(VarDeclStmt *decl) {
  llvm::ArrayRef<Stmt *> block = checker_.block_;
  size_t index = checker_.blockIndex_;
  checker_.block_ = {};
  for (size_t k = 0; k < decl->decls.size(); ++k) {
    VarDeclarator &d = decl->decls[k];
    const Type *declared =
        d.typeAnn ? checker_.typeResolver().resolveType(*d.typeAnn, /*allowVoid=*/false) : nullptr;
    // `const f = first` with a generic `first`: another name for it, as
    // generic as it is.
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(d.init);
        name && decl->isConst && !declared && !d.destructured && name->typeArgs.empty()) {
      if (FuncSymbol *generic = checker_.genericChecker().genericNamed(name)) {
        checker_.scopes().declare(d.name, d.loc, {nullptr, generic});
        d.alias = true;
        continue;
      }
    }
    // `const id = <T>(x: T) => x` at the top level: a generic function, as if
    // declared with `function` (an arrow there captures nothing: what it uses
    // are globals). Unlike a declaration, it exists from here on.
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init);
        fe && !fe->fn->typeParams.empty() && decl->isConst && !declared && !d.destructured &&
        checker_.scopes_.size() == 1) {
      FunctionDecl *fn = fe->fn;
      fn->isExpression = false;
      fn->lifted = true;
      fn->name = d.name;
      checker_.functionChecker().declareFunction(fn);
      checker_.callCycles().findCycles({fn});
      checker_.lifted_.push_back(fn);
      d.alias = true;
      continue;
    }
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init)) {
      if (fe->fn->name.empty()) {
        fe->fn->name = d.name; // as JavaScript names it
      }
      if (decl->isConst && !d.destructured) {
        // The constant exists while its function is checked, so that the
        // function can call itself through it.
        VarSymbol *var = checker_.scopes().declareVar(d.name, d.loc, nullptr, /*isConst=*/true);
        var->lambda = fe->fn;
        d.sym = var;
        const Type *function =
            declared && declared->nonOptional()->isFunction() ? declared->nonOptional() : nullptr;
        Checker::Signature sig =
            function ? ClosureChecker::signatureOf(function) : Checker::Signature();
        d.init = checker_.closureChecker().checkFunctionExpr(fe, function ? &sig : nullptr);
        if (declared) {
          d.init = checker_.typeRelations().coerce(d.init, declared);
        }
        // A function of another result is called through what adapts it.
        if (auto *cast = dyn_cast<CastExpr>(d.init); cast && cast->adaptsResult) {
          var->lambda = nullptr;
        }
        var->type = declared ? declared : d.init->type;
        continue;
      }
    }
    const Type *type = declared;
    Expr *value = nullptr; // the initializer before its conversion
    bool untyped = false;  // reported with the scope (see noteUntyped)
    // A literal takes its type from where the variable goes.
    const Type *hint = declared;
    std::vector<std::pair<const Type *, llvm::SMLoc>> places;
    if (!declared && d.init && !d.destructured && Destination::literalShaped(d.init)) {
      checker_.following_ =
          block.empty() ? std::vector<const Stmt *>() : followingStatements(block, index);
      hint = checker_.destination().literalDestination(
          d, llvm::ArrayRef(decl->decls).drop_front(k + 1));
      places = std::move(checker_.destinationConflict_);
      checker_.destinationConflict_.clear();
    }
    // `const t = setInterval(() => { ... clearInterval(t) }, 10)`: the
    // callback of a timer runs later, when t holds the timer, so it may use
    // t; through a box, as a closure created before its variable does.
    VarSymbol *early = nullptr;
    if (const Type *timer =
            d.init && !d.destructured ? checker_.timerChecker().timerCallType(d.init) : nullptr) {
      early =
          checker_.scopes().declareVar(d.name, d.loc, declared ? declared : timer, decl->isConst);
      early->declSeq = ~0u;
    }
    if (d.init) {
      if (!early) {
        checker_.initializing_.push_back(d.name);
      }
      if (d.destructured && !d.objectPattern) {
        checker_.iteratorMethods().markFused(d.init);
      }
      d.init = value = checker_.exprChecker().checkExpr(d.init, hint);
      if (d.destructured && !d.objectPattern) {
        d.init = value = checker_.dictChecker().iterableOf(d.init);
      }
      if (!early) {
        checker_.initializing_.pop_back();
      }
      if (declared) {
        d.init = checker_.typeRelations().coerce(d.init, declared);
      } else {
        type = d.init->type;
        if (type->isVoid()) {
          checker_.diags_.error(d.init->loc(),
                                "an expression of type 'void' cannot initialize a variable");
          type = Type::error();
        } else if (type->isUndefined() && !decl->isConst && !d.destructured) {
          untyped = true;
          type = Type::error();
        } else if (type->isUndefined()) {
          checker_.diags_.error(
              d.init->loc(), llvm::Twine("cannot infer a type from 'undefined'; add one, e.g. '") +
                                 (decl->isConst ? "const " : "let ") + d.name +
                                 ": int | undefined = undefined'");
          type = Type::error();
        }
      }
    } else if (decl->isConst) {
      checker_.diags_.error(d.loc, "'const' declarations must be initialized");
      if (!type) {
        type = Type::error();
      }
    } else if (!declared) {
      untyped = true;
      type = Type::error();
    }

    if (d.destructured && d.objectPattern) {
      // The fields are read by name: `%0.x` reports what has no field x.
    } else if (d.destructured && (type->isString() || type->isTypedArray()) && !declared) {
      // A string destructures into its characters, as JavaScript's iterator
      // gives them: `[...s]`; a typed array into its elements, and `...rest`
      // is an array of them, as in JavaScript.
      auto *chars = checker_.ctx_.make<ArrayLitExpr>(d.init->loc());
      chars->elements.push_back({d.init, /*spread=*/true});
      chars->type = Type::array(type->isString() ? Type::string() : type->elementType());
      d.init = value = chars;
      type = chars->type;
    } else if (d.destructured && type->isIterator() && !declared) {
      // `const [a, b] = it` takes the first two values of the iterator.
      d.init = value = checker_.generatorChecker().destructuredIterator(d.init, d);
      type = d.init->type;
    } else if (d.destructured && !type->isArray() && !type->isTuple() && !type->isMatch() &&
               !type->isError() &&
               !type->nonOptional()->isJson()) { // a Json reads its elements by index
      std::string message = "only arrays can be destructured, found '" + type->name().str() + "'";
      if (type->isOptional() && type->inner()->isArray()) {
        message += "; the array may be undefined: check it first";
      }
      checker_.diags_.error(d.init ? d.init->loc() : d.loc, message);
      type = Type::error();
    }

    if (early) {
      early->declSeq = ++checker_.seq_;
      d.sym = early;
    } else {
      d.sym = checker_.scopes().declareVar(d.name, d.loc, type, decl->isConst);
    }
    checker_.regexChecker().noteRegexVariable(d.sym, value, decl->isConst, block, index);
    // `const m = Reflect.get(user, "save")`: the method it holds.
    if (decl->isConst && !d.destructured) {
      if (auto *got = llvm::dyn_cast_or_null<CallExpr>(value);
          got && got->builtin == Builtin::ReflectMethod &&
          !checker_.severalVariants_.contains(got)) {
        d.sym->reflectMethod = got->method->decl;
      }
    }
    if (decl->isConst && value && (isa<ObjectLitExpr>(value) || isa<StringLitExpr>(value))) {
      checker_.constLiterals_[d.sym] = value;
    }
    if (!places.empty()) {
      checker_.destinations_[d.sym] = std::move(places);
    }
    if (untyped) {
      noteUntyped(d.sym, d.init != nullptr);
    }
    if (value) {
      checker_.flowChecker().assigned(d.sym, value);
    }
    if (decl->isConst && d.init && type->isInteger()) {
      if (std::optional<int64_t> constant = checker_.constantFolder().intConstant(d.init)) {
        checker_.constVars_[d.sym] = *constant;
      }
    }
    // A string constant may name a field: `o[key]`.
    if (decl->isConst && d.init && type->isString()) {
      if (std::optional<std::string> key = checker_.keyedFieldChecker().constantKey(d.init)) {
        checker_.constKeys_[d.sym] = std::move(*key);
      }
      // And how it starts, which tells whether a fetch of it is of http:// (noteHttpsCall).
      if (std::string prefix = checker_.tzModulesChecker().knownPrefix(d.init); !prefix.empty()) {
        checker_.constPrefixes_[d.sym] = std::move(prefix);
      }
    }
  }
}

// As in TypeScript, the type of a variable is where it is declared; unlike
// tsc, Checker does not take it from the assignments that follow. It looks at
// them only for the hint, so the error waits for the end of the scope.
void VarChecker::noteUntyped(VarSymbol *var, bool initUndefined) {
  checker_.untyped_.push_back({var, initUndefined, {}});
}

void VarChecker::assignedUntyped(const VarSymbol *var, const Type *type) {
  for (Checker::Untyped &u : checker_.untyped_) {
    if (u.var == var && !type->isError() && !type->isVoid()) {
      u.assigned.push_back(type);
    }
  }
}

// Reports the variables declared in the innermost scope, which is ending.
void VarChecker::reportUntyped() {
  llvm::erase_if(checker_.untyped_, [&](const Checker::Untyped &u) {
    auto found = checker_.scopes_.back().find(u.var->name);
    if (found == checker_.scopes_.back().end() || found->second.var != u.var) {
      return false;
    }
    // The type that holds every value assigned, if there is one.
    const Type *type = u.initUndefined ? Type::undefinedTy() : nullptr;
    for (const Type *t : u.assigned) {
      const Type *common = type ? TypeRelations::unify(type, t) : t;
      type = common ? common : Type::error();
    }
    std::string name = u.var->name.str();
    std::string hint;
    if (type && !type->isError() && !type->isUndefined()) {
      hint = "write the type: 'let " + name + ": " + type->name().str() + "'";
    } else if (u.initUndefined) {
      hint = "add one, e.g. 'let " + name + ": int | undefined'";
    } else {
      hint = "e.g. 'let " + name + ": int' or 'let " + name + " = 0'";
    }
    if (u.initUndefined) {
      checker_.diags_.error(u.var->loc, "cannot infer a type from 'undefined'; " + hint);
    } else {
      checker_.diags_.error(u.var->loc, "variable '" + name +
                                            "' needs a type annotation or an initializer; " + hint);
    }

    return true;
  });
}

} // namespace tz
