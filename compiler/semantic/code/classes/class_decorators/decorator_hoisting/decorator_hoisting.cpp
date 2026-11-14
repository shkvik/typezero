#include "compiler/semantic/code/classes/class_decorators/decorator_hoisting/decorator_hoisting.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context_errors.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";

}

DecoratorHoisting::DecoratorHoisting(SemanticState &owner, DecoratorRules &sharedDecoratorRules)
    : state(owner),
      decoratorRules(sharedDecoratorRules) {}

void DecoratorHoisting::makeMethodDescriptor(const MakeMethodDescriptor &task) {
  const Type *descriptor = this->methodDescriptorType(task.params, task.returned);

  return this->state.walk.tasks.pushAnswer(descriptor);
}

const Type *DecoratorHoisting::methodDescriptorType(const Type *args, const Type *returnType) {
  const auto *value = Type::function({Type::unknown(), args}, 1, true, returnType, true);
  const auto *flag = Type::boolTy();
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  auto writableName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_WRITABLE);
  auto enumerableName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_ENUMERABLE);
  auto configurableName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_CONFIGURABLE);
  auto fields = std::vector<Field>{
      {.name = valueName.str(), .type = value},
      {.name = writableName.str(), .type = flag},
      {.name = enumerableName.str(), .type = flag},
      {.name = configurableName.str(), .type = flag},
  };
  auto descriptorName = TypeNameTable::getName(TypeName::METHOD_DESCRIPTOR);
  std::string typeName =
      descriptorName.str() + "<" + args->name().str() + ", " + returnType->name().str() + ">";
  const auto *type = Type::object(fields, typeName);
  this->state.code.decorators.rememberDescriptorOrigin(type, args, returnType);

  return type;
}

void DecoratorHoisting::decorate(const Decorate &task) {
  const DecorateRun &run = task.run;
  auto plain = this->checkMemberDecoratable(run.field, run.method);
  if (plain.isErr()) {
    return this->state.diagnostics.error(run.at, plain.error().message);
  }

  return this->nextHoistedDecorator(NextHoistedDecorator{.run = run});
}

void DecoratorHoisting::nextHoistedDecorator(const NextHoistedDecorator &task) {
  auto run = task.run;
  while (run.index < run.uses.size()) {
    Expr *expr = run.uses[run.index].decorator->expr;
    if (this->isMetadataCall(expr)) {
      run.callees.push_back(nullptr);
      run.index += 1;
      continue;
    }
    if (!this->isPlainReference(expr)) {
      return this->startHoistedDecorator(std::move(run));
    }
    run.callees.push_back(expr);
    run.index += 1;
  }
  run.index = 0;

  return this->afterHoistedDecorators(std::move(run));
}

void DecoratorHoisting::startHoistedDecorator(DecorateRun run) {
  const DecoratorUse &use = run.uses[run.index];
  Expr *expr = use.decorator->expr;
  llvm::SMLoc loc = use.decorator->loc;
  auto &tasks = this->state.walk.tasks;
  if (this->isAskingMethodReturn(run, use, expr)) {
    FuncSymbol *method = run.method->sym;
    tasks.pushTask(AfterFactoryMethodReturn{.run = std::move(run)});

    return tasks.pushTask(ReturnTypeOf{.function = method});
  }
  llvm::StringRef hidden = this->state.ast.hiddenName();
  auto *varDecl = this->state.ast.make<VarDeclStmt>(loc, true);
  VarDeclarator declarator{.name = hidden, .loc = loc, .init = expr};
  const auto *hint = this->decoratorRules.isParamUse(use) ? nullptr : this->factoryHint(run, expr);
  varDecl->decls.push_back(std::move(declarator));
  run.classDecl->decorations.push_back(varDecl);
  auto *callee = this->state.ast.make<NameExpr>(loc, hidden);
  run.callees.push_back(callee);
  run.index += 1;
  tasks.pushTask(NextHoistedDecorator{.run = std::move(run)});
  if (hint == nullptr) {
    return tasks.pushTask(WalkStmt{.stmt = varDecl});
  }
  tasks.pushTask(DeclareHoistedDecorator{.varDecl = varDecl});

  return tasks.pushTask(CheckExprSlot{.slot = &varDecl->decls[0].init, .expected = hint});
}

