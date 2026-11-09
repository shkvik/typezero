#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

FunctionChecker::FunctionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->byteProofs = &all.getByteProofs();
  this->chunkTypes = &all.getChunkTypes();
  this->typeParams = &all.getTypeParams();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->indexProofs = &all.getIndexProofs();
  this->lateClasses = &all.getLateClasses();
  this->asyncChecker = &all.getAsyncChecker();
  this->classChecker = &all.getClassChecker();
  this->typeResolver = &all.getTypeResolver();
  this->typeRelations = &all.getTypeRelations();
  this->argListChecker = &all.getArgListChecker();
  this->closureChecker = &all.getClosureChecker();
  this->emitterChecker = &all.getEmitterChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->iterableChecker = &all.getIterableChecker();
  this->generatorChecker = &all.getGeneratorChecker();
  this->predicateChecker = &all.getPredicateChecker();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->genericClassChecker = &all.getGenericClassChecker();
}

FunctionChecker::ResultTopLevelGenerics FunctionChecker::topLevelGeneric(const FunctionDecl *fn) {
  if (!fn->typeParams.empty()) {
    std::string message = ErrorTextFunctionDecl::nested();

    return err(FunctionCheckerTopLevelGenericError{
        .code = FunctionCheckerTopLevelGenericErrorCode::NESTED_GENERIC,
        .message = message,
    });
  }

  return ok(fn);
}

FunctionChecker::ResultDecoratorAllowed FunctionChecker::paramDecorators(const FunctionDecl *fn,
                                                                         const Param &param) {
  bool classMethod = fn->cls() && !fn->cls()->literal();
  if (!param.decorators.empty() && !classMethod && !fn->isInstance) {
    std::string message = ErrorTextFunctionDecl::decorator();

    return err(FunctionCheckerParamDecoratorsError{
        .code = FunctionCheckerParamDecoratorsErrorCode::DECORATOR_NOT_ALLOWED,
        .message = message,
    });
  }

  return ok(&param);
}

FunctionChecker::ResultThisParamAllowed FunctionChecker::thisParam(const FunctionDecl *fn) {
  if (fn->isArrow()) {
    std::string message = ErrorTextFunctionDecl::arrowThis();

    return err(FunctionCheckerThisParamError{
        .code = FunctionCheckerThisParamErrorCode::ARROW_THIS,
        .message = message,
    });
  }
  if (fn->cls()) {
    std::string message = ErrorTextFunctionDecl::methodThis();

    return err(FunctionCheckerThisParamError{
        .code = FunctionCheckerThisParamErrorCode::METHOD_THIS,
        .message = message,
    });
  }

  return ok(fn);
}

