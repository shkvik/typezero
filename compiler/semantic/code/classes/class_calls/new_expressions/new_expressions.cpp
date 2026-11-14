#include "compiler/semantic/code/classes/class_calls/new_expressions/new_expressions.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

NewExpressions::NewExpressions(SemanticState &owner, ClassFacts &sharedFacts,
                               ClassCallRules &sharedClassCallRules)
    : facts(sharedFacts),
      state(owner),
      classCallRules(sharedClassCallRules) {}

void NewExpressions::checkValueNew(const CheckValueNew &task) {
  NewExpr *expr = task.newExpr;
  expr->type = Type::error();
  this->state.walk.tasks.pushTask(AfterValueNewCallee{.newExpr = expr, .expected = task.expected});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &expr->callee, .expected = nullptr});
}

void NewExpressions::afterValueNewCallee(const AfterValueNewCallee &task) {
  NewExpr *expr = task.newExpr;
  const Type *type = expr->callee->type;
  if (type->isError()) {
    return this->failNew(expr);
  }
  auto classValue = this->checkValueNewClass(type);
  if (classValue.isErr()) {
    this->state.diagnostics.error(expr->callee->loc(), classValue.error().message);
    return this->failNew(expr);
  }
  auto *classDecl = const_cast<ClassDecl *>(type->classRef());
  if (classDecl != nullptr) {
    return this->startClassValueNew(expr, classDecl, task.expected);
  }
  auto plain = this->checkRuntimeTypeArgs(expr);
  if (plain.isErr()) {
    this->state.diagnostics.error(expr->loc(), plain.error().message);
  }
  expr->form = ReflectNew{};
  auto draft = ReflectNewDraft{
      .newExpr = expr,
      .type = type,
      .argTypes = {},
      .index = 0,
  };

  return this->nextReflectedArg(NextReflectedArg{.draft = std::move(draft)});
}

void NewExpressions::failNew(NewExpr *expr) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void NewExpressions::startClassValueNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected) {
  auto typed = this->checkTypedClass(classDecl);
  if (typed.isErr()) {
    this->state.diagnostics.error(expr->callee->loc(), typed.error().message);
    return this->failNew(expr);
  }

  return this->state.walk.tasks.pushTask(
      CheckClassNew{.newExpr = expr, .classDecl = typed.value(), .expected = expected});
}

void NewExpressions::nextReflectedArg(const NextReflectedArg &task) {
  const ReflectNewDraft &draft = task.draft;
  NewExpr *expr = draft.newExpr;
  if (draft.index == expr->args.size()) {
    return this->finishReflectNew(draft);
  }
  this->state.walk.tasks.pushTask(AfterReflectedArg{.draft = draft});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &expr->args[draft.index], .expected = nullptr});
}

void NewExpressions::afterReflectedArg(const AfterReflectedArg &task) {
  auto draft = task.draft;
  Expr *&arg = draft.newExpr->args[draft.index];
  draft.argTypes.push_back(arg->type);
  draft.index += 1;
  auto value = this->checkArgumentValue(arg);
  if (value.isErr()) {
    this->state.diagnostics.error(arg->loc(), value.error().message);
    return this->nextReflectedArg(NextReflectedArg{.draft = std::move(draft)});
  }
  this->state.walk.tasks.pushTask(NextReflectedArg{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &arg, .to = Type::unknown()});
}

