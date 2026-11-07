#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
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
#include "compiler/load/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

bool VarChecker::isTextChoice(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return false;
    }
    expr = conv->operand;
  }
  if (auto *choice = dyn_cast<ConditionalExpr>(expr)) {
    return VarChecker::isTextChoice(choice->thenExpr) && VarChecker::isTextChoice(choice->elseExpr);
  }

  return isa<StringLitExpr>(expr);
}

VarChecker::VarChecker(Checker &owner)
    : checker(owner) {}

void VarChecker::checkVarDecl(VarDeclStmt *decl) {
  llvm::ArrayRef<Stmt *> block = this->checker.block_;
  size_t index = this->checker.blockIndex_;
  this->checker.block_ = {};
  for (size_t k = 0; k < decl->decls.size(); ++k) {
    VarDeclarator &d = decl->decls[k];
    const Type *declared =
        d.typeAnn ? this->checker.typeResolver().resolveType(*d.typeAnn, false) : nullptr;
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(d.init);
        name && decl->isConst && !declared && !d.destructured() && name->typeArgs.empty()) {
      if (FuncSymbol *generic = this->checker.genericChecker().genericNamed(name)) {
        this->checker.scopes().declare(d.name, d.loc, {FunctionEntry{.func = generic}});
        d.alias = true;
        continue;
      }
    }
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init);
        fe && !fe->fn->typeParams.empty() && decl->isConst && !declared && !d.destructured() &&
        this->checker.scopes_.size() == 1) {
      FunctionDecl *fn = fe->fn;
      fn->place = FunctionPlace::LIFTED;
      fn->name = d.name;
      if (this->checker.module_) {
        this->checker.declModules_[fn] = this->checker.module_;
      }
      this->checker.functionChecker().declareFunction(fn);
      this->checker.callCycles().findCycles({fn});
      this->checker.lifted_.push_back(fn);
      d.alias = true;
      continue;
    }
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init)) {
      if (fe->fn->name.empty()) {
        fe->fn->name = d.name;
      }
      if (decl->isConst && !d.destructured()) {
        VarSymbol *var = this->checker.scopes().declareVar(d.name, d.loc, nullptr, true);
        var->holds = Lambda{.lambda = fe->fn};
        d.sym = var;
        const Type *function =
            declared && declared->nonOptional()->isFunction() ? declared->nonOptional() : nullptr;
        Checker::Signature sig =
            function ? ClosureChecker::signatureOf(function) : Checker::Signature();
        d.init = this->checker.closureChecker().checkFunctionExpr(fe, function ? &sig : nullptr);
        if (declared) {
          d.init = this->checker.typeRelations().coerce(d.init, declared);
        }
        if (auto *cast = dyn_cast<CastExpr>(d.init); cast && cast->adaptsResult) {
          var->holds = OrdinaryValue{};
        }
        var->type = declared ? declared : d.init->type;
        continue;
      }
    }
    const Type *type = declared;
    Expr *value = nullptr;
    bool untyped = false;
    const Type *hint = declared;
    std::vector<std::pair<const Type *, llvm::SMLoc>> places;
    if (!declared && d.init && !d.destructured() && Destination::literalShaped(d.init)) {
      this->checker.following_ =
          block.empty() ? std::vector<const Stmt *>() : followingStatements(block, index);
      hint = this->checker.destination().literalDestination(
          d, llvm::ArrayRef(decl->decls).drop_front(k + 1));
      places = std::move(this->checker.destinationConflict_);
      this->checker.destinationConflict_.clear();
    }
    VarSymbol *early = nullptr;
    if (const Type *timer = d.init && !d.destructured()
                                ? this->checker.timerChecker().timerCallType(d.init)
                                : nullptr) {
      early = this->checker.scopes().declareVar(d.name, d.loc, declared ? declared : timer,
                                                decl->isConst);
      if (LocalVarState *state = early->local()) {
        state->declSeq = ~0u;
      }
    }
    if (d.init) {
      if (!early) {
        this->checker.initializing_.push_back(d.name);
      }
      if (d.destructured() && !d.objectPattern()) {
        this->checker.iteratorMethods().markFused(d.init);
      }
      d.init = value = this->checker.exprChecker().checkExpr(d.init, hint);
      if (d.destructured() && !d.objectPattern()) {
        d.init = value = this->checker.dictChecker().iterableOf(d.init);
      }
      if (!early) {
        this->checker.initializing_.pop_back();
      }
      if (declared) {
        d.init = this->checker.typeRelations().coerce(d.init, declared);
      } else {
        type = d.init->type;
        if (type->isVoid()) {
          this->checker.diags_.error(d.init->loc(),
                                     "an expression of type 'void' cannot initialize a variable");
          type = Type::error();
        } else if (type->isUndefined() && !decl->isConst && !d.destructured()) {
          untyped = true;
          type = Type::error();
        } else if (type->isUndefined()) {
          this->checker.diags_.error(
              d.init->loc(), llvm::Twine("cannot infer a type from 'undefined'; add one, e.g. '") +
                                 (decl->isConst ? "const " : "let ") + d.name +
                                 ": int | undefined = undefined'");
          type = Type::error();
        }
      }
    } else if (decl->isConst) {
      this->checker.diags_.error(d.loc, "'const' declarations must be initialized");
      if (!type) {
        type = Type::error();
      }
    } else if (!declared) {
      untyped = true;
      type = Type::error();
    }

    if (d.destructured() && d.objectPattern()) {
    } else if (d.destructured() && (type->isString() || type->isTypedArray()) && !declared) {
      auto *chars = this->checker.ctx_.make<ArrayLitExpr>(d.init->loc());
      chars->elements.push_back({d.init, true});
      chars->type = Type::array(type->isString() ? Type::string() : type->elementType());
      d.init = value = chars;
      type = chars->type;
    } else if (d.destructured() && type->isIterator() && !declared) {
      d.init = value = this->checker.generatorChecker().destructuredIterator(d.init, d);
      type = d.init->type;
    } else if (d.destructured() && !type->isArray() && !type->isTuple() && !type->isMatch() &&
               !type->isError() && !type->nonOptional()->isJson()) {
      std::string message = "only arrays can be destructured, found '" + type->name().str() + "'";
      if (type->isOptional() && type->inner()->isArray()) {
        message += "; the array may be undefined: check it first";
      }
      this->checker.diags_.error(d.init ? d.init->loc() : d.loc, message);
      type = Type::error();
    }

    if (early) {
      if (LocalVarState *state = early->local()) {
        state->declSeq = ++this->checker.seq_;
      }
      d.sym = early;
    } else {
      d.sym = this->checker.scopes().declareVar(d.name, d.loc, type, decl->isConst);
    }
    this->checker.regexChecker().noteRegexVariable(d.sym, value, decl->isConst, block, index);
    if (decl->isConst && !d.destructured()) {
      if (auto *got = llvm::dyn_cast_or_null<CallExpr>(value);
          got && got->builtin() == Builtin::ReflectMethod &&
          !this->checker.severalVariants_.contains(got)) {
        d.sym->holds = ReflectedMethod{.method = got->method()->decl};
      }
      if (auto *got = llvm::dyn_cast_or_null<CallExpr>(value);
          got && got->builtin() == Builtin::ReflectGet && got->args.size() == 2) {
        const Expr *key = got->args[1];
        while (auto *conv = llvm::dyn_cast<CastExpr>(key)) {
          if (!conv->isImplicit()) {
            break;
          }
          key = conv->operand;
        }
        std::vector<std::u16string> names;
        if (auto *lit = llvm::dyn_cast<StringLitExpr>(key)) {
          names.push_back(lit->value);
        } else if (const Type *keyType = key->type ? key->type->nonOptional() : nullptr) {
          std::vector<const Type *> members;
          if (keyType->isUnion()) {
            members.assign(keyType->members().begin(), keyType->members().end());
          } else {
            members.push_back(keyType);
          }
          for (const Type *m : members) {
            if (!m->isLiteral() || !m->literalBase()->isString()) {
              names.clear();
              break;
            }
            names.push_back(m->literalText());
          }
        }
        std::vector<llvm::StringRef> kept;
        for (const std::u16string &name : names) {
          kept.push_back(this->checker.ctx_.keepName(toUtf8(name)));
        }
        if (!kept.empty()) {
          d.sym->holds = ReflectedNames{.names = std::move(kept)};
        }
      }
      auto *made = llvm::dyn_cast_or_null<CallExpr>(value);
      bool fresh =
          llvm::isa_and_nonnull<ArrayLitExpr>(value) ||
          (made &&
           (made->builtin() == Builtin::ArrMap || made->builtin() == Builtin::ArrFilter ||
            made->builtin() == Builtin::ArrSlice || made->builtin() == Builtin::ArrConcat ||
            made->builtin() == Builtin::ArrFlatMap || made->builtin() == Builtin::ArrayFrom));
      if (LocalVarState *state = d.sym->local()) {
        state->unshared =
            fresh && type->isArray() && FlowChecker::staysLocal(d.sym->owner(), d.name);
      }
    }
    if (decl->isConst && value &&
        (isa<ObjectLitExpr>(value) || isa<StringLitExpr>(value) ||
         (isa<ConditionalExpr>(value) && VarChecker::isTextChoice(value)))) {
      this->checker.constLiterals_[d.sym] = value;
      d.sym->literalConst = !declared && type->isString();
    }
    if (auto *alias = dyn_cast_or_null<NameExpr>(value); decl->isConst && alias && alias->var() &&
                                                         alias->var()->literalConst && !declared &&
                                                         type->isString()) {
      this->checker.constLiterals_[d.sym] = this->checker.constLiterals_.lookup(alias->var());
      d.sym->literalConst = true;
    }
    if (!places.empty()) {
      this->checker.destinations_[d.sym] = std::move(places);
    }
    if (const Type *held = type->nonOptional();
        !d.destructured() && !this->checker.checkingInstance_ &&
        (held->isResult() || (held->isPromise() && held->promised()->isResult())) &&
        !this->exported(d.name)) {
      this->checker.resultVars_.push_back(d.sym);
    }
    if (untyped) {
      this->noteUntyped(d.sym, d.init != nullptr);
    }
    if (value) {
      this->checker.flowChecker().assigned(d.sym, value);
    }
    if (const Type *base = type->nonOptional();
        decl->isConst && value && !d.destructured() &&
        (base->isByteView() || base->bytes() == Type::Bytes::ArrayBuffer)) {
      if (std::optional<Linear> length = this->checker.indexProofs().knownLength(value);
          length && length->term.isZero()) {
        this->checker.fixedLengths_[d.sym] = length->offset;
      }
    }
    if (decl->isConst && d.init && type->isInteger()) {
      this->checker.constantFolder().noteUnsignedVar(d.sym, d.init);
      if (std::optional<int64_t> constant = this->checker.constantFolder().intConstant(d.init)) {
        this->checker.constVars_[d.sym] = *constant;
        d.sym->literalConst = !declared && type->isInt() && TypeRelations::isIntLiteralTree(d.init);
      }
    }
    if (decl->isConst && d.init && type->isString()) {
      if (std::optional<std::string> key = this->checker.keyedFieldChecker().constantKey(d.init)) {
        this->checker.constKeys_[d.sym] = std::move(*key);
      }
      if (std::string prefix = this->checker.tzModulesChecker().knownPrefix(d.init);
          !prefix.empty()) {
        this->checker.constPrefixes_[d.sym] = std::move(prefix);
      }
    }
  }
}

