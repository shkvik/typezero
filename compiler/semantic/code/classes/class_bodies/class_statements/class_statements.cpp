#include "compiler/semantic/code/classes/class_bodies/class_statements/class_statements.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker_errors.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr const char *kAnonymousClassName = "(anonymous class)";

}

ClassStatements::ClassStatements(SemanticState &owner, ClassFacts &sharedFacts)
    : facts(sharedFacts),
      state(owner) {}

void ClassStatements::checkClassStmt(const CheckClassStmt &task) {
  ClassDecl *classDecl = task.classDecl;
  auto declared = this->checkTopLevelClass(classDecl);
  if (declared.isErr()) {
    return this->state.diagnostics.error(classDecl->loc(), declared.error().message);
  }
  auto &tasks = this->state.walk.tasks;
  if (classDecl->generic() == nullptr) {
    tasks.pushTask(DecorateClass{.classDecl = classDecl});
  }
  tasks.pushTask(WalkStaticFields{.classDecl = classDecl});
  if (classDecl->baseExpr != nullptr && classDecl->stage == ClassStage::UNRESOLVED) {
    return this->startLateClass(classDecl);
  }
}

ClassStatements::ResultClassDeclared
ClassStatements::checkTopLevelClass(ClassDecl *classDecl) const {
  bool isTopLevel = this->state.walk.scopes.isTopLevel() &&
                    !this->state.walk.functions.isInsideFunction() && classDecl->type != nullptr;
  if (!isTopLevel) {
    auto message = ErrorTextClassStmt::nested(classDecl->name);

    return err(CodeClassesRegistryCheckClassStmtError{
        .code = CodeClassesRegistryCheckClassStmtErrorCode::NOT_TOP_LEVEL,
        .message = message,
    });
  }

  return ok(classDecl);
}

void ClassStatements::checkClassExpr(const CheckClassExpr &task) {
  ClassExpr *expr = task.classExpr;
  ClassDecl *classDecl = expr->decl;
  this->state.walk.tasks.pushTask(AfterLateClassExpr{.classExpr = expr});
  if (classDecl->type == nullptr) {
    return this->startLateClass(classDecl);
  }
}

void ClassStatements::afterLateClassExpr(const AfterLateClassExpr &task) {
  ClassExpr *expr = task.classExpr;
  ClassDecl *classDecl = expr->decl;
  auto &tasks = this->state.walk.tasks;
  classDecl->usedAsValue = true;
  tasks.pushTask(FinishClassExpr{.classExpr = expr});
  if (classDecl->declared) {
    return;
  }
  tasks.pushTask(DecorateClass{.classDecl = classDecl});

  return tasks.pushTask(WalkStaticFields{.classDecl = classDecl});
}

void ClassStatements::finishClassExpr(const FinishClassExpr &task) {
  ClassExpr *expr = task.classExpr;
  expr->type = this->facts.valueTypeOf(expr->decl);

  return this->state.walk.tasks.pushChecked(expr);
}

void ClassStatements::walkStaticFields(const WalkStaticFields &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &classContext = this->state.walk.self;
  classContext.enterClass(classDecl);
  classContext.enterStaticInit();
  classDecl->declared = true;
  classContext.forgetSetStatics();
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(LeaveStaticFields{});
  for (ClassField &field : llvm::reverse(classDecl->fields)) {
    if (field.isStatic() && field.var() != nullptr) {
      tasks.pushTask(WalkStaticField{.field = &field});
    }
  }
}

void ClassStatements::leaveStaticFields() {
  auto &classContext = this->state.walk.self;
  classContext.leaveStaticInit();

  return classContext.leaveClass();
}

void ClassStatements::walkStaticField(const WalkStaticField &task) {
  ClassField &field = *task.field;
  VarSymbol *variable = field.var();
  if (field.init == nullptr) {
    if (variable->type == nullptr) {
      variable->type = Type::error();
    }
    return this->state.walk.self.markStaticSet(variable);
  }
  const Type *declared = this->declaredStaticType(field);
  const Type *expected = declared != nullptr && !declared->isError() ? declared : nullptr;
  this->state.walk.tasks.pushTask(AfterStaticField{.field = &field, .declared = declared});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &field.init, .expected = expected});
}

