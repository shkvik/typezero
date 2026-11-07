#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/async/async_checker/async_checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/function_names/function_names.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

ClosureChecker::ClosureChecker(Checker &owner)
    : checker(owner) {}

Checker::Signature ClosureChecker::signatureOf(const Type *fnType) {
  Checker::Signature sig;
  sig.params.assign(fnType->params().begin(), fnType->params().end());
  sig.minArgs = fnType->minArgs();
  sig.ret = fnType->ret();
  sig.voidResult = fnType->ret()->isVoid();
  sig.hasThis = fnType->hasThis();

  return sig;
}

ClosureChecker::ResultExprNotGeneric ClosureChecker::notGeneric(const FunctionDecl *fn) {
  if (!fn->typeParams.empty()) {
    std::string message = ErrorTextNotGeneric::generic();

    return err(ClosureCheckerNotGenericError{
        .code = ClosureCheckerNotGenericErrorCode::GENERIC_FUNCTION_EXPRESSION,
        .message = message,
    });
  }

  return ok(fn);
}

ClosureChecker::ResultParamTypeAgree
ClosureChecker::paramAgrees(const Param &param, const Type *passed, const Type *given) {
  if (given && passed != given && !passed->isError() && !given->isError() && !param.rest()) {
    std::string message =
        ErrorTextParamAgrees::declared(param.label(), passed->name(), given->name());

    return err(ClosureCheckerParamAgreesError{
        .code = ClosureCheckerParamAgreesErrorCode::DECLARED_DIFFERENTLY,
        .message = message,
    });
  }

  return ok(passed);
}