void NewExpressions::finishReflectNew(const ReflectNewDraft &draft) {
  NewExpr *expr = draft.newExpr;
  bool isOnlyChecked = this->state.library.reflection.isOnlyChecked();
  if (!isOnlyChecked) {
    auto argTypes = draft.argTypes;
    this->state.library.reflection.addReflectedNew(expr, std::move(argTypes));
    this->state.library.reflection.markReflectionUsed();
  }
  expr->type = draft.type->constructed();
  this->state.walk.tasks.pushChecked(expr);

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void NewExpressions::checkClassNew(const CheckClassNew &task) {
  NewExpr *expr = task.newExpr;
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  expr->type = Type::error();
  if (classDecl->isGeneric()) {
    return tasks.pushTask(
        CheckGenericNew{.newExpr = expr, .classDecl = classDecl, .expected = task.expected});
  }
  auto possible = this->checkNotRuntime(classDecl);
  if (possible.isErr()) {
    this->state.diagnostics.error(expr->loc(), possible.error().message);
    return this->failNew(expr);
  }
  this->walkNewClass(expr, classDecl);
  auto *call = this->state.ast.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.cls = classDecl, .ctor = call};
  call->type = Type::voidTy();
  auto constructors = this->facts.constructorsOf(classDecl);
  if (!constructors.empty()) {
    tasks.pushTask(AfterConstructorChoice{
        .newExpr = expr,
        .call = call,
        .classDecl = classDecl,
        .first = constructors[0],
    });
    return this->state.walk.tasks.pushTask(ChooseVariant{
        .variants = constructors,
        .args = &call->args,
        .spread = {},
        .generics = GenericVariants::REJECTED,
    });
  }
  auto absent = this->checkArgumentsAbsent(call, classDecl);
  if (absent.isErr()) {
    this->state.diagnostics.error(expr->loc(), absent.error().message);
    tasks.pushChecked(expr);
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }

  return this->finishClassNew(FinishClassNew{.newExpr = expr, .classDecl = classDecl});
}

void NewExpressions::afterConstructorChoice(const AfterConstructorChoice &task) {
  NewExpr *expr = task.newExpr;
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  auto choice = this->state.code.overloads.takeChoice();
  if (choice.function == nullptr) {
    this->classCallRules.reportChoice(choice, expr->loc());
    return tasks.pushChecked(expr);
  }
  FuncSymbol *function = choice.function;
  const FunctionDecl *chosen = function->decl != nullptr ? function->decl : task.first;
  auto constructorName = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto accessible = this->facts.checkAccess(chosen->cls(), chosen->access(), constructorName);
  if (accessible.isErr()) {
    this->state.diagnostics.error(expr->loc(), accessible.error().message);
  }
  call->target = FunctionCall{.func = function};
  tasks.pushTask(FinishClassNew{.newExpr = expr, .classDecl = task.classDecl});
  tasks.pushTask(DiscardChecked{});

  auto arguments =
      this->classCallRules.argumentsTask(call, function, expr->loc(), choice.isChecked);
  return tasks.pushTask(std::move(arguments));
}

void NewExpressions::finishClassNew(const FinishClassNew &task) {
  NewExpr *expr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  expr->type = task.classDecl->type;
  tasks.pushChecked(expr);
  if (this->hasInitializer(task.classDecl)) {
    return tasks.pushTask(CalledFunction{});
  }
}

void NewExpressions::checkConstructorAccess(const CheckConstructorAccess &task) {
  const FunctionDecl *constructor = task.constructor;
  auto constructorName = ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR);
  auto accessible =
      this->facts.checkAccess(constructor->cls(), constructor->access(), constructorName);
  if (accessible.isErr()) {
    return this->state.diagnostics.error(task.newExpr->loc(), accessible.error().message);
  }
}

