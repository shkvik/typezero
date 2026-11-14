#include "compiler/semantic/code/classes/class_calls/method_calls/method_calls.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/events/events_function_table/events_function_table.h"
#include "compiler/program/library/stream/stream_static_table/stream_static_table.h"

using llvm::dyn_cast;

namespace tz {

MethodCalls::MethodCalls(SemanticState &owner, ClassFacts &sharedFacts,
                         ClassCallRules &sharedClassCallRules)
    : facts(sharedFacts),
      state(owner),
      classCallRules(sharedClassCallRules) {}

void MethodCalls::checkMethodCall(const CheckMethodCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto *classDecl = this->facts.classOf(member->object->type);
  call->type = Type::error();
  member->type = Type::error();
  this->state.walk.tasks.pushTask(NoteTlsMethod{.member = member, .classDecl = classDecl});
  auto draft = MethodCallDraft{
      .call = call,
      .member = member,
      .classDecl = classDecl,
      .expected = task.expected,
      .stage = MethodCallStage::FILE_SYSTEM,
  };

  return this->nextMethodCall(draft);
}

void MethodCalls::nextMethodCall(const MethodCallDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  auto next = draft;
  CallExpr *call = draft.call;
  MemberExpr *member = draft.member;
  ClassDecl *classDecl = draft.classDecl;
  switch (draft.stage) {
    case MethodCallStage::FILE_SYSTEM:
      next.stage = MethodCallStage::STREAM;
      tasks.pushTask(AfterMethodCallTry{.draft = next});
      return tasks.pushTask(
          CheckFsClassMethod{.call = call, .member = member, .classDecl = classDecl});
    case MethodCallStage::STREAM:
      next.stage = MethodCallStage::EMITTER;
      tasks.pushTask(AfterMethodCallTry{.draft = next});
      return tasks.pushTask(
          CheckStreamCall{.call = call, .member = member, .classDecl = classDecl});
    case MethodCallStage::EMITTER:
      next.stage = MethodCallStage::EVENT_TARGET;
      if (!this->state.library.events.isEmitterClass(classDecl)) {
        return this->nextMethodCall(next);
      }
      tasks.pushTask(AfterMethodCallTry{.draft = next});
      return tasks.pushTask(CheckEmitterCall{
          .call = call,
          .member = member,
          .classDecl = classDecl,
          .receiver = CallReceiver::OWN,
      });
    case MethodCallStage::EVENT_TARGET:
      next.stage = MethodCallStage::CLASS;
      if (!this->isEventTargetClass(classDecl)) {
        return this->nextMethodCall(next);
      }
      tasks.pushTask(AfterMethodCallTry{.draft = next});
      return tasks.pushTask(
          CheckEventTargetCall{.call = call, .member = member, .classDecl = classDecl});
    case MethodCallStage::CLASS:
      return this->startClassMethodCall(call, member, classDecl, draft.expected);
  }
}

void MethodCalls::afterMethodCallTry(const AfterMethodCallTry &task) {
  Expr *claimed = this->state.walk.tasks.takeChecked();
  if (claimed != nullptr) {
    return this->state.walk.tasks.pushChecked(claimed);
  }

  return this->nextMethodCall(task.draft);
}

bool MethodCalls::isEventTargetClass(const ClassDecl *classDecl) const {
  auto targetName = LibraryClassTable::getName(LibraryClass::EVENT_TARGET);
  auto *target = this->state.library.declarations.findLibraryClass(targetName);

  return target != nullptr && classDecl->derivesFrom(target);
}

void MethodCalls::startClassMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                                       const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  const auto *field = classDecl->findField(member->name);
  if (field != nullptr && field->method() == nullptr) {
    tasks.pushTask(AfterClassFieldCallee{.call = call, .member = member});
    return tasks.pushTask(CheckClassMember{.member = member, .expected = nullptr});
  }
  auto found = this->checkCalledMethods(classDecl, member->name);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);
    tasks.pushChecked(call);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  std::vector<FunctionDecl *> methods = found.value();
  if (methods.size() == 1) {
    this->walkMethodAccess(member, methods[0]);
  }
  auto ready = this->checkFieldsSetBeforeCall(member);
  if (ready.isErr()) {
    this->state.diagnostics.error(member->loc(), ready.error().message);
  }
  tasks.pushTask(AfterMethodChoice{
      .call = call,
      .member = member,
      .classDecl = classDecl,
      .expected = expected,
      .count = methods.size(),
  });

  return this->state.walk.tasks.pushTask(ChooseVariant{
      .variants = methods,
      .args = &call->args,
      .spread = call->spread,
      .generics = GenericVariants::ALLOWED,
  });
}

