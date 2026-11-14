#include "compiler/semantic/code/classes/class_calls/super_calls/super_calls.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

SuperCalls::SuperCalls(SemanticState &owner, ClassFacts &sharedFacts,
                       ClassCallRules &sharedClassCallRules)
    : facts(sharedFacts),
      state(owner),
      classCallRules(sharedClassCallRules) {}

void SuperCalls::checkSuperCall(const CheckSuperCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::voidTy();
  call->callee->type = Type::voidTy();
  call->target = SuperCall{};
  auto *classDecl = this->superCallClass(call);
  if (classDecl == nullptr) {
    tasks.pushChecked(call);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  tasks.pushTask(FinishSuperCall{.classDecl = classDecl});
  auto constructors = this->facts.constructorsOf(classDecl->parent);
  if (constructors.empty()) {
    tasks.pushChecked(call);
    auto none = this->checkSuperArguments(call, classDecl->parent);
    if (none.isErr()) {
      this->state.diagnostics.error(call->loc(), none.error().message);
    }
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  tasks.pushTask(AfterParentConstructorChoice{.call = call, .first = constructors[0]});

  return this->state.walk.tasks.pushTask(ChooseVariant{
      .variants = constructors,
      .args = &call->args,
      .spread = call->spread,
      .generics = GenericVariants::REJECTED,
  });
}

void SuperCalls::afterParentConstructorChoice(const AfterParentConstructorChoice &task) {
  CallExpr *call = task.call;
  auto choice = this->state.code.overloads.takeChoice();
  if (choice.function == nullptr) {
    this->classCallRules.reportChoice(choice, call->loc());
    return this->state.walk.tasks.pushChecked(call);
  }
  FuncSymbol *function = choice.function;
  const FunctionDecl *chosen = function->decl != nullptr ? function->decl : task.first;
  auto constructorName = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto accessible = this->facts.checkAccess(chosen->cls(), chosen->access(), constructorName);
  if (accessible.isErr()) {
    this->state.diagnostics.error(call->loc(), accessible.error().message);
  }
  call->target = SuperCall{.method = function};
  auto arguments =
      this->classCallRules.argumentsTask(call, function, call->loc(), choice.isChecked);

  return this->state.walk.tasks.pushTask(std::move(arguments));
}

void SuperCalls::finishSuperCall(const FinishSuperCall &task) {
  this->state.walk.flow.markSuperCalled();
  for (const ClassField &field : task.classDecl->fields) {
    if (!field.isStatic() && field.init != nullptr && field.method() == nullptr) {
      this->state.walk.flow.addSetField(field.name);
    }
  }

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void SuperCalls::checkSuperMethodCall(const CheckSuperMethodCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  member->type = Type::error();
  auto *method = this->enclosingMethod();
  auto scope = this->checkSuperContext(method);
  if (scope.isErr() && scope.error().code != CodeClassesRegistrySuperContextErrorCode::BASE_ONLY) {
    this->state.diagnostics.error(member->object->loc(), scope.error().message);
  }
  if (scope.isErr()) {
    tasks.pushChecked(call);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  ClassDecl *parent = method->cls()->parent;
  auto *self =
      this->state.ast.make<NameExpr>(member->object->loc(), KeywordTable::getName(Keyword::THIS));
  this->state.walk.self.rememberThisMember(member->name);
  tasks.pushTask(AfterSuperObject{.call = call, .member = member, .parent = parent});

  return tasks.pushTask(CheckThis{.name = self});
}

void SuperCalls::afterSuperObject(const AfterSuperObject &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  ClassDecl *parent = task.parent;
  auto &tasks = this->state.walk.tasks;
  this->state.walk.self.forgetThisMember();
  member->object = tasks.takeChecked();
  if (member->object->type->isError()) {
    tasks.pushChecked(call);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto complete = this->checkObjectComplete(member);
  if (complete.isErr()) {
    this->state.diagnostics.error(member->loc(), complete.error().message);
  }
  auto found = this->checkParentMethods(parent, member->name);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);
    tasks.pushChecked(call);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  std::vector<FunctionDecl *> methods = found.value();
  auto accessible = this->facts.checkAccess(methods[0]->cls(), methods[0]->access(), member->name);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  if (!this->state.library.events.isEmitterClass(parent)) {
    return this->startSuperMethodChoice(call, member, methods);
  }
  tasks.pushTask(AfterSuperEmitterTry{.call = call, .member = member, .methods = methods});

  return tasks.pushTask(CheckEmitterCall{
      .call = call,
      .member = member,
      .classDecl = parent,
      .receiver = CallReceiver::SUPER,
  });
}

void SuperCalls::afterSuperEmitterTry(const AfterSuperEmitterTry &task) {
  Expr *claimed = this->state.walk.tasks.takeChecked();
  if (claimed != nullptr) {
    return this->state.walk.tasks.pushChecked(claimed);
  }

  return this->startSuperMethodChoice(task.call, task.member, task.methods);
}

void SuperCalls::startSuperMethodChoice(CallExpr *call, MemberExpr *member,
                                        const std::vector<FunctionDecl *> &methods) {
  this->state.walk.tasks.pushTask(AfterSuperMethodChoice{.call = call, .member = member});

  return this->state.walk.tasks.pushTask(ChooseVariant{
      .variants = methods,
      .args = &call->args,
      .spread = call->spread,
      .generics = GenericVariants::ALLOWED,
  });
}

void SuperCalls::afterSuperMethodChoice(const AfterSuperMethodChoice &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto choice = this->state.code.overloads.takeChoice();
  if (choice.function == nullptr) {
    this->classCallRules.reportChoice(choice, member->loc());
    return tasks.pushChecked(call);
  }
  auto body = this->checkMethodBody(choice.function, member->name);
  if (body.isErr()) {
    this->state.diagnostics.error(member->loc(), body.error().message);
    tasks.pushChecked(call);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  FuncSymbol *function = body.value();
  call->target = SuperCall{};
  if (function->isGeneric() && choice.isChecked) {
    return this->classCallRules.startGenericCall(call, function, choice.typeArgs, member->loc(),
                                                 GenericCallee::METHOD);
  }
  if (function->isGeneric()) {
    tasks.pushTask(AfterGenericMethodCall{.call = call, .member = member, .isSuper = true});
    return tasks.pushTask(CheckGenericCall{
        .call = call,
        .function = function,
        .loc = member->loc(),
        .expected = nullptr,
    });
  }
  call->target = SuperCall{.method = function};

  return this->classCallRules.pushPlainMethodCall(call, member, function, choice.isChecked);
}

SuperCalls::ResultConstructorScope SuperCalls::checkSuperAllowed(FunctionDecl *constructor) const {
  if (constructor == nullptr || !constructor->isConstructor() || constructor->cls() == nullptr) {
    auto message = ErrorTextSuperCall::outside();

    return err(CodeClassesRegistrySuperAllowedError{
        .code = CodeClassesRegistrySuperAllowedErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(constructor);
}

SuperCalls::ResultParentClassFound SuperCalls::checkParentClass(ClassDecl *classDecl) const {
  if (classDecl->parent == nullptr && !classDecl->base.has_value()) {
    auto message = ErrorTextSuperCall::noParent(classDecl->name);

    return err(CodeClassesRegistryParentClassError{
        .code = CodeClassesRegistryParentClassErrorCode::NO_PARENT,
        .message = message,
    });
  }
  if (classDecl->parent == nullptr) {
    return err(CodeClassesRegistryParentClassError{
        .code = CodeClassesRegistryParentClassErrorCode::BASE_ONLY,
        .message = "",
    });
  }

  return ok(classDecl->parent);
}

SuperCalls::ResultSuperIsStatement SuperCalls::checkRootSuper(const CallExpr *call) const {
  if (!this->state.code.constructors.isRootSuper(call)) {
    auto message = ErrorTextSuperCall::statement();

    return err(CodeClassesRegistryRootSuperError{
        .code = CodeClassesRegistryRootSuperErrorCode::NOT_A_STATEMENT,
        .message = message,
    });
  }

  return ok(call);
}

SuperCalls::ResultSuperNotRepeated SuperCalls::checkSuperOnce(const ClassDecl *classDecl) const {
  if (!this->state.walk.flow.isBeforeSuper()) {
    auto message = ErrorTextSuperCall::twice(classDecl->parent->name);

    return err(CodeClassesRegistrySuperOnceError{
        .code = CodeClassesRegistrySuperOnceErrorCode::CALLED_TWICE,
        .message = message,
    });
  }

  return ok(classDecl);
}

SuperCalls::ResultNoSuperArguments SuperCalls::checkSuperArguments(const CallExpr *call,
                                                                   const ClassDecl *parent) const {
  if (!call->args.empty()) {
    auto message = ErrorTextSuperCall::noArgs(parent->name);

    return err(CodeClassesRegistrySuperArgumentsError{
        .code = CodeClassesRegistrySuperArgumentsErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(call);
}

ClassDecl *SuperCalls::superCallClass(const CallExpr *call) {
  auto *currentDecl = this->state.walk.functions.getCurrentDecl();
  auto scope = this->checkSuperAllowed(currentDecl);
  if (scope.isErr()) {
    this->state.diagnostics.error(call->loc(), scope.error().message);
    return nullptr;
  }
  ClassDecl *classDecl = scope.value()->cls();
  auto parent = this->checkParentClass(classDecl);
  if (parent.isErr() && parent.error().code != CodeClassesRegistryParentClassErrorCode::BASE_ONLY) {
    this->state.diagnostics.error(call->loc(), parent.error().message);
  }
  if (parent.isErr()) {
    return nullptr;
  }
  auto statement = this->checkRootSuper(call);
  if (statement.isErr()) {
    this->state.diagnostics.error(call->loc(), statement.error().message);
    return nullptr;
  }
  auto once = this->checkSuperOnce(classDecl);
  if (once.isErr()) {
    this->state.diagnostics.error(call->loc(), once.error().message);
    return nullptr;
  }

  return classDecl;
}

SuperCalls::ResultSuperMethodScope SuperCalls::checkSuperContext(FunctionDecl *method) const {
  if (method == nullptr || (method->cls()->parent == nullptr && !method->cls()->base.has_value())) {
    auto message = ErrorTextSuperMethod::outside();

    return err(CodeClassesRegistrySuperContextError{
        .code = CodeClassesRegistrySuperContextErrorCode::NO_SUPER_CLASS,
        .message = message,
    });
  }
  if (method->cls()->parent == nullptr) {
    return err(CodeClassesRegistrySuperContextError{
        .code = CodeClassesRegistrySuperContextErrorCode::BASE_ONLY,
        .message = "",
    });
  }

  return ok(method);
}

SuperCalls::ResultObjectIsComplete SuperCalls::checkObjectComplete(const MemberExpr *member) const {
  const Flow &flow = this->state.walk.flow.getFlow();
  bool isEarly = this->facts.isInitThis(member->object).isOk() &&
                 !this->state.walk.flow.isBeforeSuper() && this->facts.checkThisReady(flow).isErr();
  if (isEarly) {
    auto unset = this->facts.firstUnset(flow);
    auto message = ErrorTextSuperMethod::early(member->name, unset.value());

    return err(CodeClassesRegistryObjectCompleteError{
        .code = CodeClassesRegistryObjectCompleteErrorCode::CALLED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(member);
}

SuperCalls::ResultParentHasMethods SuperCalls::checkParentMethods(const ClassDecl *parent,
                                                                  llvm::StringRef name) const {
  auto methods = parent->findMethodsNamed(name);
  if (methods.empty() && parent->findField(name) != nullptr) {
    auto message = ErrorTextSuperMethod::isField(name, parent->name);

    return err(CodeClassesRegistryParentMethodsError{
        .code = CodeClassesRegistryParentMethodsErrorCode::FIELD_OF_PARENT,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextSuperMethod::noMethod(parent->name, name);

    return err(CodeClassesRegistryParentMethodsError{
        .code = CodeClassesRegistryParentMethodsErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(methods);
}

SuperCalls::ResultMethodHasItsBody SuperCalls::checkMethodBody(FuncSymbol *function,
                                                               llvm::StringRef name) const {
  if (function->decl->isAbstract()) {
    auto message = ErrorTextSuperMethod::abstract(name, function->decl->cls()->name);

    return err(CodeClassesRegistryMethodBodyError{
        .code = CodeClassesRegistryMethodBodyErrorCode::ABSTRACT_METHOD,
        .message = message,
    });
  }

  return ok(function);
}

FunctionDecl *SuperCalls::enclosingMethod() {
  auto functions = this->state.walk.functions.getFunctions();
  for (FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (!functionDecl->isArrow()) {
      return this->methodWithThis(functionDecl);
    }
  }

  return nullptr;
}

FunctionDecl *SuperCalls::methodWithThis(FunctionDecl *functionDecl) const {
  bool isMethod = functionDecl->takesThis() && functionDecl->cls()->literal() == nullptr;

  return isMethod ? functionDecl : nullptr;
}

}
