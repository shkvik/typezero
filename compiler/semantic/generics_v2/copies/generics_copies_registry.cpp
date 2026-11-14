#include "compiler/semantic/generics_v2/copies/generics_copies_registry.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

GenericsCopiesRegistry::GenericsCopiesRegistry(SemanticState &owner)
    : state(owner) {}

void GenericsCopiesRegistry::forceConversion(const ForceConversion &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *expr = tasks.takeChecked();
  tasks.pushChecked(expr);
  const Type *to = task.to;
  const Type *type = expr->type;
  if (type == nullptr || type->isError() || type == to) {
    return;
  }
  if (to->isString()) {
    return tasks.pushTask(ToStringOperand{});
  }
  if (this->state.types.relations.isImplicitlyConvertible(type, to)) {
    return tasks.pushTask(Convert{.to = to});
  }
}

void GenericsCopiesRegistry::walkInstance(const WalkInstance &task) {
  InstanceRecord *instance = task.instance;
  instance->checking = true;
  if (this->state.generics.copies.isBrokenGeneric(instance->generic)) {
    return this->finishInstanceWalk(instance, {});
  }

  return this->startCopyWalk(instance, instance->decl, CopyWalk::INSTANCE);
}

void GenericsCopiesRegistry::walkPreciseBody(const WalkPreciseBody &task) {
  return this->startCopyWalk(task.instance, task.copy, CopyWalk::PRECISE);
}

void GenericsCopiesRegistry::walkTemplateInstance(const WalkTemplateInstance &task) {
  InstanceRecord *instance = task.instance;
  instance->checking = true;

  return this->startCopyWalk(instance, instance->decl, CopyWalk::TEMPLATE);
}

void GenericsCopiesRegistry::startCopyWalk(InstanceRecord *instance, FunctionDecl *copy,
                                           CopyWalk walk) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(StartCopyBody{.instance = instance, .copy = copy, .walk = walk});

  return tasks.pushTask(EnterReturnBody{.functionDecl = copy});
}

void GenericsCopiesRegistry::startCopyBody(const StartCopyBody &task) {
  FunctionDecl *copy = task.copy;
  auto &tasks = this->state.walk.tasks;
  this->state.generics.copies.rememberCheckingInstance(task.instance);
  this->state.diagnostics.pushList(DiagnosticRoute::OWNER);
  tasks.pushTask(PopCopyDiagnostics{.instance = task.instance, .walk = task.walk});
  if (task.walk != CopyWalk::TEMPLATE) {
    return tasks.pushTask(WalkFunctionFromScratch{.functionDecl = copy});
  }
  if (!this->state.declaration.modules.isTopLevelDone()) {
    this->state.walk.functions.rememberEarlyFunction(copy->sym);
  }
  this->state.walk.flow.replaceFlow(Flow());
  if (copy->sym->hasReturnType()) {
    return tasks.pushTask(WalkFunctionBody{.functionDecl = copy});
  }

  return tasks.pushTask(InferFromBody{.functionDecl = copy});
}

void GenericsCopiesRegistry::popCopyDiagnostics(const PopCopyDiagnostics &task) {
  auto &tasks = this->state.walk.tasks;
  auto diagnostics = this->state.diagnostics.popList();
  tasks.pushTask(FinishCopyWalk{
      .instance = task.instance,
      .walk = task.walk,
      .diagnostics = std::move(diagnostics),
  });

  return tasks.pushTask(LeaveReturnBody{});
}

void GenericsCopiesRegistry::finishCopyWalk(const FinishCopyWalk &task) {
  InstanceRecord *instance = task.instance;
  if (task.walk == CopyWalk::INSTANCE) {
    return this->finishInstanceWalk(instance, task.diagnostics);
  }
  const auto *firstError = this->state.diagnostics.findFirstError(task.diagnostics);
  if (task.walk == CopyWalk::PRECISE) {
    if (firstError != nullptr) {
      instance->message = firstError->message;
      instance->errorLoc = firstError->loc;
    }
    return;
  }
  instance->checking = false;
  instance->checked = true;
  if (firstError == nullptr) {
    return;
  }
  instance->failed = true;
  instance->message = firstError->message;
  instance->errorLoc = firstError->loc;
}

void GenericsCopiesRegistry::finishInstanceWalk(InstanceRecord *instance,
                                                const std::vector<Diagnostic> &diagnostics) {
  instance->checking = false;
  instance->checked = true;
  const auto *firstError = this->state.diagnostics.findFirstError(diagnostics);
  if (firstError == nullptr) {
    return;
  }
  instance->failed = true;
  instance->message = firstError->message;
  instance->errorLoc = firstError->loc;
  if (instance->concrete != instance->args) {
    return this->state.walk.tasks.pushTask(WalkPreciseCopy{.instance = instance});
  }
}