bool VarChecker::exported(llvm::StringRef name) const {
  if (this->checker.scopes_.size() != 1 || !this->checker.module_) {
    return false;
  }

  return llvm::any_of(this->checker.module_->file.exports, [&](const ExportDecl &decl) {
    const LocalExport *local = decl.form.as<LocalExport>();

    return local && llvm::any_of(local->names, [&](const ModuleName &n) { return n.name == name; });
  });
}

void VarChecker::reportUncheckedResults() {
  llvm::DenseMap<const char *, bool> readAt;
  for (const VarSymbol *var : this->checker.resultVars_) {
    readAt[var->loc.getPointer()] |= var->read;
  }
  for (const VarSymbol *var : this->checker.resultVars_) {
    bool &done = readAt[var->loc.getPointer()];
    if (done) {
      continue;
    }
    done = true;
    std::string name = var->name.str();
    const Type *held = var->type->nonOptional();
    if (held->isPromise()) {
      this->checker.diags_.error(var->loc,
                                 "'" + name +
                                     "' holds a Promise of a Result that is never awaited, "
                                     "so an error in it would go unchecked; await it and "
                                     "check it, e.g. 'const r = await " +
                                     name + "; if (r.isErr()) ...'");
      continue;
    }
    this->checker.diags_.error(var->loc, "the Result in '" + name +
                                             "' is never checked, so an error in it would go "
                                             "unchecked; check it, e.g. 'if (" +
                                             name + ".isErr()) ...'");
  }
  this->checker.resultVars_.clear();
}

