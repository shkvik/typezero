#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ClosureChecker::ClosureChecker(SemanticState &owner)
    : state(owner) {}

Signature ClosureChecker::signatureOf(const Type *fnType) {
  Signature sig;
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

ClosureChecker::ResultPromiseAwaited ClosureChecker::promiseAwaited(const Signature *sig) {
  if (!sig->asyncVoid) {
    std::string message = ErrorTextPromiseAwaited::dropped();

    return err(ClosureCheckerPromiseAwaitedError{
        .code = ClosureCheckerPromiseAwaitedErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(sig);
}

Expr *ClosureChecker::checkFunctionExpr(FunctionExpr *fe, const Signature *sig) {
  FunctionDecl *fn = fe->fn;
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->parent = this->state.getFunctionStack().getCurrentDecl();
  fn->creationSeq = this->state.getReturnInference().takeSequenceNumber();
  ResultExprNotGeneric plain = ClosureChecker::notGeneric(fn);
  bool generic = plain.isErr();
  if (generic) {
    this->state.getDiagnostics().error(fe->loc(), plain.error().message);
    fn->typeArgs.assign(fn->typeParams.size(), Type::error());
    this->state.getScopeStack().openTypeScope();
    TypeParams(this->state).bindTypeParams(fn->typeParams, fn->typeArgs);
  }

  bool thisFromContext = false;
  if (sig && sig->hasThis && !sig->params.empty() &&
      (fn->params.empty() || fn->params[0].name != "this" || fn->isArrow())) {
    Param self;
    self.name = fn->isArrow() ? this->state.getAst().hiddenName() : llvm::StringRef("this");
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
      this->state.getTypeContext().enterListContext(param.rest());
      type = TypeResolver(this->state).resolveType(*param.type, false);
      this->state.getTypeContext().leaveListContext();
      if (param.optional()) {
        type = Type::optional(type);
      }
      passed = mayOmit ? Type::optional(type) : type;
      ResultParamTypeAgree agrees = ClosureChecker::paramAgrees(param, passed, given);
      if (agrees.isErr()) {
        this->state.getDiagnostics().error(param.loc, agrees.error().message);
        passed = given;
        type = param.defaultValue() ? given->nonOptional() : given;
      }
    } else if (given) {
      passed = given;
      type = param.defaultValue() ? given->nonOptional() : given;
      ResultDefaultMatters matters = ClosureChecker::defaultMatters(param, given);
      if (matters.isErr()) {
        this->state.getDiagnostics().error(param.defaultValue()->loc(), matters.error().message);
      }
    } else if (param.defaultValue() && (type = SharedHelpers::literalType(param.defaultValue()))) {
      passed = Type::optional(type);
    } else {
      ResultParamTypeKnown known = ClosureChecker::paramTypeKnown(param);
      if (known.isErr()) {
        this->state.getDiagnostics().error(param.loc, known.error().message);
      }
      type = passed = Type::error();
    }
    if (i == 0 && thisFromContext) {
      sym->hasThis = true;
    } else if (i == 0 && param.name == "this" && param.type && !param.rest()) {
      ResultThisParamValid valid = ClosureChecker::thisParam(fn, param);
      if (valid.isErr()) {
        this->state.getDiagnostics().error(param.loc, valid.error().message);
      } else {
        sym->hasThis = !mayOmit;
      }
    }
    if (param.rest()) {
      ResultRestParamArray array = ClosureChecker::restParam(param, type);
      if (array.isErr()) {
        this->state.getDiagnostics().error(param.loc, array.error().message);
      }
      sym->hasRest = true;
      passed = type;
    } else if (!mayOmit) {
      ResultParamOrderGood order = ClosureChecker::paramOrder(param, afterOptional);
      if (order.isErr()) {
        this->state.getDiagnostics().error(param.loc, order.error().message);
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
      hidden.name = this->state.getAst().hiddenName();
      hidden.loc = fe->loc();
      hidden.declaredType = sig->params[i];
      fn->params.push_back(hidden);
      sym->params.push_back(sig->params[i]);
    }
    sym->minArgs = sig->minArgs;
  }
  ArgListChecker(this->state).expandArgumentList(fn, sym);

  if (fn->isGenerator) {
    if (fn->returnType) {
      const Type *declared = TypeResolver(this->state).resolveType(*fn->returnType, true);
      GeneratorChecker::ResultGeneratorReturns generator =
          GeneratorChecker(this->state).generatorReturnType(fn, declared);
      if (generator.isErr()) {
        this->state.getDiagnostics().error(fn->returnType->loc, generator.error().message);
      }
      sym->setReturnType(generator.isOk() ? generator.value() : Type::error());
    } else if (sig && sig->ret &&
               (fn->isAsync ? sig->ret->isAsyncIterator() : sig->ret->isIterator())) {
      sym->setReturnType(sig->ret);
    }
  } else if (fn->isAsync) {
    if (sig && sig->voidResult) {
      ResultPromiseAwaited awaited = ClosureChecker::promiseAwaited(sig);
      if (awaited.isErr()) {
        this->state.getDiagnostics().error(fe->loc(), awaited.error().message);
      }
      sym->setReturnType(Type::voidTy());
      sym->discardsResult = true;
    } else if (fn->returnType) {
      const Type *declared = TypeResolver(this->state).resolveType(*fn->returnType, true);
      const Type *returned = AsyncChecker(this->state).asyncReturnType(fn, declared);
      sym->setReturnType(returned);
    } else if (sig && sig->ret && sig->ret->isPromise()) {
      sym->setReturnType(sig->ret);
    } else if (!ReturnInferrer::returnsValue(fn->body)) {
      sym->setReturnType(Type::promise(Type::voidTy()));
    }
  } else if (sig && sig->voidResult) {
    sym->setReturnType(Type::voidTy());
    sym->discardsResult = true;
  } else if (fn->returnType) {
    sym->setReturnType(PredicateChecker(this->state).resolveReturnType(fn, sym));
  } else if (sig && sig->ret) {
    sym->setReturnType(sig->ret);
  } else if (!ReturnInferrer::returnsValue(fn->body)) {
    sym->setReturnType(Type::voidTy());
  }
  if (generic) {
    this->state.getScopeStack().closeTypeScope();
  }

  if (!sym->hasReturnType()) {
    llvm::StringSet<> names;
    AstWalker::collectNames(fn->body, names);
    bool recursive = !fn->ownName().empty() && names.contains(fn->ownName());
    for (const auto &entry : names) {
      VarSymbol *var = Scopes(this->state).lookup(entry.getKey()).var();
      recursive |= var && var->lambda() == fn;
    }
    if (recursive) {
      this->state.getReturnInference().addCycle({sym});
      BodyState saved = ReturnInferrer(this->state).enterBody(fn);
      bool ownScope = !fn->ownName().empty();
      if (ownScope) {
        this->declareOwnName(fn);
      }
      ReturnInferrer(this->state).inferCycle(this->state.getReturnInference().getCycleOf(sym));
      if (ownScope) {
        Scopes(this->state).closeScope();
      }
      ReturnInferrer(this->state).leaveBody(saved);
    }
  }
  ClassContext &classContext = this->state.getClassContext();
  FunctionDecl *initializer = classContext.getInitializer();
  bool inInitializer =
      initializer && this->state.getFunctionStack().getCurrentDecl() == initializer;
  if (inInitializer) {
    bool thisReady =
        ClassChecker(this->state).thisReady(this->state.getFlowState().getFlow()).isOk();
    classContext.enterInitializerClosure(thisReady);
  }
  this->checkClosureBody(fn);
  if (inInitializer) {
    classContext.leaveInitializerClosure();
  }
  fe->type = ReturnInferrer(this->state).functionType(sym);

  return fe;
}

void ClosureChecker::declareOwnName(FunctionDecl *fn) {
  this->state.getScopeStack().openScope();
  VarSymbol *self = this->state.getAst().newVar();
  self->name = fn->ownName();
  self->loc = fn->loc();
  self->isConst = true;
  self->holds = Lambda{.lambda = fn};
  unsigned declSeq = this->state.getReturnInference().takeSequenceNumber();
  self->place = FunctionLocalVar{.owner = fn, .state = {.declSeq = declSeq}};
  Scopes::ResultEntryDeclared added =
      Scopes(this->state).declare(fn->ownName(), {VarEntry{.var = self}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = Scopes(this->state).lookup(fn->ownName());
    std::string declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
}

void ClosureChecker::checkClosureBody(FunctionDecl *fn) {
  BodyState saved = ReturnInferrer(this->state).enterBody(fn);
  auto fixed = [](Term t) {
    return (!t.var || (t.var->isConst && !t.length)) && (!t.var2 || (t.var2->isConst && !t.length2));
  };
  for (const Facts::Fact &f : saved.flow.facts.all()) {
    if (fixed(f.lhs) && fixed(f.rhs)) {
      this->state.getFlowState().getFacts().add(f.lhs, f.rhs, f.offset);
    }
  }
  for (const VarSymbol *var : saved.flow.defined) {
    if (var->isConst) {
      this->state.getFlowState().markDefined(var);
    }
  }
  for (const auto &[var, type] : saved.flow.narrowed) {
    if (var->isConst) {
      this->state.getFlowState().addNarrowed(var, type);
    }
  }
  for (const VarSymbol *var : saved.flow.okResults) {
    if (var->isConst) {
      this->state.getFlowState().markOkResult(var);
    }
  }
  for (const VarSymbol *var : saved.flow.errResults) {
    if (var->isConst) {
      this->state.getFlowState().markErrResult(var);
    }
  }
  bool ownScope = fn->isExpression() && !fn->ownName().empty();
  if (ownScope) {
    this->declareOwnName(fn);
  }
  if (fn->sym->hasReturnType()) {
    FunctionChecker(this->state).checkFunctionBody(fn);
  } else {
    ReturnInferrer(this->state).checkAndInfer(fn);
  }
  if (ownScope) {
    Scopes(this->state).closeScope();
  }
  ReturnInferrer(this->state).leaveBody(saved);
}

void ClosureChecker::checkStatements(BlockStmt *block) {
  TypeDeclarer(this->state).declareTypes(block->body);
  NestedGroup *group = NestedFunctions(this->state).declareNested(block);
  if (group) {
    this->state.getFunctionStack().enterGroup(group);
  }
  BlockNames names{this->state.getFunctionStack().getFunctionDepth(), {}};
  for (Stmt *stmt : block->body) {
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt)) {
      for (const VarDeclarator &d : decl->decls) {
        names.names.push_back(d.name);
      }
    }
  }
  this->state.getScopeStack().enterBlockNames(std::move(names));
  for (size_t i = 0; i < block->body.size(); ++i) {
    if (group && i == group->firstUse) {
      group->creationSeq = this->state.getReturnInference().takeSequenceNumber();
      for (FunctionDecl *fn : group->members) {
        if (LocalVarState *localState = fn->var->local()) {
          localState->declSeq = group->creationSeq;
        }
      }
    }
    this->state.getBlockPosition().rememberBlockPosition(block->body, i);
    StmtChecker(this->state).checkStmt(block->body[i]);
    auto *decided = dyn_cast<IfStmt>(block->body[i]);
    if (this->state.getGenericStore().isCheckingInstance() && decided &&
        isa<BoolLitExpr>(decided->cond) && this->state.getFlowState().isUnreachable() &&
        (!group || group->firstUse <= i) &&
        llvm::none_of(llvm::ArrayRef<Stmt *>(block->body).drop_front(i + 1),
                      [](const Stmt *s) { return isa<FunctionDecl>(s) || isa<ClassDecl>(s); })) {
      block->body.resize(i + 1);
      break;
    }
  }
  this->state.getScopeStack().leaveBlockNames();
  if (group) {
    NestedFunctions(this->state).finishGroup(group);
    this->state.getFunctionStack().leaveGroup();
  }
}

}