void DecoratorHoisting::afterFactoryMethodReturn(const AfterFactoryMethodReturn &task) {
  auto run = task.run;
  run.methodReturn = this->state.walk.tasks.takeAnswer();
  run.isMethodReturnKnown = true;

  return this->startHoistedDecorator(std::move(run));
}

bool DecoratorHoisting::isAskingMethodReturn(const DecorateRun &run, const DecoratorUse &use,
                                             const Expr *expr) const {
  bool isUnknown =
      !run.isMethodReturnKnown && run.earlyDescriptor == nullptr && run.method != nullptr;
  if (!isUnknown || this->decoratorRules.isParamUse(use)) {
    return false;
  }

  return this->genericFactory(run, expr) != nullptr;
}

void DecoratorHoisting::declareHoistedDecorator(const DeclareHoistedDecorator &task) {
  VarDeclarator &declarator = task.varDecl->decls[0];

  return this->state.walk.tasks.pushTask(DeclareVariableSlot{
      .slot = &declarator.sym,
      .name = declarator.name,
      .loc = declarator.loc,
      .type = declarator.init->type,
      .isConst = true,
  });
}

void DecoratorHoisting::afterHoistedDecorators(DecorateRun run) {
  auto &tasks = this->state.walk.tasks;
  if (!this->matchDecorators(run)) {
    return;
  }
  auto usable = this->checkGenericDescriptor(run.classDecl, run.hasDescriptor);
  if (usable.isErr()) {
    return this->state.diagnostics.error(run.at, usable.error().message);
  }
  if (!run.hasDescriptor) {
    return this->startDecoratorApplies(std::move(run));
  }
  if (!run.isMethodReturnKnown) {
    FuncSymbol *method = run.method->sym;
    tasks.pushTask(AfterDescriptorMethodReturn{.run = std::move(run)});

    return tasks.pushTask(ReturnTypeOf{.function = method});
  }

  return this->startKnownDescriptor(std::move(run));
}

void DecoratorHoisting::afterDescriptorMethodReturn(const AfterDescriptorMethodReturn &task) {
  auto run = task.run;
  run.methodReturn = this->state.walk.tasks.takeAnswer();
  run.isMethodReturnKnown = true;

  return this->startKnownDescriptor(std::move(run));
}

void DecoratorHoisting::startKnownDescriptor(DecorateRun run) {
  if (!this->hasKnownSignature(run.method->sym, run.methodReturn)) {
    return;
  }

  return this->startDescriptor(std::move(run));
}

void DecoratorHoisting::startDescriptor(DecorateRun run) {
  auto &tasks = this->state.walk.tasks;
  run.descriptorType = this->methodDescriptor(run);
  auto *implementation = this->declareMethodImpl(run);
  run.method->member()->impl = implementation;
  auto *literal = this->descriptorLiteral(run, implementation);
  llvm::StringRef hidden = this->state.ast.hiddenName();
  auto *varDecl = this->state.ast.make<VarDeclStmt>(run.at, false);
  VarDeclarator declarator{.name = hidden, .loc = run.at, .init = literal};
  varDecl->decls.push_back(std::move(declarator));
  run.classDecl->decorations.push_back(varDecl);
  VarDeclarator &descriptor = varDecl->decls[0];
  const Type *descriptorType = run.descriptorType;
  llvm::SMLoc at = run.at;
  tasks.pushTask(AfterDescriptorDeclared{.run = std::move(run), .varDecl = varDecl});
  tasks.pushTask(DeclareVariableSlot{
      .slot = &descriptor.sym,
      .name = hidden,
      .loc = at,
      .type = descriptorType,
      .isConst = false,
  });
  tasks.pushTask(CoerceSlot{.slot = &descriptor.init, .to = descriptorType, .what = ""});

  return tasks.pushTask(CheckExprSlot{.slot = &descriptor.init, .expected = descriptorType});
}