void MethodCalls::afterClassFieldCallee(const AfterClassFieldCallee &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->callee = tasks.takeChecked();
  auto callable = this->checkFieldCallable(call->callee->type, member->name);
  if (callable.isOk()) {
    return tasks.pushTask(CheckValueCall{.call = call, .type = callable.value()});
  }
  if (callable.error().code != CodeClassesRegistryFieldCallableErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(member->loc(), callable.error().message);
  }
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void MethodCalls::afterMethodChoice(const AfterMethodChoice &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto choice = this->state.code.overloads.takeChoice();
  if (choice.function == nullptr) {
    this->classCallRules.reportChoice(choice, member->loc());
    return tasks.pushChecked(call);
  }
  FuncSymbol *function = choice.function;
  if (task.count > 1 && function->decl != nullptr) {
    this->walkMethodAccess(member, function->decl);
  }
  auto allowed = this->checkMethodTypeArgs(function, member);
  if (allowed.isErr()) {
    this->state.diagnostics.error(member->loc(), allowed.error().message);
  }
  if (function->isGeneric() && choice.isChecked) {
    return this->classCallRules.startGenericCall(call, function, choice.typeArgs, member->loc(),
                                                 GenericCallee::METHOD);
  }
  if (function->isGeneric()) {
    tasks.pushTask(AfterGenericMethodCall{.call = call, .member = member, .isSuper = false});
    return tasks.pushTask(CheckGenericCall{
        .call = call,
        .function = function,
        .loc = member->loc(),
        .expected = task.expected,
    });
  }
  call->target = this->methodTarget(task.classDecl, function);

  return this->classCallRules.pushPlainMethodCall(call, member, function, choice.isChecked);
}

void MethodCalls::afterPlainMethodType(const AfterPlainMethodType &task) {
  task.member->type = this->state.walk.tasks.takeAnswer();
  task.call->type = task.function->getReturnType();
}

void MethodCalls::afterGenericMethodCall(const AfterGenericMethodCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  tasks.takeChecked();
  FuncSymbol *chosen = call->func();
  if (task.isSuper) {
    call->target = SuperCall{.method = chosen};
  } else {
    call->target = MethodCall{.method = chosen};
  }
  tasks.pushChecked(call);
  if (call->method() == nullptr) {
    return;
  }
  tasks.pushTask(AfterMemberFunctionType{.member = task.member});

  return tasks.pushTask(FunctionTypeOf{.function = call->method()});
}

void MethodCalls::afterBoundTypes(const AfterBoundTypes &task) {
  CallExpr *call = task.call;
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  const Type *returned = task.isReturnKnown ? tasks.takeAnswer() : Type::error();
  auto params = std::vector<const Type *>(function->params.size());
  for (size_t index = params.size(); index > 0; --index) {
    params[index - 1] = tasks.takeAnswer();
  }
  this->state.generics.copies.leaveInstanceLoc();
  call->type = returned;
  this->setGenericTarget(call, function, task.kind);
  tasks.pushTask(CheckArguments{
      .call = call,
      .params = std::move(params),
      .minArgs = function->minArgs,
      .hasRest = function->hasRest,
      .loc = task.loc,
      .checked = std::vector<bool>(call->args.size(), true),
  });
  tasks.pushTask(AfterGenericCallee{.call = call, .kind = task.kind});
  if (this->isInstantiatedNow(function)) {
    return tasks.pushTask(InstantiateCallTarget{
        .call = call,
        .generic = function,
        .typeArgs = task.bound,
        .loc = task.loc,
    });
  }
}

void MethodCalls::afterGenericCallee(const AfterGenericCallee &task) {
  CallExpr *call = task.call;
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || task.kind != GenericCallee::METHOD) {
    return;
  }
  this->state.walk.tasks.pushTask(AfterMemberFunctionType{.member = member});

  return this->state.walk.tasks.pushTask(FunctionTypeOf{.function = call->method()});
}

void MethodCalls::checkOverloadCall(const CheckOverloadCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *interfaceType = member->object->type;
  const Overload &overload = *interfaceType->overload(member->name);
  call->type = Type::error();
  member->type = Type::error();
  auto variants = this->facts.signatureVariants(interfaceType, overload);
  this->state.walk.tasks.pushTask(
      AfterOverloadChoice{.call = call, .member = member, .variants = variants});

  return this->state.walk.tasks.pushTask(ChooseVariant{
      .variants = variants,
      .args = &call->args,
      .spread = call->spread,
      .generics = GenericVariants::REJECTED,
  });
}