void VarChecker::noteUntyped(VarSymbol *var, bool initUndefined) {
  this->checker.untyped_.push_back({var, initUndefined, {}});
}

void VarChecker::assignedUntyped(const VarSymbol *var, const Type *type) {
  for (Checker::Untyped &u : this->checker.untyped_) {
    if (u.var == var && !type->isError() && !type->isVoid()) {
      u.assigned.push_back(type);
    }
  }
}

void VarChecker::reportUntyped() {
  llvm::erase_if(this->checker.untyped_, [&](const Checker::Untyped &u) {
    auto found = this->checker.scopes_.back().find(u.var->name);
    if (found == this->checker.scopes_.back().end() || found->second.var() != u.var) {
      return false;
    }
    const Type *type = u.initUndefined ? Type::undefinedTy() : nullptr;
    for (const Type *t : u.assigned) {
      const Type *common = type ? TypeRelations::unify(type, t) : t;
      if (!common && type && type->nonOptional()->isNumeric() && t->isNumeric()) {
        common = type->nonOptional()->isFloat() ? type : t->isFloat() ? t : nullptr;
      }
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
      this->checker.diags_.error(u.var->loc, "cannot infer a type from 'undefined'; " + hint);
    } else {
      this->checker.diags_.error(
          u.var->loc, "variable '" + name + "' needs a type annotation or an initializer; " + hint);
    }

    return true;
  });
}

}