void ClassStatements::afterStaticField(const AfterStaticField &task) {
  ClassField &field = *task.field;
  VarSymbol *variable = field.var();
  const Type *declared = task.declared;
  this->state.walk.self.markStaticSet(variable);
  if (declared != nullptr) {
    auto label = ErrorTextStaticType::fieldLabel(field.name);
    return this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &field.init, .to = declared, .what = label});
  }
  auto known = this->staticType(field, field.init->type);
  if (known.isErr()) {
    this->state.diagnostics.error(field.init->loc(), known.error().message);
  }
  const Type *type = known.isOk() ? known.value() : Type::error();
  field.resolved = type;
  variable->type = type;
}

void ClassStatements::startLateClass(ClassDecl *classDecl) {
  classDecl->checkOnly = this->state.library.reflection.isOnlyChecked();
  this->registerLateClass(classDecl);
  this->rememberLateScope(classDecl);
  this->state.walk.tasks.pushTask(AfterLateBase{.classDecl = classDecl});
  if (classDecl->baseExpr != nullptr) {
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &classDecl->baseExpr, .expected = nullptr});
  }
}

void ClassStatements::afterLateBase(const AfterLateBase &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  ClassDecl *parent = this->lateParent(classDecl);
  if (parent != nullptr) {
    this->attachLateParent(classDecl, parent);
  }
  this->nameAnonymousClass(classDecl);
  this->state.code.late.enterDeclaringClass(classDecl);
  this->state.walk.self.enterClass(classDecl);
  tasks.pushTask(FinishLateClass{.classDecl = classDecl});
  tasks.pushTask(LeaveDeclaringClass{});
  tasks.pushTask(DeclareMethods{.classDecl = classDecl});
  tasks.pushTask(ResolveClass{.classDecl = classDecl});
  if (parent != nullptr && !classDecl->checkOnly) {
    return tasks.pushTask(AddBase{.type = classDecl->type, .base = parent->type});
  }
}

ClassDecl *ClassStatements::lateParent(ClassDecl *classDecl) {
  if (classDecl->baseExpr == nullptr) {
    return nullptr;
  }
  auto base = this->baseClass(classDecl->baseExpr->type);
  bool isReported =
      base.isErr() && base.error().code == CodeClassesRegistryBaseClassErrorCode::ALREADY_REPORTED;
  if (base.isErr() && !isReported) {
    this->state.diagnostics.error(classDecl->baseExpr->loc(), base.error().message);
  }
  classDecl->base.reset();

  return base.isOk() ? base.value() : nullptr;
}

void ClassStatements::attachLateParent(ClassDecl *classDecl, ClassDecl *parent) {
  classDecl->parent = parent;
  if (classDecl->checkOnly) {
    const_cast<Type *>(classDecl->type)->addBase(parent->type);
    return;
  }
  parent->children.push_back(classDecl);
}

void ClassStatements::leaveDeclaringClass() {
  this->state.walk.self.leaveClass();

  return this->state.code.late.leaveDeclaringClass();
}

void ClassStatements::finishLateClass(const FinishLateClass &task) {
  ClassDecl *classDecl = task.classDecl;
  this->rememberMethodModules(classDecl);
  this->markClassOverrides(classDecl);
  auto &late = this->state.code.late;
  auto *instance = this->state.generics.copies.getCheckingInstance();
  if (instance != nullptr && instance->generic != nullptr) {
    late.rememberLateOwner(classDecl, instance);
  }
  late.addLateClass(classDecl);
  if (!this->state.declaration.classes.hasLinkedClasses()) {
    return;
  }
  this->state.walk.tasks.pushTask(BuildVtable{.classDecl = classDecl});

  return this->state.walk.tasks.pushTask(StartLinkOverrides{.classDecl = classDecl});
}

void ClassStatements::rememberMethodModules(const ClassDecl *classDecl) {
  auto *home = this->state.declaration.modules.findDeclModule(classDecl);
  if (home == nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->memberFunctions(classDecl)) {
    this->state.declaration.modules.rememberDeclModule(functionDecl, home);
  }
}