void MethodCalls::afterOverloadChoice(const AfterOverloadChoice &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto choice = this->state.code.overloads.takeChoice();
  if (choice.function == nullptr) {
    this->classCallRules.reportChoice(choice, member->loc());
    return tasks.pushChecked(call);
  }
  const Type *interfaceType = member->object->type;
  const Overload &overload = *interfaceType->overload(member->name);
  FuncSymbol *function = choice.function;
  int signature = this->facts.signatureIndex(task.variants, function);
  call->target = OverloadCall{.signature = signature};
  member->type = overload.signatures[size_t(signature)];
  call->type = function->getKnownReturnType();
  auto arguments =
      this->classCallRules.argumentsTask(call, function, member->loc(), choice.isChecked);
  tasks.pushTask(std::move(arguments));

  return tasks.pushTask(CalledFunction{});
}

void MethodCalls::checkClassStaticCall(const CheckClassStaticCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  auto eventsFunction = EventsFunctionTable::find(member->name);
  bool hasEventsFunction =
      classDecl->library() &&
      LibraryClassTable::isName(classDecl->name, LibraryClass::EVENT_EMITTER) &&
      eventsFunction.has_value();
  if (hasEventsFunction) {
    tasks.pushTask(
        AfterEventsFunctionTry{.call = call, .member = member, .expected = task.expected});
    return tasks.pushTask(
        CheckEventsFunction{.call = call, .name = member->name, .loc = member->loc()});
  }
  bool isReadableFrom = classDecl->library() &&
                        LibraryClassTable::isName(classDecl->name, LibraryClass::READABLE) &&
                        StreamStaticTable::find(member->name) == StreamStatic::FROM;
  if (isReadableFrom) {
    return tasks.pushTask(CheckStreamFrom{.call = call, .member = member});
  }
  auto methods = classDecl->findStaticMethodsNamed(member->name);
  if (methods.empty()) {
    tasks.pushTask(AfterStaticFieldCallee{.call = call, .member = member, .classDecl = classDecl});
    return tasks.pushTask(
        CheckStaticMember{.member = member, .classDecl = classDecl, .expected = nullptr});
  }
  auto accessible = this->facts.checkAccess(methods[0]->cls(), methods[0]->access(), member->name);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  tasks.pushTask(
      AfterStaticMethodChoice{.call = call, .member = member, .expected = task.expected});

  return this->state.walk.tasks.pushTask(ChooseVariant{
      .variants = methods,
      .args = &call->args,
      .spread = call->spread,
      .generics = GenericVariants::ALLOWED,
  });
}

void MethodCalls::afterEventsFunctionTry(const AfterEventsFunctionTry &task) {
  Expr *claimed = this->state.walk.tasks.takeChecked();
  if (claimed != nullptr) {
    return this->state.walk.tasks.pushChecked(claimed);
  }
  MemberExpr *member = task.member;
  auto *function = this->state.library.declarations.findLibraryFunction(member->name);

  return this->state.walk.tasks.pushTask(CheckDirectCall{
      .call = task.call,
      .function = function->sym,
      .loc = member->loc(),
      .expected = task.expected,
  });
}