NewExpressions::ResultNewOfClassValue NewExpressions::checkValueNewClass(const Type *type) const {
  if (!type->isClassRef()) {
    auto message = ErrorTextValueNew::notClass(type->name());

    return err(CodeClassesRegistryValueNewClassError{
        .code = CodeClassesRegistryValueNewClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(type);
}

NewExpressions::ResultClassHasItsType NewExpressions::checkTypedClass(ClassDecl *classDecl) const {
  if (classDecl->type == nullptr) {
    auto message = ErrorTextValueNew::valueOnly(classDecl->name);

    return err(CodeClassesRegistryTypedClassError{
        .code = CodeClassesRegistryTypedClassErrorCode::VALUE_ONLY_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

NewExpressions::ResultNoTypeArguments
NewExpressions::checkRuntimeTypeArgs(const NewExpr *expr) const {
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextValueNew::typeArgs();

    return err(CodeClassesRegistryRuntimeTypeArgsError{
        .code = CodeClassesRegistryRuntimeTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(expr);
}

NewExpressions::ResultArgumentNotVoid NewExpressions::checkArgumentValue(const Expr *arg) const {
  if (arg->type->isVoid()) {
    auto message = ErrorTextValueNew::voidArg();

    return err(CodeClassesRegistryArgumentValueError{
        .code = CodeClassesRegistryArgumentValueErrorCode::VOID_ARGUMENT,
        .message = message,
    });
  }

  return ok(arg);
}

NewExpressions::ResultClassNotRuntime
NewExpressions::checkNotRuntime(const ClassDecl *classDecl) const {
  if (this->state.library.filesystem.isFileClass(classDecl)) {
    auto message = ErrorTextRuntimeNew::onlyFs(classDecl->name);

    return err(CodeClassesRegistryNotRuntimeError{
        .code = CodeClassesRegistryNotRuntimeErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

NewExpressions::ResultMethodCallNoTypeArguments
NewExpressions::checkClassTypeArgs(const NewExpr *expr, const ClassDecl *classDecl) const {
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextClassTypeArgs::notGeneric(classDecl->name);

    return err(CodeClassesRegistryClassTypeArgsError{
        .code = CodeClassesRegistryClassTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(expr);
}

NewExpressions::ResultClassIsDeclared
NewExpressions::checkStaticOwnerDeclared(const ClassDecl *classDecl) const {
  bool isEarly = !classDecl->staticOwner()->declared &&
                 this->state.declaration.modules.getLoadingModule() != nullptr &&
                 !this->state.walk.functions.isInsideFunction() &&
                 this->state.declaration.modules.findDeclModule(classDecl) ==
                     this->state.declaration.modules.getLoadingModule();
  if (isEarly) {
    auto message = ErrorTextClassOrder::usedEarly(classDecl->name);

    return err(CodeClassesRegistryClassDeclaredError{
        .code = CodeClassesRegistryClassDeclaredErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(classDecl);
}

NewExpressions::ResultClassIsConcrete
NewExpressions::checkClassConcrete(const ClassDecl *classDecl) const {
  if (classDecl->isAbstract) {
    auto message = ErrorTextAbstractNew::abstract(classDecl->name);

    return err(CodeClassesRegistryClassConcreteError{
        .code = CodeClassesRegistryClassConcreteErrorCode::ABSTRACT_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

NewExpressions::ResultArgumentsAbsent
NewExpressions::checkArgumentsAbsent(const CallExpr *call, const ClassDecl *classDecl) const {
  if (!call->args.empty()) {
    auto message = ErrorTextNoConstructor::noArgs(classDecl->name);

    return err(CodeClassesRegistryArgumentsAbsentError{
        .code = CodeClassesRegistryArgumentsAbsentErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(call);
}

void NewExpressions::walkNewClass(const NewExpr *expr, const ClassDecl *classDecl) {
  auto plain = this->checkClassTypeArgs(expr, classDecl);
  if (plain.isErr()) {
    this->state.diagnostics.error(expr->loc(), plain.error().message);
  }
  auto declared = this->checkStaticOwnerDeclared(classDecl);
  if (declared.isErr()) {
    this->state.diagnostics.error(expr->loc(), declared.error().message);
  }
  auto concrete = this->checkClassConcrete(classDecl);
  if (concrete.isErr()) {
    this->state.diagnostics.error(expr->loc(), concrete.error().message);
  }
}

bool NewExpressions::hasInitializer(const ClassDecl *classDecl) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (current->init != nullptr || !current->constructors.empty()) {
      return true;
    }
  }

  return false;
}

}
