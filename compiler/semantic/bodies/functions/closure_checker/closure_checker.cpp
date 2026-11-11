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
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ClosureChecker::ClosureChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->typeParams = &all.getTypeParams();
  this->stmtChecker = &all.getStmtChecker();
  this->asyncChecker = &all.getAsyncChecker();
  this->classChecker = &all.getClassChecker();
  this->typeDeclarer = &all.getTypeDeclarer();
  this->typeResolver = &all.getTypeResolver();
  this->argListChecker = &all.getArgListChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->functionChecker = &all.getFunctionChecker();
  this->nestedFunctions = &all.getNestedFunctions();
  this->generatorChecker = &all.getGeneratorChecker();
  this->predicateChecker = &all.getPredicateChecker();
}

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
  FuncSymbol *sym = this->makeClosureSymbol(fn);
  bool generic = this->reportsGenericClosure(fe);
  ClosureParams params{
      .sym = sym,
      .thisFromContext = this->addsContextThis(fe, sig),
      .afterOptional = false,
  };
  size_t named = fn->params.size();
  for (size_t i = 0; i < named; ++i) {
    this->walkClosureParam(fn, i, sig, params);
  }
  this->padHiddenParams(fe, sym, sig);
  this->argListChecker->expandArgumentList(fn, sym);
  this->walkClosureReturn(fe, sym, sig);
  if (generic) {
    this->state.getScopeStack().closeTypeScope();
  }
  this->inferRecursiveReturn(fn, sym);
  this->walkInitializerBody(fn);
  fe->type = this->returnInferrer->functionType(sym);

  return fe;
}

FuncSymbol *ClosureChecker::makeClosureSymbol(FunctionDecl *fn) {
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->parent = this->state.getFunctionStack().getCurrentDecl();
  fn->creationSeq = this->state.getReturnInference().takeSequenceNumber();

  return sym;
}

bool ClosureChecker::reportsGenericClosure(FunctionExpr *fe) {
  FunctionDecl *fn = fe->fn;
  ResultExprNotGeneric plain = ClosureChecker::notGeneric(fn);
  if (plain.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(fe->loc(), plain.error().message);
  fn->typeArgs.assign(fn->typeParams.size(), Type::error());
  this->state.getScopeStack().openTypeScope();
  this->typeParams->bindTypeParams(fn->typeParams, fn->typeArgs);

  return true;
}

bool ClosureChecker::addsContextThis(FunctionExpr *fe, const Signature *sig) {
  FunctionDecl *fn = fe->fn;
  bool wanted = sig != nullptr && sig->hasThis && !sig->params.empty() &&
                (fn->params.empty() || fn->params[0].name != "this" || fn->isArrow());
  if (!wanted) {
    return false;
  }
  Param self;
  self.name = fn->isArrow() ? this->state.getAst().hiddenName() : llvm::StringRef("this");
  self.loc = fe->loc();
  fn->params.insert(fn->params.begin(), self);

  return true;
}

void ClosureChecker::walkClosureParam(FunctionDecl *fn, size_t i, const Signature *sig,
                                      ClosureParams &params) {
  FuncSymbol *sym = params.sym;
  Param &param = fn->params[i];
  const Type *given = sig != nullptr && i < sig->params.size() ? sig->params[i] : nullptr;
  bool mayOmit = param.optional() || param.defaultValue();
  ParamTypes types = this->closureParamTypes(param, given);
  if (i == 0) {
    this->walkClosureThis(fn, param, mayOmit, params);
  }
  if (param.rest()) {
    ResultRestParamArray array = ClosureChecker::restParam(param, types.type);
    if (array.isErr()) {
      this->state.getDiagnostics().error(param.loc, array.error().message);
    }
    sym->hasRest = true;
    types.passed = types.type;
  } else if (!mayOmit) {
    ResultParamOrderGood order = ClosureChecker::paramOrder(param, params.afterOptional);
    if (order.isErr()) {
      this->state.getDiagnostics().error(param.loc, order.error().message);
    }
    sym->minArgs = i + 1;
  }
  params.afterOptional |= mayOmit;
  param.declaredType = types.type;
  sym->params.push_back(types.passed);
}

ClosureChecker::ParamTypes ClosureChecker::closureParamTypes(const Param &param,
                                                             const Type *given) {
  if (param.type.has_value()) {
    return this->writtenParamTypes(param, given);
  }
  if (given != nullptr) {
    ResultDefaultMatters matters = ClosureChecker::defaultMatters(param, given);
    if (matters.isErr()) {
      this->state.getDiagnostics().error(param.defaultValue()->loc(), matters.error().message);
    }
    return ParamTypes{
        .type = param.defaultValue() ? given->nonOptional() : given,
        .passed = given,
    };
  }
  const Type *literal =
      param.defaultValue() ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return ParamTypes{
        .type = literal,
        .passed = Type::optional(literal),
    };
  }
  ResultParamTypeKnown known = ClosureChecker::paramTypeKnown(param);
  if (known.isErr()) {
    this->state.getDiagnostics().error(param.loc, known.error().message);
  }

  return ParamTypes{
      .type = Type::error(),
      .passed = Type::error(),
  };
}

