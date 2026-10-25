#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/declare/function_names/function_names.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it.
const Type *literalType(const Expr *value);

ClosureChecker::ClosureChecker(Sema &sema) : sema_(sema) {}

Sema::Signature ClosureChecker::signatureOf(const Type *fnType) {
  Sema::Signature sig;
  sig.params.assign(fnType->params().begin(), fnType->params().end());
  sig.minArgs = fnType->minArgs();
  sig.ret = fnType->ret();
  // As in TypeScript, a function whose result is dropped may return anything.
  sig.voidResult = fnType->ret()->isVoid();
  sig.hasThis = fnType->hasThis();

  return sig;
}

Expr *ClosureChecker::checkFunctionExpr(FunctionExpr *fe, const Sema::Signature *sig) {
  FunctionDecl *fn = fe->fn;
  FuncSymbol *sym = sema_.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->parent = sema_.currentDecl();
  fn->creationSeq = ++sema_.seq_;
  // Its type parameters stand for nothing that could be checked.
  bool generic = !fn->typeParams.empty();
  if (generic) {
    sema_.diags_.error(fe->loc(),
                 "a generic arrow or function expression can only be the value of a top-level "
                 "constant for now, e.g. 'const f = <T>(x: T) => x'; or declare a generic function "
                 "at the top level: 'function f<T>(x: T) { ... }'");
    fn->typeArgs.assign(fn->typeParams.size(), Type::error());
    sema_.typeScopes_.emplace_back();
    sema_.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
  }

  // A function of a type that takes `this`, written without a `this`
  // parameter: it gets one (see Signature::hasThis).
  bool thisFromContext = false;
  if (sig && sig->hasThis && !sig->params.empty() &&
      (fn->params.empty() || fn->params[0].name != "this" || fn->isArrow)) {
    Param self;
    self.name = fn->isArrow ? sema_.ctx_.hiddenName() : llvm::StringRef("this");
    self.loc = fe->loc();
    fn->params.insert(fn->params.begin(), self);
    thisFromContext = true;
  }

  // Parameters: an annotation, or the type the context passes.
  size_t named = fn->params.size();
  bool afterOptional = false;
  for (size_t i = 0; i < named; ++i) {
    Param &param = fn->params[i];
    const Type *given = sig && i < sig->params.size() ? sig->params[i] : nullptr;
    bool mayOmit = param.optional || param.defaultValue;
    const Type *type;   // inside the body
    const Type *passed; // by callers
    if (param.type) {
      bool outerList = std::exchange(sema_.listContext_, param.rest);
      type = sema_.typeResolver().resolveType(*param.type, /*allowVoid=*/false);
      sema_.listContext_ = outerList;
      if (param.optional) {
        type = Type::optional(type);
      }
      passed = mayOmit ? Type::optional(type) : type;
      if (given && passed != given && !passed->isError() && !given->isError() && !param.rest) {
        sema_.diags_.error(param.loc, "parameter '" + param.label() + "' is declared as '" +
                                    passed->name() + "', but the function gets '" + given->name() +
                                    "' here");
        passed = given; // reported once
        type = param.defaultValue ? given->nonOptional() : given;
      }
    } else if (given) {
      passed = given;
      type = param.defaultValue ? given->nonOptional() : given;
      if (param.defaultValue && !given->isOptional()) {
        sema_.diags_.error(param.defaultValue->loc(),
                     "the default value of '" + param.label() +
                         "' is never used: the function always gets a value here");
      }
    } else if (param.defaultValue && (type = literalType(param.defaultValue))) {
      passed = Type::optional(type);
    } else {
      sema_.diags_.error(param.loc,
                   "parameter '" + param.label() +
                       "' needs a type: nothing here says what the function is called with");
      type = passed = Type::error();
    }
    // `function (this: unknown, ...)`: the `this` it gets (Type::hasThis).
    if (i == 0 && thisFromContext) {
      sym->hasThis = true;
    } else if (i == 0 && param.name == "this" && param.type && !param.rest) {
      if (fn->isArrow) {
        sema_.diags_.error(param.loc, "an arrow function cannot have a 'this' parameter: it sees the "
                                "'this' of the function around it; write a 'function'");
      } else {
        sym->hasThis = !mayOmit;
      }
    }
    if (param.rest) {
      // Or a list of the types of parameters, `...args: A`.
      if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
        sema_.diags_.error(param.loc,
                     "a rest parameter must be an array, e.g. '..." + param.name + ": int[]'");
      }
      sym->hasRest = true;
      passed = type;
    } else if (!mayOmit) {
      if (afterOptional) {
        sema_.diags_.error(param.loc, "a required parameter cannot follow an optional one");
      }
      sym->minArgs = i + 1;
    }
    afterOptional |= mayOmit;
    param.declaredType = type;
    sym->params.push_back(passed);
  }
  named = fn->params.size();
  // It takes the arguments it does not name too, so that its type is the one
  // expected; a rest parameter takes them already.
  if (sig && sig->pad && !sym->hasRest) {
    for (size_t i = named; i < sig->params.size(); ++i) {
      Param hidden;
      hidden.name = sema_.ctx_.hiddenName();
      hidden.loc = fe->loc();
      hidden.declaredType = sig->params[i];
      fn->params.push_back(hidden);
      sym->params.push_back(sig->params[i]);
    }
    sym->minArgs = sig->minArgs;
  }
  // `...args: [int, int]`: those parameters (see expandArgumentList).
  sema_.argListChecker().expandArgumentList(fn, sym);

  if (fn->isGenerator) {
    // A generator returns an iterator, whose types its body gives unless
    // they are declared or expected.
    if (fn->returnType) {
      sym->ret = sema_.generatorChecker().generatorReturnType(fn, sema_.typeResolver().resolveType(*fn->returnType, /*allowVoid=*/true));
    } else if (sig && sig->ret &&
               (fn->isAsync ? sig->ret->isAsyncIterator() : sig->ret->isIterator())) {
      sym->ret = sig->ret;
    }
  } else if (fn->isAsync) {
    // An async function returns a Promise of what its body returns. Where
    // the result is dropped, so is that Promise, which only a callback that
    // runs on its own may do.
    if (sig && sig->voidResult) {
      if (!sig->asyncVoid) {
        sema_.diags_.error(fe->loc(), "the Promise of this async function is dropped here, so nothing "
                                "waits for it to finish; loop with 'for (const x of xs) await "
                                "f(x)', or wait for all with 'await Promise.all(xs.map(async x "
                                "=> ...))'");
      }
      sym->ret = Type::voidTy();
      sym->discardsResult = true;
    } else if (fn->returnType) {
      sym->ret = sema_.asyncChecker().asyncReturnType(fn, sema_.typeResolver().resolveType(*fn->returnType, /*allowVoid=*/true));
    } else if (sig && sig->ret && sig->ret->isPromise()) {
      sym->ret = sig->ret;
    } else if (!ReturnInferrer::returnsValue(fn->body)) {
      sym->ret = Type::promise(Type::voidTy());
    }
  } else if (sig && sig->voidResult) {
    sym->ret = Type::voidTy();
    sym->discardsResult = true;
  } else if (fn->returnType) {
    sym->ret = sema_.predicateChecker().resolveReturnType(fn, sym);
  } else if (sig && sig->ret) {
    sym->ret = sig->ret;
  } else if (!ReturnInferrer::returnsValue(fn->body)) {
    sym->ret = Type::voidTy();
  }
  if (generic) {
    sema_.typeScopes_.pop_back();
  }

  // A function that calls itself, through its constant or its own name,
  // needs its return type before its body is checked: found on copies, as
  // for recursive declarations (see inferCycle).
  if (!sym->ret) {
    llvm::StringSet<> names;
    collectNames(fn->body, names);
    bool recursive = !fn->ownName.empty() && names.contains(fn->ownName);
    for (const auto &entry : names) {
      VarSymbol *var = sema_.scopes().lookup(entry.getKey()).var;
      recursive |= var && var->lambda == fn;
    }
    if (recursive) {
      sema_.cycleOf_[sym] = static_cast<unsigned>(sema_.cycles_.size());
      sema_.cycles_.push_back({sym});
      Sema::BodyState saved = sema_.returnInferrer().enterBody(fn);
      bool ownScope = !fn->ownName.empty();
      if (ownScope) {
        declareOwnName(fn);
      }
      sema_.returnInferrer().inferCycle(sema_.cycles_.back());
      if (ownScope) {
        sema_.popScope();
      }
      sema_.returnInferrer().leaveBody(saved);
    }
  }
  // Created in a constructor before every field is set, it must not use
  // `this` (see checkThis).
  bool outerReady = sema_.closureThisReady_;
  if (sema_.initFn_ && sema_.currentDecl() == sema_.initFn_) {
    sema_.closureThisReady_ = sema_.classChecker().thisReady(sema_.flow_);
  }
  checkClosureBody(fn);
  sema_.closureThisReady_ = outerReady;
  fe->type = sema_.returnInferrer().functionType(sym);

  return fe;
}