void GenericsCopiesRegistry::reportInstance(const ReportInstance &task) {
  InstanceRecord *instance = task.instance;

  return this->reportInstanceAt(
      ReportInstanceAt{.instance = instance, .loc = instance->callLoc, .args = instance->concrete});
}

void GenericsCopiesRegistry::reportInstanceAt(const ReportInstanceAt &task) {
  InstanceRecord *instance = task.instance;
  llvm::SMLoc callLoc = task.loc;
  auto destination = this->state.diagnostics.getCurrentDestination();
  instance->isReported = destination != DiagnosticRoute::DISCARD;
  if (destination == DiagnosticRoute::OWNER) {
    this->state.generics.copies.rememberFailedCall(callLoc, instance);

    return this->state.diagnostics.error(callLoc, instance->message);
  }
  const FuncSymbol *generic = instance->generic;
  auto bindings = GenericCheckHelpers::bindingNames(generic->typeParams, task.args);
  auto message = ErrorTextInstanceCall::cannotCall(generic->name, bindings, instance->message);
  this->state.diagnostics.error(callLoc, message);

  return this->noteFailedChain(instance, CopyChain::CALL);
}

void GenericsCopiesRegistry::noteFailedChain(const InstanceRecord *start, CopyChain chain) {
  auto shown = llvm::SmallPtrSet<const char *, 8>();
  const InstanceRecord *at = start;
  while (at != nullptr && at->errorLoc.isValid()) {
    bool isFirstShown = shown.insert(at->errorLoc.getPointer()).second;
    if (!isFirstShown) {
      return;
    }
    const auto *next = this->state.generics.copies.findFailedCall(at->errorLoc);
    auto where = chain == CopyChain::CALL ? this->failedCallNote(at, next)
                                          : this->failedClassCallNote(at, next);
    this->state.diagnostics.note(at->errorLoc, where);
    at = next;
  }
}

std::string GenericsCopiesRegistry::failedCallNote(const InstanceRecord *at,
                                                   const InstanceRecord *next) const {
  llvm::StringRef name = GenericCheckHelpers::recordName(at);
  if (next == nullptr) {
    return ErrorTextInstanceCall::here(name);
  }
  bool isClass = next->cls != nullptr;

  return ErrorTextInstanceCall::inWhich(name, isClass, GenericCheckHelpers::recordName(next));
}

std::string GenericsCopiesRegistry::failedClassCallNote(const InstanceRecord *at,
                                                        const InstanceRecord *next) const {
  llvm::StringRef name = GenericCheckHelpers::recordName(at);
  if (next == nullptr) {
    return ErrorTextInstanceUse::here(name);
  }

  return ErrorTextInstanceUse::usesHere(name, GenericCheckHelpers::recordName(next));
}

void GenericsCopiesRegistry::pushInstanceTasks() {
  return this->state.walk.tasks.pushTask(NextPendingInstance{.index = 0});
}

void GenericsCopiesRegistry::nextPendingInstance(const NextPendingInstance &task) {
  auto &copies = this->state.generics.copies;
  auto &tasks = this->state.walk.tasks;
  int index = task.index;
  while (index < copies.countPendingInstances() && copies.getPendingInstance(index)->checked) {
    index += 1;
  }
  if (index == copies.countPendingInstances()) {
    return copies.forgetPendingInstances();
  }
  InstanceRecord *instance = copies.getPendingInstance(index);
  tasks.pushTask(NextPendingInstance{.index = index + 1});
  tasks.pushTask(ReportFailedPending{.instance = instance});
  if (instance->cls != nullptr) {
    return tasks.pushTask(WalkClassInstance{.check = instance});
  }

  return tasks.pushTask(WalkInstance{.instance = instance});
}

void GenericsCopiesRegistry::reportFailedPending(const ReportFailedPending &task) {
  InstanceRecord *instance = task.instance;
  if (!instance->failed) {
    return;
  }
  if (instance->cls != nullptr) {
    return this->reportClassInstance(
        ReportClassInstance{.check = instance, .loc = instance->callLoc});
  }

  return this->reportInstance(ReportInstance{.instance = instance});
}