std::vector<FunctionDecl *> ClassStatements::memberFunctions(const ClassDecl *classDecl) const {
  std::vector<FunctionDecl *> methods = classDecl->methods;
  methods.insert(methods.end(), classDecl->constructors.begin(), classDecl->constructors.end());
  if (classDecl->init != nullptr) {
    methods.push_back(classDecl->init);
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

void ClassStatements::resolveConstructorType(const ResolveConstructorType &task) {
  const TypeRef &ref = *task.ref;
  auto argument = this->checkConstructorTypeArgument(ref);
  if (argument.isErr()) {
    this->state.diagnostics.error(ref.loc, argument.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(AfterConstructorTypeArgument{.ref = task.ref});

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE});
}

ClassStatements::ResultConstructorArgument
ClassStatements::checkConstructorTypeArgument(const TypeRef &ref) const {
  if (ref.args.size() != 1) {
    auto message = ErrorTextConstructorType::typeArgs();

    return err(CodeClassesRegistryConstructorTypeError{
        .code = CodeClassesRegistryConstructorTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

void ClassStatements::afterConstructorTypeArgument(const AfterConstructorTypeArgument &task) {
  const TypeRef &ref = *task.ref;
  const Type *made = this->state.walk.tasks.takeAnswer();
  if (made->isError()) {
    return this->state.walk.tasks.pushAnswer(made);
  }
  auto constructed = this->checkConstructedObject(made);
  if (constructed.isErr()) {
    this->state.diagnostics.error(ref.args[0].loc, constructed.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *constructor = Type::constructorOf(made);

  return this->state.walk.tasks.pushAnswer(constructor);
}

ClassStatements::ResultConstructedObject
ClassStatements::checkConstructedObject(const Type *made) const {
  bool isObjectType = (made->isObject() && !made->isTuple()) || made->isParam();
  if (!isObjectType) {
    auto message = ErrorTextConstructorType::notObject(made->name());

    return err(CodeClassesRegistryConstructorTypeError{
        .code = CodeClassesRegistryConstructorTypeErrorCode::NOT_AN_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(made);
}

void ClassStatements::checkConstructorArgs(const CheckConstructorArgs &task) {
  auto args = this->constructorArgsList();
  if (args.isErr()) {
    this->state.diagnostics.error(task.ref->loc, args.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }

  return this->state.walk.tasks.pushAnswer(args.value());
}

ClassStatements::ResultConstructorArgsList ClassStatements::constructorArgsList() const {
  const auto *classDecl = this->state.code.late.getDeclaringClass();
  if (classDecl == nullptr || classDecl->parent == nullptr) {
    auto message = ErrorTextConstructorArgs::subclass();

    return err(CodeClassesRegistryConstructorArgsError{
        .code = CodeClassesRegistryConstructorArgsErrorCode::NOT_IN_SUBCLASS,
        .message = message,
    });
  }
  auto constructors = this->facts.constructorsOf(classDecl->parent);
  if (constructors.empty()) {
    const auto *none = Type::tuple({});

    return ok(none);
  }
  auto parent = classDecl->parent->type->name().str();
  if (constructors.size() > 1) {
    auto message = ErrorTextConstructorArgs::several(parent);

    return err(CodeClassesRegistryConstructorArgsError{
        .code = CodeClassesRegistryConstructorArgsErrorCode::SEVERAL_CONSTRUCTORS,
        .message = message,
    });
  }
  FuncSymbol *function = constructors[0]->sym;
  if (function == nullptr) {
    auto message = ErrorTextConstructorArgs::later(parent);

    return err(CodeClassesRegistryConstructorArgsError{
        .code = CodeClassesRegistryConstructorArgsErrorCode::DECLARED_LATER,
        .message = message,
    });
  }
  if (llvm::is_contained(function->params, nullptr)) {
    return ok(Type::error());
  }
  llvm::ArrayRef<const Type *> params = function->params;
  const Type *rest = function->hasRest ? params.back() : nullptr;
  auto fixed = function->hasRest ? params.drop_back() : params;
  auto elements = std::vector<const Type *>(fixed.begin(), fixed.end());
  const auto *list = Type::argumentList(elements, function->minArgs, rest);

  return ok(list);
}

ClassStatements::ResultBaseClassUsable ClassStatements::baseClass(const Type *type) const {
  auto *parent = const_cast<ClassDecl *>(type->classRef());
  if (type->isError()) {
    return err(CodeClassesRegistryBaseClassError{
        .code = CodeClassesRegistryBaseClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (type->isConstructor()) {
    auto message = ErrorTextBaseClass::atRun(type->name());

    return err(CodeClassesRegistryBaseClassError{
        .code = CodeClassesRegistryBaseClassErrorCode::KNOWN_ONLY_AT_RUN,
        .message = message,
    });
  }
  if (!type->isClassRef()) {
    auto message = ErrorTextBaseClass::notClass(type->name());

    return err(CodeClassesRegistryBaseClassError{
        .code = CodeClassesRegistryBaseClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }
  if (parent->type == nullptr) {
    auto message = ErrorTextBaseClass::javascript(parent->name);

    return err(CodeClassesRegistryBaseClassError{
        .code = CodeClassesRegistryBaseClassErrorCode::JAVASCRIPT_CLASS,
        .message = message,
    });
  }
  if (parent->isGeneric()) {
    auto message = ErrorTextBaseClass::generic(parent->name);

    return err(CodeClassesRegistryBaseClassError{
        .code = CodeClassesRegistryBaseClassErrorCode::GENERIC_CLASS,
        .message = message,
    });
  }
  if (this->state.library.filesystem.isFileClass(parent)) {
    auto message = ErrorTextBaseClass::madeByFs(parent->name);

    return err(CodeClassesRegistryBaseClassError{
        .code = CodeClassesRegistryBaseClassErrorCode::MADE_BY_FS,
        .message = message,
    });
  }

  return ok(parent);
}

void ClassStatements::registerLateClass(ClassDecl *classDecl) {
  if (classDecl->type == nullptr) {
    classDecl->id = this->state.declaration.classes.takeClassId();
    llvm::StringRef typeName = classDecl->name.empty() ? kAnonymousClassName : classDecl->name;
    classDecl->type = Type::classType(typeName, classDecl);
    this->state.declaration.classes.addClass(classDecl);
  }
  auto *current = this->state.declaration.modules.getCurrentModule();
  if (current != nullptr && !this->state.declaration.modules.hasDeclModule(classDecl)) {
    this->state.declaration.modules.rememberDeclModule(classDecl, current);
  }
}

void ClassStatements::rememberLateScope(ClassDecl *classDecl) {
  const auto *outer = this->innerGenericCopy();
  if (outer != nullptr) {
    classDecl->outer = outer;
  }
  for (llvm::StringRef local : this->state.walk.scopes.getLocalVarNames()) {
    classDecl->locals.push_back(local);
  }
}

const FunctionDecl *ClassStatements::innerGenericCopy() const {
  auto functions = this->state.walk.functions.getFunctions();
  for (const FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (functionDecl->isInstance && !functionDecl->typeArgs.empty()) {
      return functionDecl;
    }
  }

  return nullptr;
}

void ClassStatements::nameAnonymousClass(ClassDecl *classDecl) const {
  if (!classDecl->name.empty() || classDecl->parent == nullptr) {
    return;
  }
  std::string extendsName = "(class extends " + classDecl->parent->type->name().str() + ")";
  const_cast<Type *>(classDecl->type)->rename(extendsName);
}

ClassStatements::ResultStaticTypeKnown ClassStatements::staticType(const ClassField &field,
                                                                   const Type *type) const {
  if (type->isVoid() || type->isUndefined()) {
    auto message = ErrorTextStaticType::unknown(field.name);

    return err(CodeClassesRegistryStaticTypeError{
        .code = CodeClassesRegistryStaticTypeErrorCode::UNKNOWN_FROM_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

const Type *ClassStatements::declaredStaticType(const ClassField &field) const {
  const Type *declared = field.var()->type;
  bool isFromValue = field.resolved != nullptr && !field.type.has_value() && !field.optional();
  if (declared != nullptr && isFromValue) {
    return nullptr;
  }

  return declared;
}

void ClassStatements::markClassOverrides(ClassDecl *classDecl) {
  auto &late = this->state.code.late;
  if (!late.hasLateMethodNames() || classDecl->literal() != nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->facts.ownMethods(classDecl)) {
    Overriding *overriding = functionDecl->overriding();
    if (late.isLateMethodName(functionDecl->name) && overriding != nullptr) {
      overriding->overridden = true;
      overriding->openToLate = true;
    }
  }
}

}