void ClosureChecker::declareOwnName(FunctionDecl *fn) {
  sema_.pushScope();
  VarSymbol *self = sema_.ctx_.newVar();
  self->name = fn->ownName;
  self->loc = fn->loc();
  self->isConst = true;
  self->lambda = fn;
  self->owner = fn;
  self->declSeq = ++sema_.seq_;
  sema_.scopes().declare(fn->ownName, fn->loc(), {self, nullptr});
}

void ClosureChecker::checkClosureBody(FunctionDecl *fn) {
  Sema::BodyState saved = sema_.returnInferrer().enterBody(fn);
  // `function f() { ... f() ... }`: the name is visible inside.
  bool ownScope = fn->isExpression && !fn->ownName.empty();
  if (ownScope) {
    declareOwnName(fn);
  }
  if (fn->sym->ret) {
    sema_.functionChecker().checkFunctionBody(fn);
  } else {
    sema_.returnInferrer().checkAndInfer(fn);
  }
  if (ownScope) {
    sema_.popScope();
  }
  sema_.returnInferrer().leaveBody(saved);
}

void ClosureChecker::checkStatements(BlockStmt *block) {
  sema_.typeDeclarer().declareTypes(block->body);
  NestedGroup *group = sema_.nestedFunctions().declareNested(block);
  if (group) {
    sema_.groups_.push_back(group);
  }
  Sema::BlockNames names{sema_.functions_.size(), {}};
  for (Stmt *stmt : block->body) {
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt)) {
      for (const VarDeclarator &d : decl->decls) {
        names.names.push_back(d.name);
      }
    }
  }
  sema_.blockNames_.push_back(std::move(names));
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse) {
      // The functions are created here.
      group->creationSeq = ++sema_.seq_;
      for (FunctionDecl *fn : group->members) {
        fn->var->declSeq = group->creationSeq;
      }
    }
    sema_.block_ = block->body;
    sema_.blockIndex_ = i;
    sema_.stmtChecker().checkStmt(block->body[i]);
    // An `if` an instance's types decided ended in a return, so the code after
    // it cannot run in this instance: dropped unchecked, as the branch that
    // cannot run is (see checkIf). `if (typeof x == "string") return f(x)`
    // then `return g(x)`, the code for the other types.
    auto *decided = dyn_cast<IfStmt>(block->body[i]);
    if (sema_.checkingInstance_ && decided && isa<BoolLitExpr>(decided->cond) && sema_.flow_.unreachable &&
        (!group || group->firstUse <= i) &&
        llvm::none_of(llvm::ArrayRef<Stmt *>(block->body).drop_front(i + 1),
                      [](const Stmt *s) { return isa<FunctionDecl>(s) || isa<ClassDecl>(s); })) {
      block->body.resize(i + 1);
      break;
    }
  }
  sema_.blockNames_.pop_back();
  if (group) {
    sema_.nestedFunctions().finishGroup(group);
    sema_.groups_.pop_back();
  }
}

} // namespace tz