void GenericsCopiesRegistry::walkClassInstance(const WalkClassInstance &task) {
  InstanceRecord *instance = task.check;
  if (instance->checked) {
    return;
  }
  instance->checking = true;
  ClassDecl *classDecl = instance->cls;
  if (this->state.generics.copies.isBrokenClass(classDecl->generic())) {
    return this->finishClassWalk(instance, {});
  }
  auto draft = ClassBodiesDraft{
      .instance = instance,
      .bodies = this->instanceBodies(classDecl),
      .index = 0,
      .diagnostics = {},
  };

  return this->nextClassBody(NextClassBody{.draft = std::move(draft)});
}

std::vector<FunctionDecl *>
GenericsCopiesRegistry::instanceBodies(const ClassDecl *classDecl) const {
  auto bodies = std::vector<FunctionDecl *>();
  if (classDecl->init != nullptr) {
    bodies.push_back(classDecl->init);
  }
  bodies.insert(bodies.end(), classDecl->constructors.begin(), classDecl->constructors.end());
  for (FunctionDecl *method : classDecl->findOwnMethods()) {
    if (!method->isAbstract() && method->typeParams.empty()) {
      bodies.push_back(method);
    }
  }

  return bodies;
}

void GenericsCopiesRegistry::nextClassBody(const NextClassBody &task) {
  ClassBodiesDraft draft = task.draft;
  while (draft.index < draft.bodies.size() && !this->isWalkedBody(draft.bodies[draft.index])) {
    draft.index += 1;
  }
  if (draft.index == draft.bodies.size()) {
    return this->finishClassWalk(draft.instance, draft.diagnostics);
  }
  FunctionDecl *functionDecl = draft.bodies[draft.index];
  auto &tasks = this->state.walk.tasks;
  if (functionDecl->sym->hasReturnType()) {
    return this->startClassBody(StartClassBody{.draft = std::move(draft)});
  }
  tasks.pushTask(StartClassBody{.draft = std::move(draft)});

  return tasks.pushTask(InferReturnType{.function = functionDecl->sym});
}

bool GenericsCopiesRegistry::isWalkedBody(const FunctionDecl *functionDecl) const {
  return !functionDecl->checked && functionDecl->sym != nullptr;
}

void GenericsCopiesRegistry::startClassBody(const StartClassBody &task) {
  FunctionDecl *functionDecl = task.draft.bodies[task.draft.index];
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(ClassBodyEntered{.draft = task.draft});

  return tasks.pushTask(EnterReturnBody{.functionDecl = functionDecl});
}

void GenericsCopiesRegistry::classBodyEntered(const ClassBodyEntered &task) {
  FunctionDecl *functionDecl = task.draft.bodies[task.draft.index];
  auto &tasks = this->state.walk.tasks;
  this->state.generics.copies.rememberCheckingInstance(task.draft.instance);
  this->state.diagnostics.pushList(DiagnosticRoute::OWNER);
  tasks.pushTask(PopClassBodyDiagnostics{.draft = task.draft});

  return tasks.pushTask(WalkFunctionFromScratch{.functionDecl = functionDecl});
}

void GenericsCopiesRegistry::popClassBodyDiagnostics(const PopClassBodyDiagnostics &task) {
  ClassBodiesDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  llvm::append_range(draft.diagnostics, this->state.diagnostics.popList());
  draft.index += 1;
  tasks.pushTask(NextClassBody{.draft = std::move(draft)});

  return tasks.pushTask(LeaveReturnBody{});
}

void GenericsCopiesRegistry::finishClassWalk(InstanceRecord *instance,
                                             const std::vector<Diagnostic> &diagnostics) {
  instance->checking = false;
  instance->checked = true;
  const auto *firstError = this->state.diagnostics.findFirstError(diagnostics);
  if (firstError == nullptr || instance->failed) {
    return;
  }
  instance->failed = true;
  instance->message = firstError->message;
  instance->errorLoc = firstError->loc;
}

void GenericsCopiesRegistry::reportClassInstance(const ReportClassInstance &task) {
  InstanceRecord *instance = task.check;
  llvm::SMLoc loc = task.loc.isValid() ? task.loc : instance->cls->generic()->loc();
  auto destination = this->state.diagnostics.getCurrentDestination();
  instance->isReported = destination != DiagnosticRoute::DISCARD;
  if (destination == DiagnosticRoute::OWNER) {
    this->state.generics.copies.rememberFailedCall(loc, instance);

    return this->state.diagnostics.error(loc, instance->message);
  }
  const ClassDecl *classDecl = instance->cls;
  auto bindings =
      GenericCheckHelpers::bindingNames(classDecl->generic()->typeArgs, classDecl->typeArgs);
  auto message = ErrorTextInstanceUse::cannotUse(classDecl->name, bindings, instance->message);
  this->state.diagnostics.error(loc, message);

  return this->noteFailedChain(instance, CopyChain::CLASS);
}

}