ClosureChecker::ParamTypes ClosureChecker::writtenParamTypes(const Param &param,
                                                             const Type *given) {
  bool mayOmit = param.optional() || param.defaultValue();
  this->state.getTypeContext().enterListContext(param.rest());
  const Type *type = this->typeResolver->resolveType(*param.type, false);
  this->state.getTypeContext().leaveListContext();
  if (param.optional()) {
    type = Type::optional(type);
  }
  const Type *passed = mayOmit ? Type::optional(type) : type;
  ResultParamTypeAgree agrees = ClosureChecker::paramAgrees(param, passed, given);
  if (agrees.isErr()) {
    this->state.getDiagnostics().error(param.loc, agrees.error().message);
    return ParamTypes{
        .type = param.defaultValue() ? given->nonOptional() : given,
        .passed = given,
    };
  }

  return ParamTypes{
      .type = type,
      .passed = passed,
  };
}

void ClosureChecker::walkClosureThis(const FunctionDecl *fn, const Param &param, bool mayOmit,
                                     ClosureParams &params) {
  if (params.thisFromContext) {
    params.sym->hasThis = true;
    return;
  }
  if (param.name != "this" || !param.type.has_value() || param.rest()) {
    return;
  }
  ResultThisParamValid valid = ClosureChecker::thisParam(fn, param);
  if (valid.isErr()) {
    this->state.getDiagnostics().error(param.loc, valid.error().message);
    return;
  }
  params.sym->hasThis = !mayOmit;
}