ClosureChecker::ResultDefaultMatters ClosureChecker::defaultMatters(const Param &param,
                                                                    const Type *given) {
  if (param.defaultValue() && !given->isOptional()) {
    std::string message = ErrorTextDefaultMatters::unused(param.label());

    return err(ClosureCheckerDefaultMattersError{
        .code = ClosureCheckerDefaultMattersErrorCode::DEFAULT_NEVER_USED,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultParamTypeKnown ClosureChecker::paramTypeKnown(const Param &param) {
  if (!param.type) {
    std::string message = ErrorTextParamTypeKnown::needsType(param.label());

    return err(ClosureCheckerParamTypeKnownError{
        .code = ClosureCheckerParamTypeKnownErrorCode::NEEDS_TYPE,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultThisParamValid ClosureChecker::thisParam(const FunctionDecl *fn,
                                                               const Param &param) {
  if (fn->isArrow()) {
    std::string message = ErrorTextThisParam::arrowThis();

    return err(ClosureCheckerThisParamError{
        .code = ClosureCheckerThisParamErrorCode::ARROW_THIS_PARAMETER,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultRestParamArray ClosureChecker::restParam(const Param &param,
                                                               const Type *type) {
  if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
    std::string message = ErrorTextRestParam::notArray(param.name);

    return err(ClosureCheckerRestParamError{
        .code = ClosureCheckerRestParamErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

ClosureChecker::ResultParamOrderGood ClosureChecker::paramOrder(const Param &param,
                                                                bool afterOptional) {
  if (afterOptional) {
    std::string message = ErrorTextParamOrder::order();

    return err(ClosureCheckerParamOrderError{
        .code = ClosureCheckerParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureChecker::ResultPromiseAwaited ClosureChecker::promiseAwaited(const Checker::Signature *sig) {
  if (!sig->asyncVoid) {
    std::string message = ErrorTextPromiseAwaited::dropped();

    return err(ClosureCheckerPromiseAwaitedError{
        .code = ClosureCheckerPromiseAwaitedErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(sig);
}

Expr *ClosureChecker::checkFunctionExpr(FunctionExpr *fe, const Checker::Signature *sig) {
  FunctionDecl *fn = fe->fn;
  FuncSymbol *sym = this->checker.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->parent = this->checker.currentDecl();
  fn->creationSeq = ++this->checker.seq_;
  ResultExprNotGeneric plain = ClosureChecker::notGeneric(fn);
  bool generic = plain.isErr();
  if (generic) {
    this->checker.diags_.error(fe->loc(), plain.error().message);
    fn->typeArgs.assign(fn->typeParams.size(), Type::error());
    this->checker.typeScopes_.emplace_back();
    this->checker.typeParams().bindTypeParams(fn->typeParams, fn->typeArgs);
  }

  bool thisFromContext = false;
  if (sig && sig->hasThis && !sig->params.empty() &&
      (fn->params.empty() || fn->params[0].name != "this" || fn->isArrow())) {
    Param self;
    self.name = fn->isArrow() ? this->checker.ctx_.hiddenName() : llvm::StringRef("this");
    self.loc = fe->loc();
    fn->params.insert(fn->params.begin(), self);
    thisFromContext = true;
  }

  size_t named = fn->params.size();
  bool afterOptional = false;
  for (size_t i = 0; i < named; ++i) {
    Param &param = fn->params[i];
    const Type *given = sig && i < sig->params.size() ? sig->params[i] : nullptr;
    bool mayOmit = param.optional() || param.defaultValue();
    const Type *type;
    const Type *passed;
    if (param.type) {
      bool outerList = std::exchange(this->checker.listContext_, param.rest());
      type = this->checker.typeResolver().resolveType(*param.type, false);
      this->checker.listContext_ = outerList;
      if (param.optional()) {
        type = Type::optional(type);
      }
      passed = mayOmit ? Type::optional(type) : type;
      ResultParamTypeAgree agrees = ClosureChecker::paramAgrees(param, passed, given);
      if (agrees.isErr()) {
        this->checker.diags_.error(param.loc, agrees.error().message);
        passed = given;
        type = param.defaultValue() ? given->nonOptional() : given;
      }
    } else if (given) {
      passed = given;
      type = param.defaultValue() ? given->nonOptional() : given;
      ResultDefaultMatters matters = ClosureChecker::defaultMatters(param, given);
      if (matters.isErr()) {
        this->checker.diags_.error(param.defaultValue()->loc(), matters.error().message);
      }
    } else if (param.defaultValue() && (type = literalType(param.defaultValue()))) {
      passed = Type::optional(type);
    } else {
      ResultParamTypeKnown known = ClosureChecker::paramTypeKnown(param);
      if (known.isErr()) {
        this->checker.diags_.error(param.loc, known.error().message);
      }
      type = passed = Type::error();
    }
    if (i == 0 && thisFromContext) {
      sym->hasThis = true;
    } else if (i == 0 && param.name == "this" && param.type && !param.rest()) {
      ResultThisParamValid valid = ClosureChecker::thisParam(fn, param);
      if (valid.isErr()) {
        this->checker.diags_.error(param.loc, valid.error().message);
      } else {
        sym->hasThis = !mayOmit;
      }
    }
    if (param.rest()) {
      ResultRestParamArray array = ClosureChecker::restParam(param, type);
      if (array.isErr()) {
        this->checker.diags_.error(param.loc, array.error().message);
      }
      sym->hasRest = true;
      passed = type;
    } else if (!mayOmit) {
      ResultParamOrderGood order = ClosureChecker::paramOrder(param, afterOptional);
      if (order.isErr()) {
        this->checker.diags_.error(param.loc, order.error().message);
      }
      sym->minArgs = i + 1;
    }
    afterOptional |= mayOmit;
    param.declaredType = type;
    sym->params.push_back(passed);
  }
  named = fn->params.size();
  if (sig && sig->pad && !sym->hasRest) {
    for (size_t i = named; i < sig->params.size(); ++i) {
      Param hidden;
      hidden.name = this->checker.ctx_.hiddenName();
      hidden.loc = fe->loc();
      hidden.declaredType = sig->params[i];
      fn->params.push_back(hidden);
      sym->params.push_back(sig->params[i]);
    }
    sym->minArgs = sig->minArgs;
  }
  this->checker.argListChecker().expandArgumentList(fn, sym);

  if (fn->isGenerator) {
    if (fn->returnType) {
      sym->ret = this->checker.generatorChecker().generatorReturnType(
          fn, this->checker.typeResolver().resolveType(*fn->returnType, true));
    } else if (sig && sig->ret &&
               (fn->isAsync ? sig->ret->isAsyncIterator() : sig->ret->isIterator())) {
      sym->ret = sig->ret;
    }
  } else if (fn->isAsync) {
    if (sig && sig->voidResult) {
      ResultPromiseAwaited awaited = ClosureChecker::promiseAwaited(sig);
      if (awaited.isErr()) {
        this->checker.diags_.error(fe->loc(), awaited.error().message);
      }
      sym->ret = Type::voidTy();
      sym->discardsResult = true;
    } else if (fn->returnType) {
      sym->ret = this->checker.asyncChecker().asyncReturnType(
          fn, this->checker.typeResolver().resolveType(*fn->returnType, true));
    } else if (sig && sig->ret && sig->ret->isPromise()) {
      sym->ret = sig->ret;
    } else if (!ReturnInferrer::returnsValue(fn->body)) {
      sym->ret = Type::promise(Type::voidTy());
    }
  } else if (sig && sig->voidResult) {
    sym->ret = Type::voidTy();
    sym->discardsResult = true;
  } else if (fn->returnType) {
    sym->ret = this->checker.predicateChecker().resolveReturnType(fn, sym);
  } else if (sig && sig->ret) {
    sym->ret = sig->ret;
  } else if (!ReturnInferrer::returnsValue(fn->body)) {
    sym->ret = Type::voidTy();
  }
  if (generic) {
    this->checker.typeScopes_.pop_back();
  }

  if (!sym->ret) {
    llvm::StringSet<> names;
    collectNames(fn->body, names);
    bool recursive = !fn->ownName().empty() && names.contains(fn->ownName());
    for (const auto &entry : names) {
      VarSymbol *var = this->checker.scopes().lookup(entry.getKey()).var();
      recursive |= var && var->lambda() == fn;
    }
    if (recursive) {
      this->checker.cycleOf_[sym] = static_cast<unsigned>(this->checker.cycles_.size());
      this->checker.cycles_.push_back({sym});
      Checker::BodyState saved = this->checker.returnInferrer().enterBody(fn);
      bool ownScope = !fn->ownName().empty();
      if (ownScope) {
        this->declareOwnName(fn);
      }
      this->checker.returnInferrer().inferCycle(this->checker.cycles_.back());
      if (ownScope) {
        this->checker.popScope();
      }
      this->checker.returnInferrer().leaveBody(saved);
    }
  }
  bool outerReady = this->checker.closureThisReady_;
  if (this->checker.initFn_ && this->checker.currentDecl() == this->checker.initFn_) {
    this->checker.closureThisReady_ =
        this->checker.classChecker().thisReady(this->checker.flow_).isOk();
  }
  this->checkClosureBody(fn);
  this->checker.closureThisReady_ = outerReady;
  fe->type = this->checker.returnInferrer().functionType(sym);

  return fe;
}

void ClosureChecker::declareOwnName(FunctionDecl *fn) {
  this->checker.pushScope();
  VarSymbol *self = this->checker.ctx_.newVar();
  self->name = fn->ownName();
  self->loc = fn->loc();
  self->isConst = true;
  self->holds = Lambda{.lambda = fn};
  unsigned declSeq = ++this->checker.seq_;
  self->place = FunctionLocalVar{.owner = fn, .state = {.declSeq = declSeq}};
  Scopes::ResultEntryDeclared added =
      this->checker.scopes().declare(fn->ownName(), fn->loc(), {VarEntry{.var = self}});
  if (added.isErr()) {
    this->checker.diags_.error(fn->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->checker.scopes().lookup(fn->ownName());
    std::string declaredHere = ErrorTextDeclare::previous();
    this->checker.diags_.note(previousEntry.loc(), declaredHere);
  }
}

void ClosureChecker::checkClosureBody(FunctionDecl *fn) {
  Checker::BodyState saved = this->checker.returnInferrer().enterBody(fn);
  auto fixed = [](Term t) {
    return (!t.var || (t.var->isConst && !t.length)) && (!t.var2 || (t.var2->isConst && !t.length2));
  };
  for (const Facts::Fact &f : saved.flow.facts.all()) {
    if (fixed(f.lhs) && fixed(f.rhs)) {
      this->checker.flow_.facts.add(f.lhs, f.rhs, f.offset);
    }
  }
  for (const VarSymbol *var : saved.flow.defined) {
    if (var->isConst) {
      this->checker.flow_.defined.insert(var);
    }
  }
  for (const auto &[var, type] : saved.flow.narrowed) {
    if (var->isConst) {
      this->checker.flow_.narrowed.push_back({var, type});
    }
  }
  for (const VarSymbol *var : saved.flow.okResults) {
    if (var->isConst) {
      this->checker.flow_.okResults.insert(var);
    }
  }
  for (const VarSymbol *var : saved.flow.errResults) {
    if (var->isConst) {
      this->checker.flow_.errResults.insert(var);
    }
  }
  bool ownScope = fn->isExpression() && !fn->ownName().empty();
  if (ownScope) {
    this->declareOwnName(fn);
  }
  if (fn->sym->ret) {
    this->checker.functionChecker().checkFunctionBody(fn);
  } else {
    this->checker.returnInferrer().checkAndInfer(fn);
  }
  if (ownScope) {
    this->checker.popScope();
  }
  this->checker.returnInferrer().leaveBody(saved);
}

void ClosureChecker::checkStatements(BlockStmt *block) {
  this->checker.typeDeclarer().declareTypes(block->body);
  NestedGroup *group = this->checker.nestedFunctions().declareNested(block);
  if (group) {
    this->checker.groups_.push_back(group);
  }
  Checker::BlockNames names{this->checker.functions_.size(), {}};
  for (Stmt *stmt : block->body) {
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt)) {
      for (const VarDeclarator &d : decl->decls) {
        names.names.push_back(d.name);
      }
    }
  }
  this->checker.blockNames_.push_back(std::move(names));
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse) {
      group->creationSeq = ++this->checker.seq_;
      for (FunctionDecl *fn : group->members) {
        if (LocalVarState *state = fn->var->local()) {
          state->declSeq = group->creationSeq;
        }
      }
    }
    this->checker.block_ = block->body;
    this->checker.blockIndex_ = i;
    this->checker.stmtChecker().checkStmt(block->body[i]);
    auto *decided = dyn_cast<IfStmt>(block->body[i]);
    if (this->checker.checkingInstance_ && decided && isa<BoolLitExpr>(decided->cond) &&
        this->checker.flow_.unreachable && (!group || group->firstUse <= i) &&
        llvm::none_of(llvm::ArrayRef<Stmt *>(block->body).drop_front(i + 1),
                      [](const Stmt *s) { return isa<FunctionDecl>(s) || isa<ClassDecl>(s); })) {
      block->body.resize(i + 1);
      break;
    }
  }
  this->checker.blockNames_.pop_back();
  if (group) {
    this->checker.nestedFunctions().finishGroup(group);
    this->checker.groups_.pop_back();
  }
}

}