ObjectLitExpr *DecoratorHoisting::descriptorLiteral(const DecorateRun &run,
                                                    const VarSymbol *implementation) {
  llvm::SMLoc at = run.at;
  auto *literal = this->state.ast.make<ObjectLitExpr>(at);
  Expr *value = this->state.ast.make<NameExpr>(at, implementation->name);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_VALUE, value, at);
  Expr *writable = this->state.ast.make<BoolLitExpr>(at, true);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_WRITABLE, writable, at);
  Expr *enumerable = this->state.ast.make<BoolLitExpr>(at, false);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_ENUMERABLE, enumerable, at);
  Expr *configurable = this->state.ast.make<BoolLitExpr>(at, true);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_CONFIGURABLE, configurable, at);

  return literal;
}

void DecoratorHoisting::afterDescriptorDeclared(const AfterDescriptorDeclared &task) {
  auto run = task.run;
  run.descriptorVar = task.varDecl->decls[0].sym;
  run.descriptorVar->reassigned = true;

  return this->startDecoratorApplies(std::move(run));
}

void DecoratorHoisting::startDecoratorApplies(DecorateRun run) {
  auto &tasks = this->state.walk.tasks;
  const auto *constructor = this->designConstructor(run);
  auto records = WriteDesignRecords{
      .classDecl = run.classDecl,
      .field = run.field,
      .method = run.method,
      .constructor = constructor,
      .loc = run.at,
  };
  run.index = run.uses.size();
  tasks.pushTask(NextAppliedDecorator{.run = std::move(run)});

  return tasks.pushTask(std::move(records));
}

size_t DecoratorHoisting::argumentsOf(DecoratorPlace place) const {
  switch (place) {
    case DecoratorPlace::CLASS:
      return 1;
    case DecoratorPlace::FIELD:
      return 2;
    case DecoratorPlace::METHOD:
    case DecoratorPlace::PARAMETER:
      return 3;
  }

  return 3;
}

std::string DecoratorHoisting::argumentsText(DecoratorPlace place) const {
  switch (place) {
    case DecoratorPlace::CLASS:
      return ErrorTextDecoratorPlace::classArgs();
    case DecoratorPlace::FIELD:
      return ErrorTextDecoratorPlace::fieldArgs();
    case DecoratorPlace::METHOD:
      return ErrorTextDecoratorPlace::methodArgs();
    case DecoratorPlace::PARAMETER:
      return ErrorTextDecoratorPlace::paramArgs();
  }

  return ErrorTextDecoratorPlace::paramArgs();
}

bool DecoratorHoisting::isPlainReference(const Expr *expr) const {
  const Expr *link = expr;
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    link = member->object;
    member = dyn_cast<MemberExpr>(link);
  }

  return isa<NameExpr>(link);
}

std::optional<DecoratorHoisting::KnownParams> DecoratorHoisting::knownParams(Expr *callee) {
  const auto *name = dyn_cast<NameExpr>(callee);
  if (name != nullptr) {
    return this->namedParams(name);
  }
  const auto *member = dyn_cast<MemberExpr>(callee);
  if (member != nullptr) {
    const auto *function = this->staticMethodSymbol(member);
    return this->functionParams(function);
  }

  return std::nullopt;
}

std::optional<DecoratorHoisting::KnownParams> DecoratorHoisting::namedParams(const NameExpr *name) {
  auto entry = this->state.walk.scopes.findName(name->name);
  if (entry.var() != nullptr) {
    return this->typeParams(entry.var()->type);
  }

  return this->functionParams(entry.func());
}

std::optional<DecoratorHoisting::KnownParams>
DecoratorHoisting::typeParams(const Type *type) const {
  if (type == nullptr || !type->isFunction() || type->hasRest()) {
    return std::nullopt;
  }
  auto params = type->params();

  return KnownParams{.params = params.vec(), .required = type->minArgs()};
}

std::optional<DecoratorHoisting::KnownParams>
DecoratorHoisting::functionParams(const FuncSymbol *function) const {
  if (function == nullptr || function->hasRest || llvm::is_contained(function->params, nullptr)) {
    return std::nullopt;
  }

  return KnownParams{.params = function->params, .required = function->minArgs};
}

const FuncSymbol *DecoratorHoisting::staticMethodSymbol(const MemberExpr *member) {
  const auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr) {
    return nullptr;
  }
  ClassDecl *classDecl = this->state.walk.scopes.findName(object->name).cls();
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findStaticMethodsNamed(member->name);

  return methods.size() == 1 ? methods[0]->sym : nullptr;
}

