#include "compiler/semantic/inference_v2/returns/return_requests/return_requests.h"

namespace tz {

ReturnRequests::ReturnRequests(SemanticState &owner, ReturnRules &sharedReturnRules)
    : state(owner),
      returnRules(sharedReturnRules) {}

void ReturnRequests::inferReturnType(const InferReturnType &task) {
  FuncSymbol *function = task.function;
  auto settled = this->settleReturn(function);
  if (settled.has_value()) {
    return;
  }

  return this->startInference(function);
}

void ReturnRequests::returnTypeOf(const ReturnTypeOf &task) {
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  auto settled = this->settleReturn(function);
  if (settled.has_value()) {
    return tasks.pushAnswer(*settled);
  }
  tasks.pushTask(AnswerReturnType{.function = function});

  return this->startInference(function);
}

void ReturnRequests::answerReturnType(const AnswerReturnType &task) {
  return this->state.walk.tasks.pushAnswer(task.function->getKnownReturnType());
}

void ReturnRequests::functionTypeOf(const FunctionTypeOf &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AnswerFunctionType{.function = task.function});

  return tasks.pushTask(ReturnTypeOf{.function = task.function});
}

void ReturnRequests::answerFunctionType(const AnswerFunctionType &task) {
  const FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = tasks.takeAnswer();
  const Type *returned = returnType != nullptr ? returnType : Type::error();
  const auto *type = Type::function(function->params, function->minArgs, function->hasRest,
                                    returned, function->hasThis);

  return tasks.pushAnswer(type);
}

std::optional<const Type *> ReturnRequests::settleReturn(FuncSymbol *function) {
  if (function->hasReturnType()) {
    return function->getKnownReturnType();
  }
  if (this->state.code.late.isTemplate(function)) {
    return Type::unknown();
  }
  auto early = this->earlyCall(function);
  if (early.isErr()) {
    this->state.diagnostics.error(function->loc, early.error().message);
    this->state.inference.returns.forgetSuspended(function);

    return Type::error();
  }
  if (function->inferring) {
    return function->assumed != nullptr ? function->assumed : Type::error();
  }

  return std::nullopt;
}

ReturnRequests::ResultNotCalledEarly ReturnRequests::earlyCall(const FuncSymbol *function) const {
  bool isCalledWhileLoading =
      function->inferring && this->state.inference.returns.isSuspended(function);
  if (isCalledWhileLoading) {
    auto message = ErrorTextEarlyCall::importBack(function->name);

    return err(InferenceReturnsRegistryEarlyCallError{
        .code = InferenceReturnsRegistryEarlyCallErrorCode::CALLED_WHILE_LOADING,
        .message = message,
    });
  }

  return ok(function);
}

void ReturnRequests::startInference(FuncSymbol *function) {
  auto &tasks = this->state.walk.tasks;
  FunctionDecl *functionDecl = function->decl;
  if (this->isTemplateCopy(functionDecl)) {
    tasks.pushTask(AfterTemplateReturn{.function = function});

    return tasks.pushTask(ReturnTypeOf{.function = functionDecl->generic->sym});
  }
  const auto &returns = this->state.inference.returns;
  bool hasSource = functionDecl != nullptr && returns.hasReturnSource(functionDecl);
  if (hasSource) {
    FunctionDecl *source = returns.findReturnSource(functionDecl);
    if (source == nullptr) {
      return function->setReturnType(Type::voidTy());
    }

    return this->inheritReturn(function, source->sym);
  }
  bool isInherited = functionDecl != nullptr && returns.hasInheritedReturn(functionDecl);
  if (isInherited) {
    return this->inheritReturn(function, functionDecl->overrides()->sym);
  }

  return this->startBodyInference(function);
}

bool ReturnRequests::isTemplateCopy(const FunctionDecl *functionDecl) {
  if (functionDecl == nullptr || functionDecl->generic == nullptr) {
    return false;
  }
  const ClassDecl *classDecl = functionDecl->cls();
  if (classDecl == nullptr || classDecl->generic() == nullptr) {
    return false;
  }

  return !this->state.code.late.hasClassExpr(functionDecl->generic);
}