void ClosureChecker::padHiddenParams(FunctionExpr *fe, FuncSymbol *sym, const Signature *sig) {
  FunctionDecl *fn = fe->fn;
  if (sig == nullptr || !sig->pad || sym->hasRest) {
    return;
  }
  size_t named = fn->params.size();
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

void ClosureChecker::walkClosureReturn(FunctionExpr *fe, FuncSymbol *sym, const Signature *sig) {
  FunctionDecl *fn = fe->fn;
  if (fn->isGenerator) {
    return this->walkGeneratorReturn(fn, sym, sig);
  }
  if (fn->isAsync) {
    return this->walkAsyncReturn(fe, sym, sig);
  }

  return this->walkPlainReturn(fn, sym, sig);
}

void ClosureChecker::walkGeneratorReturn(FunctionDecl *fn, FuncSymbol *sym, const Signature *sig) {
  if (fn->returnType.has_value()) {
    const Type *declared = this->typeResolver->resolveType(*fn->returnType, true);
    GeneratorChecker::ResultGeneratorReturns generator =
        this->generatorChecker->generatorReturnType(fn, declared);
    if (generator.isErr()) {
      this->state.getDiagnostics().error(fn->returnType->loc, generator.error().message);
    }
    sym->setReturnType(generator.isOk() ? generator.value() : Type::error());
    return;
  }
  bool fits = sig != nullptr && sig->ret != nullptr &&
              (fn->isAsync ? sig->ret->isAsyncIterator() : sig->ret->isIterator());
  if (fits) {
    sym->setReturnType(sig->ret);
  }
}

void ClosureChecker::walkAsyncReturn(FunctionExpr *fe, FuncSymbol *sym, const Signature *sig) {
  FunctionDecl *fn = fe->fn;
  if (sig != nullptr && sig->voidResult) {
    ResultPromiseAwaited awaited = ClosureChecker::promiseAwaited(sig);
    if (awaited.isErr()) {
      this->state.getDiagnostics().error(fe->loc(), awaited.error().message);
    }
    sym->setReturnType(Type::voidTy());
    sym->discardsResult = true;
    return;
  }
  if (fn->returnType.has_value()) {
    const Type *declared = this->typeResolver->resolveType(*fn->returnType, true);
    const Type *returned = this->asyncChecker->asyncReturnType(fn, declared);
    sym->setReturnType(returned);
    return;
  }
  if (sig != nullptr && sig->ret != nullptr && sig->ret->isPromise()) {
    sym->setReturnType(sig->ret);
    return;
  }
  if (!ReturnInferrer::returnsValue(fn->body)) {
    sym->setReturnType(Type::promise(Type::voidTy()));
  }
}

void ClosureChecker::walkPlainReturn(FunctionDecl *fn, FuncSymbol *sym, const Signature *sig) {
  if (sig != nullptr && sig->voidResult) {
    sym->setReturnType(Type::voidTy());
    sym->discardsResult = true;
    return;
  }
  if (fn->returnType.has_value()) {
    sym->setReturnType(this->predicateChecker->resolveReturnType(fn, sym));
    return;
  }
  if (sig != nullptr && sig->ret != nullptr) {
    sym->setReturnType(sig->ret);
    return;
  }
  if (!ReturnInferrer::returnsValue(fn->body)) {
    sym->setReturnType(Type::voidTy());
  }
}

void ClosureChecker::inferRecursiveReturn(FunctionDecl *fn, FuncSymbol *sym) {
  if (sym->hasReturnType() || !this->isRecursiveClosure(fn)) {
    return;
  }
  this->state.getReturnInference().addCycle({sym});
  BodyState saved = this->returnInferrer->enterBody(fn);
  bool ownScope = !fn->ownName().empty();
  if (ownScope) {
    this->declareOwnName(fn);
  }
  this->returnInferrer->inferCycle(this->state.getReturnInference().getCycleOf(sym));
  if (ownScope) {
    this->scopes->closeScope();
  }
  this->returnInferrer->leaveBody(saved);
}

bool ClosureChecker::isRecursiveClosure(FunctionDecl *fn) {
  llvm::StringSet<> names;
  AstWalker::collectNames(fn->body, names);
  bool recursive = !fn->ownName().empty() && names.contains(fn->ownName());
  for (const auto &entry : names) {
    VarSymbol *var = this->scopes->lookup(entry.getKey()).var();
    recursive |= var != nullptr && var->lambda() == fn;
  }

  return recursive;
}

void ClosureChecker::walkInitializerBody(FunctionDecl *fn) {
  ClassContext &classContext = this->state.getClassContext();
  FunctionDecl *initializer = classContext.getInitializer();
  bool inInitializer =
      initializer != nullptr && this->state.getFunctionStack().getCurrentDecl() == initializer;
  if (!inInitializer) {
    return this->checkClosureBody(fn);
  }
  bool thisReady = this->classChecker->thisReady(this->state.getFlowState().getFlow()).isOk();
  classContext.enterInitializerClosure(thisReady);
  this->checkClosureBody(fn);
  classContext.leaveInitializerClosure();
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
  Scopes::ResultEntryDeclared added = this->scopes->declare(fn->ownName(), {VarEntry{.var = self}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->scopes->lookup(fn->ownName());
    std::string declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
}

void ClosureChecker::checkClosureBody(FunctionDecl *fn) {
  BodyState saved = this->returnInferrer->enterBody(fn);
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
    this->functionChecker->checkFunctionBody(fn);
  } else {
    this->returnInferrer->checkAndInfer(fn);
  }
  if (ownScope) {
    this->scopes->closeScope();
  }
  this->returnInferrer->leaveBody(saved);
}

void ClosureChecker::checkStatements(BlockStmt *block) {
  this->typeDeclarer->declareTypes(block->body);
  NestedGroup *group = this->nestedFunctions->declareNested(block);
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
    this->stmtChecker->checkStmt(block->body[i]);
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
    this->nestedFunctions->finishGroup(group);
    this->state.getFunctionStack().leaveGroup();
  }
}

}