const Type *DecoratorHoisting::thirdParam(const std::optional<KnownParams> &known) const {
  if (!known.has_value() || known->params.size() < 3) {
    return nullptr;
  }

  return known->params[2]->nonOptional();
}

DecoratorHoisting::ResultPlainMemberFound
DecoratorHoisting::checkMemberDecoratable(const ClassField *field,
                                          const FunctionDecl *method) const {
  if (field != nullptr && field->accessor) {
    auto message = ErrorTextDecorate::accessor();

    return err(CodeClassesRegistryMemberDecoratableError{
        .code = CodeClassesRegistryMemberDecoratableErrorCode::ACCESSOR_FIELD,
        .message = message,
    });
  }
  if (method != nullptr && method->isAbstract()) {
    auto message = ErrorTextDecorate::abstract();

    return err(CodeClassesRegistryMemberDecoratableError{
        .code = CodeClassesRegistryMemberDecoratableErrorCode::ABSTRACT_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

DecoratorHoisting::ResultDecoratorArgsFit
DecoratorHoisting::checkDecoratorFits(llvm::StringRef label, DecoratorPlace place, size_t needs,
                                      size_t given, const Type *third) const {
  if (needs > given) {
    std::string needed = std::to_string(needs);
    std::string gets = std::to_string(given);
    auto where = this->decoratorRules.placeText(place);
    auto args = this->argumentsText(place);
    auto message = ErrorTextDecorate::arguments(label, where, needed, gets, args);

    return err(CodeClassesRegistryDecoratorFitsError{
        .code = CodeClassesRegistryDecoratorFitsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  if (third != nullptr && place == DecoratorPlace::PARAMETER && third->isObject() &&
      third->field(valueName) != nullptr) {
    auto message = ErrorTextDecorate::descriptor(label);

    return err(CodeClassesRegistryDecoratorFitsError{
        .code = CodeClassesRegistryDecoratorFitsErrorCode::DESCRIPTOR_FOR_PARAMETER,
        .message = message,
    });
  }
  if (third != nullptr && place == DecoratorPlace::METHOD && third->isNumeric()) {
    auto message = ErrorTextDecorate::position(label);

    return err(CodeClassesRegistryDecoratorFitsError{
        .code = CodeClassesRegistryDecoratorFitsErrorCode::POSITION_FOR_METHOD,
        .message = message,
    });
  }

  return ok(needs);
}

DecoratorHoisting::ResultDescriptorUsable
DecoratorHoisting::checkGenericDescriptor(const ClassDecl *classDecl, bool hasDescriptor) const {
  if (hasDescriptor && classDecl->isGeneric()) {
    auto message = ErrorTextDecorate::generic(classDecl->name);

    return err(CodeClassesRegistryGenericDescriptorError{
        .code = CodeClassesRegistryGenericDescriptorErrorCode::GENERIC_CLASS_DESCRIPTOR,
        .message = message,
    });
  }

  return ok(classDecl);
}

bool DecoratorHoisting::hasKnownSignature(const FuncSymbol *function,
                                          const Type *returnType) const {
  return returnType != nullptr && !returnType->isError() &&
         !llvm::is_contained(function->params, nullptr);
}

bool DecoratorHoisting::isMetadataCall(const Expr *expr) const {
  const auto *call = dyn_cast<CallExpr>(expr);
  const auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (member == nullptr) {
    return false;
  }
  auto function = ReflectFunctionTable::find(member->name);

  return function == ReflectFunction::METADATA &&
         this->state.isGlobalName(member->object, GlobalName::REFLECT);
}

const Type *DecoratorHoisting::methodDescriptor(DecorateRun &run) {
  if (run.earlyDescriptor != nullptr || run.method == nullptr) {
    return run.earlyDescriptor;
  }
  FuncSymbol *function = run.method->sym;
  if (!this->hasKnownSignature(function, run.methodReturn)) {
    return nullptr;
  }
  const auto *args = Type::tuple(function->params);
  run.earlyDescriptor = this->methodDescriptorType(args, run.methodReturn);

  return run.earlyDescriptor;
}

const Type *DecoratorHoisting::factoryHint(DecorateRun &run, const Expr *expr) {
  if (this->genericFactory(run, expr) == nullptr) {
    return nullptr;
  }
  const auto *descriptorType = this->methodDescriptor(run);
  if (descriptorType == nullptr) {
    return nullptr;
  }
  const Type *classRef = Type::classRef(run.classDecl, run.classDecl->name);

  return Type::function({classRef, Type::string(), descriptorType}, 3, false, Type::voidTy());
}

const FuncSymbol *DecoratorHoisting::genericFactory(const DecorateRun &run,
                                                    const Expr *expr) const {
  const auto *call = dyn_cast<CallExpr>(expr);
  const auto *name = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  bool isFactoryShape = name != nullptr && name->typeArgs.empty() &&
                        run.ownPlace == DecoratorPlace::METHOD && !run.classDecl->isGeneric();
  if (!isFactoryShape) {
    return nullptr;
  }
  const FuncSymbol *factory = this->state.walk.scopes.findName(name->name).func();
  if (factory == nullptr || factory->decl == nullptr || factory->decl->typeParams.empty()) {
    return nullptr;
  }

  return factory;
}

bool DecoratorHoisting::matchDecorators(DecorateRun &run) {
  bool isFitting = true;
  for (size_t index = 0; index < run.uses.size(); ++index) {
    isFitting &= this->matchDecorator(run, index);
  }

  return isFitting;
}

bool DecoratorHoisting::matchDecorator(DecorateRun &run, size_t index) {
  if (run.callees[index] == nullptr) {
    run.takes.push_back(0);
    return true;
  }
  const DecoratorUse &use = run.uses[index];
  DecoratorPlace place = this->decoratorRules.usePlace(run, use);
  size_t given = this->argumentsOf(place);
  auto known = this->knownParams(run.callees[index]);
  size_t count = known.has_value() ? known->params.size() : given;
  size_t needs = known.has_value() ? std::min(known->required, count) : given;
  auto label = this->decoratorRules.decoratorLabel(use.decorator->expr);
  const auto *third = this->thirdParam(known);
  auto fits = this->checkDecoratorFits(label, place, needs, given, third);
  if (fits.isErr()) {
    this->state.diagnostics.error(use.decorator->loc, fits.error().message);
  }
  run.takes.push_back(std::min(count, given));
  run.hasDescriptor |= place == DecoratorPlace::METHOD && count >= 3;

  return fits.isOk();
}

VarSymbol *DecoratorHoisting::declareMethodImpl(const DecorateRun &run) {
  const FunctionDecl *method = run.method;
  auto *implementation = this->state.ast.newVar();
  std::string implName = kHiddenPrefix.str() + run.classDecl->name.str() + "." +
                         method->name.str() + "." + std::to_string(method->variant());
  implementation->name = this->state.ast.keepName(implName);
  implementation->loc = method->loc();
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  implementation->type = run.descriptorType->field(valueName)->type;
  implementation->place = GlobalVar{.usedInFunction = true};
  implementation->reassigned = true;
  auto added =
      this->state.walk.scopes.declare(implementation->name, {VarEntry{.var = implementation}});
  if (added.isErr()) {
    this->state.diagnostics.error(implementation->loc, added.error().message);
  }
  if (added.isErr() && added.error().code != ScopeStackContextDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->state.walk.scopes.findName(implementation->name);
    auto declaredHere = ErrorTextDeclareName::previous();
    this->state.diagnostics.note(previousEntry.loc(), declaredHere);
  }

  return implementation;
}

void DecoratorHoisting::addProperty(ObjectLitExpr *literal, BuiltinProperty property, Expr *value,
                                    llvm::SMLoc at) const {
  auto name = BuiltinPropertyTable::getName(property);
  literal->props.push_back({.name = name, .loc = at, .value = value});
}

const FunctionDecl *DecoratorHoisting::designConstructor(const DecorateRun &run) const {
  if (run.ownPlace != DecoratorPlace::CLASS) {
    return nullptr;
  }
  const auto &constructors = run.classDecl->constructors;
  if (constructors.size() == 1) {
    return constructors[0];
  }
  auto decorated = this->decoratorRules.decoratedConstructors(run.classDecl);

  return decorated.size() == 1 ? decorated[0] : nullptr;
}

}