void MethodCalls::afterStaticFieldCallee(const AfterStaticFieldCallee &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->callee = tasks.takeChecked();
  auto callable = this->checkStaticCallable(call->callee->type, task.classDecl, member->name);
  if (callable.isOk()) {
    return tasks.pushTask(CheckValueCall{.call = call, .type = callable.value()});
  }
  if (callable.error().code != CodeClassesRegistryStaticCallableErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(member->loc(), callable.error().message);
  }
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void MethodCalls::afterStaticMethodChoice(const AfterStaticMethodChoice &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto choice = this->state.code.overloads.takeChoice();
  if (choice.function == nullptr) {
    this->classCallRules.reportChoice(choice, member->loc());
    return tasks.pushChecked(call);
  }
  FuncSymbol *function = choice.function;
  auto allowed = this->checkStaticMethodTypeArgs(function, member);
  if (allowed.isErr()) {
    this->state.diagnostics.error(member->loc(), allowed.error().message);
  }
  if (function->isGeneric() && choice.isChecked) {
    return this->classCallRules.startGenericCall(call, function, choice.typeArgs, member->loc(),
                                                 GenericCallee::FUNCTION);
  }
  if (function->isGeneric()) {
    return tasks.pushTask(CheckGenericCall{
        .call = call,
        .function = function,
        .loc = member->loc(),
        .expected = task.expected,
    });
  }
  call->target = FunctionCall{.func = function};
  call->type = function->getReturnType();

  auto arguments =
      this->classCallRules.argumentsTask(call, function, member->loc(), choice.isChecked);
  return tasks.pushTask(std::move(arguments));
}

MethodCalls::ResultFieldIsCallable MethodCalls::checkFieldCallable(const Type *type,
                                                                   llvm::StringRef name) const {
  bool isOptional = type->isOptional() && type->inner()->isFunction();
  if (isOptional) {
    auto message = ErrorTextFieldCall::undefined();

    return err(CodeClassesRegistryFieldCallableError{
        .code = CodeClassesRegistryFieldCallableErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isError()) {
    return err(CodeClassesRegistryFieldCallableError{
        .code = CodeClassesRegistryFieldCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction()) {
    auto message = ErrorTextFieldCall::notFunc(name, type->name());

    return err(CodeClassesRegistryFieldCallableError{
        .code = CodeClassesRegistryFieldCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

MethodCalls::ResultMethodsAreFound MethodCalls::checkCalledMethods(const ClassDecl *classDecl,
                                                                   llvm::StringRef name) const {
  auto methods = classDecl->findMethodsNamed(name);
  bool isStatic = methods.empty() && !classDecl->findStaticMethodsNamed(name).empty();
  if (isStatic) {
    auto message = ErrorTextMethodLookup::isStatic(name, classDecl->name);

    return err(CodeClassesRegistryMethodCallInstanceMethodsError{
        .code = CodeClassesRegistryMethodCallInstanceMethodsErrorCode::STATIC_METHOD,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextMethodLookup::noProperty(name, classDecl->type->name());

    return err(CodeClassesRegistryMethodCallInstanceMethodsError{
        .code = CodeClassesRegistryMethodCallInstanceMethodsErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(methods);
}

MethodCalls::ResultFieldsAreAllSet
MethodCalls::checkFieldsSetBeforeCall(const MemberExpr *member) const {
  const Flow &flow = this->state.walk.flow.getFlow();
  bool isEarly = this->facts.isInitThis(member->object).isOk() &&
                 !this->state.walk.flow.isBeforeSuper() && this->facts.checkThisReady(flow).isErr();
  if (isEarly) {
    auto unset = this->facts.firstUnset(flow);
    auto message = ErrorTextUnsetCall::beforeSet(member->name, unset.value());

    return err(CodeClassesRegistryFieldsSetError{
        .code = CodeClassesRegistryFieldsSetErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

MethodCalls::ResultTypeArgsAllowed
MethodCalls::checkMethodTypeArgs(const FuncSymbol *function, const MemberExpr *member) const {
  if (!function->isGeneric() && !member->typeArgs.empty()) {
    auto message = ErrorTextMethodTypeArgs::notGeneric(member->name);

    return err(CodeClassesRegistryMethodTypeArgsError{
        .code = CodeClassesRegistryMethodTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(function);
}

void MethodCalls::walkMethodAccess(MemberExpr *member, FunctionDecl *method) {
  auto accessible =
      this->facts.checkAccess(method->cls(), method->access(), member->name, member->object);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
}

MethodCall MethodCalls::methodTarget(ClassDecl *classDecl, FuncSymbol *function) const {
  auto *implementation = this->facts.soleImplementation(classDecl, function->decl);
  if (implementation != nullptr) {
    return MethodCall{.method = implementation->sym};
  }

  return MethodCall{.method = function, .virtualCall = true};
}

bool MethodCalls::isInstantiatedNow(const FuncSymbol *function) const {
  const ClassDecl *classDecl = function->decl->cls();
  bool isOwnerEmitted =
      classDecl == nullptr || function->decl->isStatic() || classDecl->isEmitted();
  bool isSettled = !this->state.walk.functions.isInsideTrial() && !function->inferring &&
                   function->hasReturnType();

  return isSettled && isOwnerEmitted;
}

void MethodCalls::setGenericTarget(CallExpr *call, FuncSymbol *callee, GenericCallee kind) const {
  if (call->superCall()) {
    call->target = SuperCall{.method = callee};
    return;
  }
  switch (kind) {
    case GenericCallee::METHOD:
      call->target = MethodCall{.method = callee};
      break;
    case GenericCallee::FUNCTION:
      call->target = FunctionCall{.func = callee};
      break;
  }
}

MethodCalls::ResultStaticFieldIsCallable
MethodCalls::checkStaticCallable(const Type *type, const ClassDecl *classDecl,
                                 llvm::StringRef name) const {
  if (type->isError()) {
    return err(CodeClassesRegistryStaticCallableError{
        .code = CodeClassesRegistryStaticCallableErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!type->isFunction()) {
    auto message = ErrorTextStaticCall::notFunc(classDecl->name, name, type->name());

    return err(CodeClassesRegistryStaticCallableError{
        .code = CodeClassesRegistryStaticCallableErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }

  return ok(type);
}

MethodCalls::ResultTypeArgsAllowed
MethodCalls::checkStaticMethodTypeArgs(const FuncSymbol *function, const MemberExpr *member) const {
  if (!function->isGeneric() && !member->typeArgs.empty()) {
    auto message = ErrorTextMethodTypeArgs::notGeneric(member->name);

    return err(CodeClassesRegistryMethodTypeArgsError{
        .code = CodeClassesRegistryMethodTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(function);
}

}