FunctionChecker::ResultRestParamIsArray FunctionChecker::restType(const Type *type,
                                                                  const Param &param) {
  bool list = type->isArray() || type->isListParam() || type->isTuple() || type->isError();
  if (!list) {
    std::string message = ErrorTextFunctionDecl::restArray(param.name);

    return err(FunctionCheckerRestTypeError{
        .code = FunctionCheckerRestTypeErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

FunctionChecker::ResultParamOrderIsKept FunctionChecker::requiredOrder(const Param &param,
                                                                       bool afterOptional) {
  if (afterOptional) {
    std::string message = ErrorTextFunctionDecl::required();

    return err(FunctionCheckerRequiredOrderError{
        .code = FunctionCheckerRequiredOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

void FunctionChecker::declareFunction(FunctionDecl *fn, VarSymbol *var, bool scoped) {
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  bool generic = !fn->typeParams.empty() || llvm::any_of(fn->params, [&](const Param &p) {
    return p.type && this->iterableChecker->isIterableRef(*p.type);
  });
  bool classParams = fn->cls() && !fn->isStatic() && !fn->cls()->typeParams.empty();
  if (generic || classParams) {
    this->state.getScopeStack().openTypeScope();
    if (classParams) {
      this->genericClassChecker->bindClassTypeParams(fn->cls());
    }
  }
  if (generic) {
    if (var) {
      ResultTopLevelGenerics topLevel = FunctionChecker::topLevelGeneric(fn);
      if (topLevel.isErr()) {
        this->state.getDiagnostics().error(fn->loc(), topLevel.error().message);
      }
      fn->typeArgs.assign(fn->typeParams.size(), Type::error());
      this->typeParams->bindTypeParams(fn->typeParams, fn->typeArgs);
    } else {
      fn->typeArgs = sym->typeParams =
          this->typeParams->declareTypeParams(fn->typeParams, &sym->typeDefaults);
    }
  }
  std::optional<std::vector<const Type *>> hook =
      fn->cls() ? this->chunkTypes->streamHookParams(fn) : std::nullopt;
  if (hook && llvm::none_of(fn->params, [](const Param &p) { return p.rest(); })) {
    for (size_t i = fn->params.size(); i < hook->size(); ++i) {
      Param hidden;
      hidden.name = this->state.getAst().hiddenName();
      hidden.loc = fn->loc();
      fn->params.push_back(hidden);
    }
  }
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    ResultDecoratorAllowed decorated = FunctionChecker::paramDecorators(fn, param);
    if (decorated.isErr()) {
      this->state.getDiagnostics().error(param.decorators[0].loc, decorated.error().message);
    }
    const Type *type = nullptr;
    if (param.type) {
      IterableChecker::ResultIterableParam iterable =
          this->iterableChecker->iterableParam(fn, sym, *param.type);
      if (iterable.isOk()) {
        type = iterable.value();
      } else if (iterable.error().code != IterableCheckerIterableParamErrorCode::NOT_ITERABLE) {
        this->state.getDiagnostics().error(param.type->loc, iterable.error().message);
        type = Type::error();
      }
      this->state.getTypeContext().enterListContext(param.rest());
      if (!type) {
        type = this->typeResolver->resolveType(*param.type, false);
      }
      this->state.getTypeContext().leaveListContext();
      if (hook && i < hook->size() && type && type->isFloat() && (*hook)[i]->isInt()) {
        type = (*hook)[i];
      }
    } else if (hook && i < hook->size() && !param.rest()) {
      type = (*hook)[i];
    } else if (param.defaultValue() && (type = SharedHelpers::literalType(param.defaultValue()))) {
    } else if (param.defaultValue() && !param.rest() && !fn->source.empty()) {
      param.declaredType = nullptr;
      sym->params.push_back(nullptr);
      sym->pendingParams = true;
      continue;
    } else {
      std::string message = ErrorTextFunctionDecl::needsType(param.label());
      this->state.getDiagnostics().error(param.loc, message);
      type = Type::error();
    }
    if (param.optional()) {
      type = Type::optional(type);
    }
    param.declaredType = type;
    if (i == 0 && param.name == "this" && !param.rest() && param.type) {
      ResultThisParamAllowed thisAllowed = FunctionChecker::thisParam(fn);
      if (thisAllowed.isErr()) {
        this->state.getDiagnostics().error(param.loc, thisAllowed.error().message);
      } else {
        sym->hasThis = !param.optional() && !param.defaultValue();
      }
    }
    if (param.rest()) {
      ResultRestParamIsArray rest = FunctionChecker::restType(type, param);
      if (rest.isErr()) {
        this->state.getDiagnostics().error(param.loc, rest.error().message);
        param.declaredType = Type::error();
      }
      sym->hasRest = true;
      sym->params.push_back(param.declaredType);
      continue;
    }
    bool mayOmit = param.optional() || param.defaultValue();
    sym->params.push_back(mayOmit ? Type::optional(type) : type);
    if (!mayOmit) {
      ResultParamOrderIsKept order = FunctionChecker::requiredOrder(param, afterOptional);
      if (order.isErr()) {
        this->state.getDiagnostics().error(param.loc, order.error().message);
      }
      sym->minArgs = i + 1;
    }
    afterOptional |= param.optional();
  }
  if (fn->returnType) {
    const Type *declared = this->predicateChecker->resolveReturnType(fn, sym);
    GeneratorChecker::ResultGeneratorReturns generator =
        this->generatorChecker->generatorReturnType(fn, declared);
    if (generator.isErr()) {
      this->state.getDiagnostics().error(fn->returnType->loc, generator.error().message);
    }
    const Type *returned = generator.isOk() ? generator.value() : Type::error();
    sym->setReturnType(this->asyncChecker->asyncReturnType(fn, returned));
  } else if (!ReturnInferrer::returnsValue(fn->body) && fn->isAsync && !fn->isGenerator) {
    sym->setReturnType(Type::promise(Type::voidTy()));
  } else if (!ReturnInferrer::returnsValue(fn->body) && !fn->isGenerator) {
    sym->setReturnType(Type::voidTy());
  }
  if (generic || classParams) {
    this->state.getScopeStack().closeTypeScope();
  }
  this->argListChecker->expandArgumentList(fn, sym);
  fn->sym = sym;
  if (!fn->name.empty() && scoped) {
    Scopes::ResultEntryDeclared added =
        this->scopes->declare(fn->name, {FunctionEntry{.func = sym, .var = var}});
    if (added.isErr()) {
      this->state.getDiagnostics().error(fn->loc(), added.error().message);
    }
    if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
      ScopeEntry previousEntry = this->scopes->lookup(fn->name);
      std::string declaredHere = ErrorTextDeclare::previous();
      this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
    }
  }
}

FunctionChecker::ResultConstructorSuper FunctionChecker::superCalled(const FunctionDecl *fn,
                                                                     unsigned supers) {
  if (supers == 0) {
    std::string message = ErrorTextFunctionBody::noSuper(fn->cls()->name, fn->cls()->parent->name);

    return err(FunctionCheckerSuperCalledError{
        .code = FunctionCheckerSuperCalledErrorCode::NO_SUPER_CALL,
        .message = message,
    });
  }

  return ok(fn);
}

FunctionChecker::ResultDefaultUsesPrior
FunctionChecker::defaultOrder(const FunctionDecl *fn, size_t i, const llvm::StringSet<> &names) {
  for (size_t j = i; j < fn->params.size(); ++j) {
    if (names.contains(fn->params[j].name)) {
      std::string message =
          ErrorTextFunctionBody::forward(fn->params[i].label(), fn->params[j].name);

      return err(FunctionCheckerDefaultOrderError{
          .code = FunctionCheckerDefaultOrderErrorCode::FORWARD_REFERENCE,
          .message = message,
      });
    }
  }

  return ok(fn);
}

FunctionChecker::ResultEveryPathReturns FunctionChecker::returnsEverywhere(const FunctionDecl *fn,
                                                                           const Type *ret) {
  bool missing = ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() &&
                 !ret->isUnknown() && !ReturnChecker::alwaysReturns(fn->body);
  if (missing) {
    std::string label = ErrorTextFunctionName::label(fn->name);
    std::string message = ErrorTextFunctionBody::noReturn(label, ret->name());

    return err(FunctionCheckerReturnsEverywhereError{
        .code = FunctionCheckerReturnsEverywhereErrorCode::MISSING_RETURN,
        .message = message,
    });
  }

  return ok(fn);
}

void FunctionChecker::checkFunctionBody(FunctionDecl *fn) {
  if (fn->intrinsic()) {
    fn->checked = true;
    return;
  }
  if (fn->external()) {
    this->tzModulesChecker->checkExternal(fn);
    return;
  }
  FuncSymbol *sym = fn->sym;
  this->returnInferrer->resolveParams(sym);
  fn->checked = true;
  FunctionStack &functions = this->state.getFunctionStack();
  FuncSymbol *outer = functions.getCurrentFunction();
  functions.rememberCurrentFunction(sym);
  functions.enterFunction(fn);
  unsigned errors = this->state.getDiagnostics().errorCount();
  ClassContext &classContext = this->state.getClassContext();
  classContext.enterClass(fn->isClosure() ? classContext.getCurrentClass() : fn->cls());
  bool initializer = false;
  this->state.getScopeStack().openScope();
  if (fn->cls() && !fn->isStatic()) {
    this->genericClassChecker->bindClassTypeParams(fn->cls());
  }
  if (fn->cls()) {
    this->lateClasses->bindOuterTypeParams(fn->cls());
  }
  if (!fn->typeArgs.empty()) {
    this->typeParams->bindTypeParams(fn->typeParams, fn->typeArgs);
  }
  if (fn->takesThis()) {
    ClassChecker::ResultNewThisSymbol declared = this->classChecker->declareThis(fn);
    if (declared.isErr()) {
      this->state.getDiagnostics().error(fn->loc(), declared.error().message);
    }
    VarSymbol *self = declared.isOk() ? declared.value() : nullptr;
    if (fn->isConstructor() || fn == fn->cls()->init) {
      classContext.enterInitializer(fn, self);
      initializer = true;
      this->state.getFlowState().forgetSetFields();
      bool derived = fn->cls()->parent != nullptr;
      for (const ClassField &f : fn->cls()->fields) {
        if (!f.isStatic() &&
            (f.optional() || (fn->isConstructor() && !derived && f.init && !f.method()) ||
             (!fn->isConstructor() && f.redeclared))) {
          this->state.getFlowState().addSetField(f.name);
        }
      }
      if (derived && fn->isConstructor()) {
        this->state.getFlowState().markBeforeSuper();
      } else {
        this->state.getFlowState().markSuperCalled();
      }
      if (this->state.getFlowState().isBeforeSuper()) {
        unsigned supers = 0;
        for (Stmt *stmt : fn->body->body) {
          auto *expr = dyn_cast<ExprStmt>(stmt);
          auto *call = expr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
          auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
          if (callee && callee->name == "super") {
            this->state.getClassStore().markRootSuper(call);
            ++supers;
          }
        }
        ResultConstructorSuper called = FunctionChecker::superCalled(fn, supers);
        if (called.isErr()) {
          this->state.getDiagnostics().error(fn->loc(), called.error().message);
        }
      }
    }
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *type = param.declaredType;
    if (param.defaultValue() && type->isError() && !param.type) {
      param.defaultValue()->type = type;
      param.sym = this->scopes->declareVar(param.name, param.loc, type, false);
      continue;
    }
    if (DefaultedParam *defaulted = param.form.as<DefaultedParam>()) {
      llvm::StringSet<> names;
      AstWalker::collectNames(defaulted->defaultValue, names);
      ResultDefaultUsesPrior prior = FunctionChecker::defaultOrder(fn, i, names);
      if (prior.isErr()) {
        this->state.getDiagnostics().error(defaulted->defaultValue->loc(), prior.error().message);
        defaulted->defaultValue->type = Type::error();
        param.sym = this->scopes->declareVar(param.name, param.loc, type, false);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(defaulted->defaultValue); fe && fe->fn->name.empty()) {
        fe->fn->name = param.name;
      }
      Expr *checked =
          this->exprChecker->checkExpr(defaulted->defaultValue, type->isError() ? nullptr : type);
      defaulted->defaultValue = this->typeRelations->coerce(checked, type);
    }
    param.sym = this->scopes->declareVar(param.name, param.loc, type, false);
    if (param.defaultValue() && type->isOptional() && !param.defaultValue()->type->isOptional() &&
        !param.defaultValue()->type->isUndefined()) {
      this->flowChecker->setDefined(param.sym, true);
    }
  }
  this->byteProofs->addByteCallbackFacts(fn);
  this->indexProofs->addIndexCallbackFacts(fn);
  this->argListChecker->declareArgumentList(fn);
  this->predicateChecker->beginPredicate(fn);
  this->state.getFlowState().enterErrorWatch();
  this->closureChecker->checkStatements(fn->body);
  if (!this->state.getFlowState().isUnreachable()) {
    this->emitterChecker->reportUnlistenedErrors();
  }
  this->state.getFlowState().leaveErrorWatch();
  this->predicateChecker->endPredicate(fn);
  if (fn->isConstructor() && classContext.getInitializer() == fn &&
      !this->state.getFlowState().isBeforeSuper()) {
    ClassChecker::ResultEveryFieldSet fieldsSet = this->classChecker->checkFieldsSet();
    if (fieldsSet.isErr()) {
      this->state.getDiagnostics().error(fn->loc(), fieldsSet.error().message);
    }
  }
  this->scopes->closeScope();
  functions.leaveFunction();
  functions.rememberCurrentFunction(outer);
  if (initializer) {
    classContext.leaveInitializer();
  }
  classContext.leaveClass();

  const Type *ret = fn->isAsync && sym->hasReturnType() && sym->getKnownReturnType()->isPromise()
                        ? sym->getKnownReturnType()->promised()
                    : !fn->isGenerator ? sym->getKnownReturnType()
                    : sym->hasReturnType() && (sym->getKnownReturnType()->isIterator() ||
                                               sym->getKnownReturnType()->isAsyncIterator())
                        ? sym->getKnownReturnType()->returned()
                        : nullptr;
  ResultEveryPathReturns returns = FunctionChecker::returnsEverywhere(fn, ret);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), returns.error().message);
  }
  if (sym->isGeneric() && this->state.getDiagnostics().errorCount() > errors) {
    this->state.getGenericStore().markBrokenGeneric(sym);
  }
  if (fn->cls() && fn->cls()->isGeneric() && this->state.getDiagnostics().errorCount() > errors) {
    this->state.getClassStore().markBrokenClass(fn->cls());
  }
}

void FunctionChecker::walkFunctionFromScratch(FunctionDecl *fn) {
  this->state.getFlowState().replaceFlow(Flow());
  this->checkFunctionBody(fn);
}

}