void ReturnRequests::afterTemplateReturn(const AfterTemplateReturn &task) {
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = tasks.takeAnswer();
  if (returnType == nullptr) {
    return function->setReturnType(Type::error());
  }
  tasks.pushTask(AfterTemplateInstance{.function = function});

  return tasks.pushTask(InstanceType{.instance = function->decl->cls(), .type = returnType});
}

void ReturnRequests::afterTemplateInstance(const AfterTemplateInstance &task) {
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  const Type *instance = tasks.takeAnswer();
  FuncSymbol *generic = function->decl->generic->sym;
  tasks.pushTask(SetInferredReturn{.function = function});

  return tasks.pushTask(SubstituteType{
      .type = instance,
      .params = generic->typeParams,
      .args = function->typeParams,
  });
}

void ReturnRequests::setInferredReturn(const SetInferredReturn &task) {
  const Type *returnType = this->state.walk.tasks.takeAnswer();

  return task.function->setReturnType(returnType);
}

void ReturnRequests::inheritReturn(FuncSymbol *function, FuncSymbol *source) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInheritedReturn{.function = function});

  return tasks.pushTask(ReturnTypeOf{.function = source});
}

void ReturnRequests::afterInheritedReturn(const AfterInheritedReturn &task) {
  FuncSymbol *function = task.function;
  const Type *inherited = this->state.walk.tasks.takeAnswer();
  if (inherited != nullptr) {
    return function->setReturnType(inherited);
  }

  return this->startBodyInference(function);
}

void ReturnRequests::startBodyInference(FuncSymbol *function) {
  auto &cycles = this->state.inference.cycles;
  if (cycles.isInCycle(function)) {
    auto cycle = cycles.getCycleOf(function);

    return this->pushBodyCheck(function, InferCycle{.cycle = std::move(cycle)});
  }

  return this->pushBodyCheck(function, InferFromBody{.functionDecl = function->decl});
}

void ReturnRequests::pushBodyCheck(FuncSymbol *function, CheckTask work) {
  auto &tasks = this->state.walk.tasks;
  FunctionDecl *functionDecl = function->decl;
  InstanceRecord *lateOwner =
      functionDecl->isClosure() ? nullptr : this->returnRules.lateOwnerOf(functionDecl);
  tasks.pushTask(LeaveReturnBody{});
  tasks.pushTask(CloseBodyCheck{.lateOwner = lateOwner});
  tasks.pushTask(std::move(work));
  tasks.pushTask(OpenBodyCheck{.function = function, .lateOwner = lateOwner});

  return tasks.pushTask(EnterReturnBody{.functionDecl = functionDecl});
}

void ReturnRequests::walkBodyNow(const WalkBodyNow &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  this->state.walk.tasks.pushTask(AfterBodyNowInference{.functionDecl = functionDecl});
  if (functionDecl->sym->hasReturnType()) {
    return;
  }

  return this->startInference(functionDecl->sym);
}

void ReturnRequests::afterBodyNowInference(const AfterBodyNowInference &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (functionDecl->checked) {
    return;
  }

  return this->pushBodyCheck(functionDecl->sym,
                             WalkFunctionFromScratch{.functionDecl = functionDecl});
}

void ReturnRequests::noteInheritedReturn(const NoteInheritedReturn &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  bool isInherited = functionDecl != nullptr && functionDecl->cls() != nullptr &&
                     !functionDecl->returnType.has_value() && functionDecl->sym != nullptr &&
                     functionDecl->sym->hasReturnType();
  if (!isInherited) {
    return;
  }
  std::string type = functionDecl->sym->getKnownReturnType()->name().str();
  const FunctionDecl *base = functionDecl->overrides();
  if (base == nullptr) {
    return;
  }
  auto returns = ErrorTextInheritedReturn::returns(functionDecl->name, base->cls()->name, type);

  return this->state.diagnostics.note(base->loc(), returns);
}

}
