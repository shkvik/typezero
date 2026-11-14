#include "compiler/semantic/declaration/classes/declaration_classes_registry.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/program/library/builtin_names/error_class_table/error_class_table.h"
#include "compiler/program/library/builtin_names/internal_name_table/internal_name_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kInternalPrefix = "%";

}

DeclarationClassesRegistry::DeclarationClassesRegistry(SemanticState &owner)
    : state(owner) {}

bool DeclarationClassesRegistry::hasSameSignature(const FuncSymbol *first,
                                                  const FuncSymbol *second) const {
  if (first == nullptr || second == nullptr) {
    return false;
  }

  return first->params == second->params && first->minArgs == second->minArgs &&
         first->hasRest == second->hasRest;
}

const ClassField *DeclarationClassesRegistry::earlierField(const ClassDecl *classDecl,
                                                           size_t index) const {
  const ClassField &field = classDecl->fields[index];
  auto earlier = llvm::ArrayRef<ClassField>(classDecl->fields).take_front(index);
  for (const ClassField &other : earlier) {
    if (other.name == field.name && other.isStatic() == field.isStatic()) {
      return &other;
    }
  }

  return nullptr;
}

DeclarationClassesRegistry::ResultFieldNameUnique
DeclarationClassesRegistry::uniqueField(const ClassDecl *classDecl, const ClassField &field,
                                        const ClassField *earlier) const {
  if (earlier != nullptr) {
    auto message = ErrorTextUniqueField::duplicate(field.name, classDecl->name);

    return err(DeclarationClassesRegistryUniqueFieldError{
        .code = DeclarationClassesRegistryUniqueFieldErrorCode::DUPLICATE_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultFieldNameIsFree
DeclarationClassesRegistry::fieldOrMethod(const ClassDecl *classDecl, const ClassField &field,
                                          const FunctionDecl *method) const {
  if (method->name == field.name && method->isStatic() == field.isStatic()) {
    auto message = ErrorTextFieldOrMethod::shared(field.name, classDecl->name);

    return err(DeclarationClassesRegistryFieldOrMethodError{
        .code = DeclarationClassesRegistryFieldOrMethodErrorCode::FIELD_AND_METHOD,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationClassesRegistry::walkMemberName(const ClassDecl *classDecl, size_t index) {
  const ClassField &field = classDecl->fields[index];
  const auto *earlier = this->earlierField(classDecl, index);
  auto unique = this->uniqueField(classDecl, field, earlier);
  if (unique.isErr()) {
    auto previous = ErrorTextUniqueField::previous();
    this->state.diagnostics.error(field.loc, unique.error().message);
    this->state.diagnostics.note(earlier->loc, previous);
  }
  for (FunctionDecl *method : classDecl->methods) {
    auto distinct = this->fieldOrMethod(classDecl, field, method);
    if (distinct.isErr()) {
      auto fieldNote = ErrorTextFieldOrMethod::field();
      this->state.diagnostics.error(method->loc(), distinct.error().message);
      this->state.diagnostics.note(field.loc, fieldNote);
    }
  }
}

DeclarationClassesRegistry::ResultFieldNotLateSet
DeclarationClassesRegistry::setLater(const ClassField &field) const {
  if (field.definite()) {
    auto message = ErrorTextSetLater::definite(field.name);

    return err(DeclarationClassesRegistrySetLaterError{
        .code = DeclarationClassesRegistrySetLaterErrorCode::SET_LATER,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultFieldTypeIsSeen
DeclarationClassesRegistry::typeSeen(const ClassField &field) const {
  bool isUntypedValue = !field.type.has_value() && field.init != nullptr &&
                        SharedHelpers::literalType(field.init) == nullptr;
  if (isUntypedValue && field.isStatic()) {
    auto message = ErrorTextTypeSeen::shared(field.name);

    return err(DeclarationClassesRegistryTypeSeenError{
        .code = DeclarationClassesRegistryTypeSeenErrorCode::STATIC_VALUE_NOT_LITERAL,
        .message = message,
    });
  }
  bool isFunction = llvm::isa_and_nonnull<FunctionExpr>(field.init);
  if (isUntypedValue && isFunction && field.method() == nullptr) {
    auto message = ErrorTextTypeSeen::function(field.name);

    return err(DeclarationClassesRegistryTypeSeenError{
        .code = DeclarationClassesRegistryTypeSeenErrorCode::FUNCTION_FIELD_UNTYPED,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultFieldHasNoValue
DeclarationClassesRegistry::fieldValue(const ClassDecl *classDecl, const ClassField &field) const {
  bool isFunction = llvm::isa_and_nonnull<FunctionExpr>(field.init);
  bool isValued = !field.isStatic() && field.init != nullptr && !isFunction;
  if (!isValued) {
    return ok(&field);
  }
  if (!classDecl->constructors.empty()) {
    auto message = ErrorTextDeclaredValue::constructor(field.name);

    return err(DeclarationClassesRegistryFieldValueError{
        .code = DeclarationClassesRegistryFieldValueErrorCode::VALUE_OUTSIDE_CONSTRUCTOR,
        .message = message,
    });
  }
  bool isLiteral = SharedHelpers::literalType(field.init) != nullptr;
  if (!field.type.has_value() || !isLiteral) {
    auto message = ErrorTextDeclaredValue::literal(field.name);

    return err(DeclarationClassesRegistryFieldValueError{
        .code = DeclarationClassesRegistryFieldValueErrorCode::START_VALUE_NOT_LITERAL,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultFieldTypeSource
DeclarationClassesRegistry::typedField(const ClassField &field, const Param *property) const {
  if (property != nullptr && !property->type.has_value()) {
    auto message = ErrorTextTypedField::property(field.name);

    return err(DeclarationClassesRegistryTypedFieldError{
        .code = DeclarationClassesRegistryTypedFieldErrorCode::PROPERTY_NO_TYPE,
        .message = message,
    });
  }
  if (!field.parameter() && !field.type.has_value() && field.init == nullptr) {
    auto message = ErrorTextTypedField::missing(field.name);

    return err(DeclarationClassesRegistryTypedFieldError{
        .code = DeclarationClassesRegistryTypedFieldErrorCode::NO_TYPE,
        .message = message,
    });
  }

  return ok(&field);
}

TypeRef *DeclarationClassesRegistry::writtenFieldRef(ClassField &field, Param *property) const {
  if (property != nullptr) {
    return &*property->type;
  }
  if (field.type.has_value()) {
    return &*field.type;
  }

  return nullptr;
}

const Type *DeclarationClassesRegistry::optionalFieldType(const ClassField &field,
                                                          const Type *written) const {
  if (written != nullptr && field.optional()) {
    return Type::optional(written);
  }

  return written;
}

const Type *DeclarationClassesRegistry::unwrittenFieldType(const ClassField &field) const {
  const auto *literal = SharedHelpers::literalType(field.init);
  bool isFromValueLater = field.isStatic() || llvm::isa<FunctionExpr>(field.init);
  if (literal != nullptr || isFromValueLater) {
    return literal;
  }

  return Type::error();
}

Param *DeclarationClassesRegistry::propertyParam(ClassDecl *classDecl, llvm::StringRef name) const {
  for (FunctionDecl *constructor : classDecl->constructors) {
    for (Param &param : constructor->params) {
      if (param.property && param.name == name) {
        return &param;
      }
    }
  }

  return nullptr;
}

void DeclarationClassesRegistry::arrowMethod(ClassDecl *classDecl, ClassField &field,
                                             const Type *type) {
  auto *function = llvm::dyn_cast_or_null<FunctionExpr>(field.init);
  if (function == nullptr) {
    return;
  }
  FunctionDecl *functionDecl = function->fn;
  if (functionDecl->name.empty()) {
    functionDecl->name = field.name;
  }
  bool isFunctionTyped = type == nullptr || type->isFunction();
  if (!functionDecl->isArrow() || !isFunctionTyped || !this->isMethodField(field)) {
    return;
  }
  functionDecl->name = field.name;
  ClassMember *member = functionDecl->member();
  if (member == nullptr) {
    functionDecl->role =
        ClassMember{.cls = classDecl, .access = field.access, .isOverride = field.isOverride};
  } else {
    member->cls = classDecl;
    member->access = field.access;
    member->isOverride = field.isOverride;
  }
  functionDecl->place = FunctionPlace::DECLARED;
  field.form = ArrowMethodField{.method = functionDecl};
  field.resolved = nullptr;
}

bool DeclarationClassesRegistry::isMethodField(const ClassField &field) const {
  bool isPlainField =
      !field.isStatic() && !field.parameter() && !field.optional() && !field.redeclared;

  return isPlainField && !this->state.flow.assignments.isFieldAssigned(field.name);
}

DeclarationClassesRegistry::ResultStaticTypeFixed
DeclarationClassesRegistry::staticTypeParams(const ClassDecl *classDecl, const ClassField &field,
                                             const Type *type) const {
  if (field.isStatic() && type != nullptr && !classDecl->typeArgs.empty() &&
      TypeQueries::mentionsParam(type)) {
    auto message = ErrorTextStaticParams::shared(field.name, classDecl->name);

    return err(DeclarationClassesRegistryStaticTypeParamsError{
        .code = DeclarationClassesRegistryStaticTypeParamsErrorCode::USES_TYPE_PARAMS,
        .message = message,
    });
  }

  return ok(type);
}

DeclarationClassesRegistry::ResultStaticGetsValue
DeclarationClassesRegistry::staticValue(const ClassField &field, const Type *type) const {
  if (field.init == nullptr && !field.optional() && type != nullptr && !type->isError()) {
    auto message = ErrorTextStaticValue::missing(field.name, type->name());

    return err(DeclarationClassesRegistryStaticValueError{
        .code = DeclarationClassesRegistryStaticValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationClassesRegistry::declareStaticVar(const ClassDecl *classDecl,
                                                  const ClassField &field, StaticField *shared,
                                                  const Type *type) {
  auto *variable = this->state.ast.newVar();
  std::string name = (classDecl->name + "." + field.name).str();
  variable->name = this->state.ast.keepName(name);
  variable->loc = field.loc;
  variable->type = type;
  variable->isConst = field.readonly;
  variable->place = GlobalVar{.usedInFunction = true};
  shared->var = variable;
  auto *literal = field.readonly ? llvm::dyn_cast_or_null<IntLitExpr>(field.init) : nullptr;
  bool isFittingInt =
      literal != nullptr && literal->value <= uint64_t(std::numeric_limits<int64_t>::max());
  if (isFittingInt) {
    this->state.code.constants.rememberConstVar(variable, int64_t(literal->value));
  }
  auto valued = this->staticValue(field, type);
  if (valued.isErr()) {
    this->state.diagnostics.error(field.loc, valued.error().message);
  }
}

void DeclarationClassesRegistry::startClassField(const StartClassField &task) {
  ClassDecl *classDecl = task.classDecl;
  ClassField &field = classDecl->fields[task.index];
  auto early = this->setLater(field);
  if (early.isErr()) {
    this->state.diagnostics.error(field.loc, early.error().message);
  }
  auto valueless = this->fieldValue(classDecl, field);
  if (valueless.isErr()) {
    this->state.diagnostics.error(field.loc, valueless.error().message);
  }
  Param *property = field.parameter() ? this->propertyParam(classDecl, field.name) : nullptr;
  auto source = this->typedField(field, property);
  if (source.isErr()) {
    this->state.diagnostics.error(field.loc, source.error().message);
    const auto *failed = this->optionalFieldType(field, Type::error());

    return this->placeClassField(classDecl, field, failed);
  }
  if (property != nullptr && property->optional()) {
    field.presence = FieldPresence::OPTIONAL;
  }
  TypeRef *written = this->writtenFieldRef(field, property);
  if (written == nullptr) {
    const auto *unwritten = this->unwrittenFieldType(field);
    const auto *type = this->optionalFieldType(field, unwritten);

    return this->placeClassField(classDecl, field, type);
  }
  this->state.walk.tasks.pushTask(
      FinishClassField{.classDecl = classDecl, .index = task.index, .written = written});
  this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = written, .isVoidAllowed = false});
}

void DeclarationClassesRegistry::finishClassField(const FinishClassField &task) {
  ClassField &field = task.classDecl->fields[task.index];
  const auto *type = this->optionalFieldType(field, task.written->resolved);

  return this->placeClassField(task.classDecl, field, type);
}

void DeclarationClassesRegistry::placeClassField(ClassDecl *classDecl, ClassField &field,
                                                 const Type *type) {
  field.resolved = type;
  this->arrowMethod(classDecl, field, type);
  auto seen = this->typeSeen(field);
  if (seen.isErr()) {
    this->state.diagnostics.error(field.loc, seen.error().message);
    type = Type::error();
    field.resolved = type;
  }
  auto fixed = this->staticTypeParams(classDecl, field, type);
  if (fixed.isErr()) {
    this->state.diagnostics.error(field.loc, fixed.error().message);
    type = Type::error();
    field.resolved = type;
  }
  StaticField *shared = field.form.as<StaticField>();
  if (shared != nullptr) {
    this->declareStaticVar(classDecl, field, shared, type);
  }
}

void DeclarationClassesRegistry::resolveClass(const ResolveClass &task) {
  ClassDecl *classDecl = task.classDecl;
  if (classDecl->stage != ClassStage::UNRESOLVED) {
    return;
  }
  classDecl->stage = ClassStage::RESOLVED;
  this->state.walk.tasks.pushTask(OpenClassScope{.classDecl = classDecl});
  this->state.walk.tasks.pushTask(DeclareClassTypeParams{.classDecl = classDecl});
}

void DeclarationClassesRegistry::openClassScope(const OpenClassScope &task) {
  ClassDecl *classDecl = task.classDecl;
  this->state.walk.scopes.openTypeScope();
  this->state.walk.tasks.pushTask(CloseClassScope{});
  this->state.walk.tasks.pushTask(ResolveClassMembers{.classDecl = classDecl});
  this->state.walk.tasks.pushTask(StartClassBases{.classDecl = classDecl});
  this->state.walk.tasks.pushTask(BindClassTypeParams{.classDecl = classDecl});
}

void DeclarationClassesRegistry::closeClassScope() {
  this->state.walk.scopes.closeTypeScope();
}

void DeclarationClassesRegistry::resolveClassMembers(const ResolveClassMembers &task) {
  ClassDecl *classDecl = task.classDecl;
  for (size_t index = 0; index < classDecl->fields.size(); ++index) {
    this->walkMemberName(classDecl, index);
  }
  for (size_t index = classDecl->fields.size(); index > 0; --index) {
    this->state.walk.tasks.pushTask(StartClassField{.classDecl = classDecl, .index = index - 1});
  }
}

DeclarationClassesRegistry::ResultParamTypePassed
DeclarationClassesRegistry::paramType(const Param &param, const Type *type,
                                      const Type *declared) const {
  if (type == nullptr) {
    auto message = ErrorTextParamType::extra(param.label(), declared->name());

    return err(DeclarationClassesRegistryParamTypeError{
        .code = DeclarationClassesRegistryParamTypeErrorCode::MORE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(type);
}

void DeclarationClassesRegistry::startTypedMethod(const StartTypedMethod &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  auto *function = this->state.ast.newFunc();
  function->setCompleter(this->state.completer);
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  functionDecl->sym = function;
  this->state.walk.tasks.pushTask(
      FinishTypedMethod{.functionDecl = functionDecl, .declared = task.declared});
  for (size_t index = functionDecl->params.size(); index > 0; --index) {
    Param &param = functionDecl->params[index - 1];
    this->state.walk.tasks.pushTask(FinishTypedParam{
        .functionDecl = functionDecl,
        .declared = task.declared,
        .index = index - 1,
    });
    if (param.type.has_value()) {
      this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &*param.type, .isVoidAllowed = false});
    }
  }
}

void DeclarationClassesRegistry::finishTypedParam(const FinishTypedParam &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  Param &param = functionDecl->params[task.index];
  const auto &declaredParams = task.declared->params();
  const Type *given = task.index < declaredParams.size() ? declaredParams[task.index] : nullptr;
  const Type *type = this->typedParam(param, given, task.declared);
  param.declaredType = param.defaultValue() != nullptr ? type->nonOptional() : type;
  functionDecl->sym->params.push_back(type);
}

void DeclarationClassesRegistry::finishTypedMethod(const FinishTypedMethod &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  const Type *declared = task.declared;
  FuncSymbol *function = functionDecl->sym;
  const auto &declaredParams = declared->params();
  for (size_t index = functionDecl->params.size(); index < declaredParams.size(); ++index) {
    this->addHiddenParam(functionDecl, declaredParams[index]);
  }
  function->minArgs = declared->minArgs();
  function->hasRest = declared->hasRest();
  function->setReturnType(declared->ret());
  function->discardsResult = declared->ret()->isVoid();
}

const Type *DeclarationClassesRegistry::typedParam(const Param &param, const Type *given,
                                                   const Type *declared) {
  const Type *written = param.type.has_value() ? param.type->resolved : given;
  auto passed = this->paramType(param, written, declared);
  if (passed.isErr()) {
    this->state.diagnostics.error(param.loc, passed.error().message);
  }
  const Type *type = passed.isOk() ? passed.value() : Type::error();
  if (param.optional() && !type->isOptional()) {
    return Type::optional(type);
  }

  return type;
}

void DeclarationClassesRegistry::addHiddenParam(FunctionDecl *functionDecl, const Type *type) {
  Param hidden;
  hidden.name = this->state.ast.hiddenName();
  hidden.loc = functionDecl->loc();
  hidden.declaredType = type;
  functionDecl->params.push_back(hidden);
  functionDecl->sym->params.push_back(type);
}

DeclarationClassesRegistry::ResultAbstractReturns
DeclarationClassesRegistry::abstractReturn(const ClassDecl *classDecl,
                                           const FunctionDecl *method) const {
  if (method->isAbstract() && !method->returnType.has_value() && classDecl->isGeneric()) {
    auto message = ErrorTextAbstractReturn::missing(method->name, classDecl->name);

    return err(DeclarationClassesRegistryAbstractReturnError{
        .code = DeclarationClassesRegistryAbstractReturnErrorCode::NO_RETURN_TYPE,
        .message = message,
    });
  }

  return ok(method);
}

DeclarationClassesRegistry::ResultGenericIsDirect
DeclarationClassesRegistry::genericMethod(const FunctionDecl *method) const {
  bool isGeneric = !method->typeParams.empty();
  if (isGeneric && method->isAbstract()) {
    auto message = ErrorTextGenericMethod::abstract();

    return err(DeclarationClassesRegistryGenericMethodError{
        .code = DeclarationClassesRegistryGenericMethodErrorCode::ABSTRACT,
        .message = message,
    });
  }
  if (isGeneric && method->isOverride()) {
    auto message = ErrorTextGenericMethod::overrides();

    return err(DeclarationClassesRegistryGenericMethodError{
        .code = DeclarationClassesRegistryGenericMethodErrorCode::OVERRIDE,
        .message = message,
    });
  }

  return ok(method);
}

bool DeclarationClassesRegistry::isVariantOf(const FunctionDecl *earlier,
                                             const FunctionDecl *later) const {
  return earlier->name == later->name && earlier->isStatic() == later->isStatic();
}

void DeclarationClassesRegistry::numberVariants(llvm::ArrayRef<FunctionDecl *> functions) const {
  for (size_t index = 0; index < functions.size(); ++index) {
    for (const FunctionDecl *earlier : functions.take_front(index)) {
      if (this->isVariantOf(earlier, functions[index])) {
        functions[index]->member()->variant = earlier->variant() + 1;
      }
    }
  }
}

void DeclarationClassesRegistry::declareMethods(const DeclareMethods &task) {
  ClassDecl *classDecl = task.classDecl;
  this->state.walk.tasks.pushTask(FinishMethods{.classDecl = classDecl});
  for (size_t index = classDecl->fields.size(); index > 0; --index) {
    if (classDecl->fields[index - 1].method() != nullptr) {
      this->state.walk.tasks.pushTask(StartFieldMethod{.classDecl = classDecl, .index = index - 1});
    }
  }
  for (FunctionDecl *method : llvm::reverse(classDecl->methods)) {
    this->state.walk.tasks.pushTask(FinishClassMethod{.classDecl = classDecl, .method = method});
    this->pushSignature(method);
  }
  for (FunctionDecl *constructor : llvm::reverse(classDecl->constructors)) {
    this->state.walk.tasks.pushTask(FinishConstructor{.constructor = constructor});
    this->pushSignature(constructor);
  }
}

void DeclarationClassesRegistry::pushSignature(FunctionDecl *functionDecl) {
  this->state.walk.tasks.pushTask(
      StartFunctionSymbol{.functionDecl = functionDecl, .variable = nullptr, .isNamed = false});
}

void DeclarationClassesRegistry::finishConstructor(const FinishConstructor &task) {
  task.constructor->sym->setReturnType(Type::voidTy());
}

void DeclarationClassesRegistry::finishClassMethod(const FinishClassMethod &task) {
  const ClassDecl *classDecl = task.classDecl;
  FunctionDecl *method = task.method;
  bool isReturnOpen = method->isAbstract() && !method->returnType.has_value();
  auto returns = this->abstractReturn(classDecl, method);
  if (returns.isErr()) {
    this->state.diagnostics.error(method->loc(), returns.error().message);
  }
  if (returns.isOk() && isReturnOpen) {
    method->sym->setReturnType(nullptr);
  }
  auto direct = this->genericMethod(method);
  if (direct.isErr()) {
    this->state.diagnostics.error(method->loc(), direct.error().message);
  }
}

void DeclarationClassesRegistry::startFieldMethod(const StartFieldMethod &task) {
  ClassDecl *classDecl = task.classDecl;
  ClassField &field = classDecl->fields[task.index];
  this->state.walk.scopes.openTypeScope();
  this->state.walk.tasks.pushTask(FinishFieldMethod{.classDecl = classDecl, .index = task.index});
  if (field.type.has_value()) {
    this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &*field.type, .isVoidAllowed = false});
  }
  this->state.walk.tasks.pushTask(BindClassTypeParams{.classDecl = classDecl});
}

void DeclarationClassesRegistry::finishFieldMethod(const FinishFieldMethod &task) {
  const ClassField &field = task.classDecl->fields[task.index];
  FunctionDecl *method = field.method();
  this->state.walk.scopes.closeTypeScope();
  const Type *declared = field.type.has_value() ? field.type->resolved : nullptr;
  if (declared == nullptr || !declared->isFunction()) {
    return this->pushSignature(method);
  }

  return this->startTypedMethod(StartTypedMethod{.functionDecl = method, .declared = declared});
}

void DeclarationClassesRegistry::finishMethods(const FinishMethods &task) {
  ClassDecl *classDecl = task.classDecl;
  this->state.walk.tasks.pushTask(MakeFieldInit{.classDecl = classDecl});
  if (classDecl->findConstructors().empty()) {
    this->state.walk.tasks.pushTask(NextFieldDefault{.classDecl = classDecl, .fieldIndex = 0});
  }
}

void DeclarationClassesRegistry::nextFieldDefault(const NextFieldDefault &task) {
  ClassDecl *classDecl = task.classDecl;
  for (size_t index = task.fieldIndex; index < classDecl->fields.size(); ++index) {
    const ClassField &field = classDecl->fields[index];
    if (this->isNeedingDefault(field)) {
      auto run = this->fieldDefaultRun(classDecl, field);

      return this->runFieldDefault(classDecl, index, std::move(run));
    }
  }
}

void DeclarationClassesRegistry::continueFieldDefault(const ContinueFieldDefault &task) {
  return this->runFieldDefault(task.classDecl, task.fieldIndex, task.run);
}

DefaultRun DeclarationClassesRegistry::fieldDefaultRun(const ClassDecl *classDecl,
                                                       const ClassField &field) const {
  auto visiting = llvm::SmallPtrSet<const ClassDecl *, 4>();
  visiting.insert(classDecl);
  const TypeRef *ref = field.type.has_value() ? &*field.type : nullptr;
  auto run = DefaultRun{
      .visiting = visiting,
      .loc = field.loc,
      .steps = {DefaultFor{.type = field.resolved, .ref = ref}},
      .values = {},
      .hasNoDefault = false,
  };

  return run;
}

void DeclarationClassesRegistry::runFieldDefault(ClassDecl *classDecl, size_t index,
                                                 DefaultRun run) {
  while (!run.steps.empty() && !run.hasNoDefault) {
    auto *needed = this->unresolvedFamily(run.steps.back());
    if (needed != nullptr) {
      this->state.walk.tasks.pushTask(
          ContinueFieldDefault{.classDecl = classDecl, .fieldIndex = index, .run = run});

      return this->state.walk.tasks.pushTask(ResolveClass{.classDecl = needed});
    }
    DefaultStep step = run.steps.back();
    run.steps.pop_back();
    this->applyDefaultStep(step, run);
  }
  ClassField &field = classDecl->fields[index];
  field.init = run.hasNoDefault ? nullptr : run.values.back();
  this->state.walk.tasks.pushTask(
      NextFieldDefault{.classDecl = classDecl, .fieldIndex = index + 1});
}

ClassDecl *DeclarationClassesRegistry::unresolvedFamily(const DefaultStep &step) const {
  const ResolveFamily *family = step.as<ResolveFamily>();
  bool isUnresolved = family != nullptr && family->classDecl->stage == ClassStage::UNRESOLVED;

  return isUnresolved ? family->classDecl : nullptr;
}

bool DeclarationClassesRegistry::isNeedingDefault(const ClassField &field) const {
  bool isValued = field.isStatic() || field.init != nullptr || field.method() != nullptr;
  bool isLeftEmpty = field.optional() || field.parameter() || field.definite();

  return !isValued && !isLeftEmpty && field.resolved != nullptr;
}

void DeclarationClassesRegistry::makeFieldInit(const MakeFieldInit &task) {
  ClassDecl *classDecl = task.classDecl;
  auto sets = this->fieldSets(classDecl);
  this->state.walk.tasks.pushTask(FinishVariants{.classDecl = classDecl});
  if (sets.empty()) {
    return;
  }
  auto *init = this->makeInitDecl(classDecl, std::move(sets));
  this->state.walk.tasks.pushTask(FinishFieldInit{.classDecl = classDecl, .init = init});
  this->pushSignature(init);
}

void DeclarationClassesRegistry::finishFieldInit(const FinishFieldInit &task) {
  task.init->sym->setReturnType(Type::voidTy());
  task.classDecl->init = task.init;
}

void DeclarationClassesRegistry::buildInstanceInit(const BuildInstanceInit &task) {
  ClassDecl *instance = task.instance;
  auto sets = this->fieldSets(instance);
  instance->init = this->makeInitDecl(instance, std::move(sets));
}

void DeclarationClassesRegistry::finishVariants(const FinishVariants &task) {
  ClassDecl *classDecl = task.classDecl;
  this->numberVariants(classDecl->methods);
  this->numberVariants(classDecl->constructors);
  this->walkVariants(classDecl);
}

std::vector<Stmt *> DeclarationClassesRegistry::fieldSets(const ClassDecl *classDecl) {
  auto sets = std::vector<Stmt *>();
  for (const ClassField &field : classDecl->fields) {
    bool isSetInInit = !field.isStatic() && field.init != nullptr && field.method() == nullptr;
    if (isSetInInit) {
      Stmt *set = this->fieldSet(field);
      sets.push_back(set);
    }
  }

  return sets;
}

Stmt *DeclarationClassesRegistry::fieldSet(const ClassField &field) {
  auto &ast = this->state.ast;
  auto *self = ast.make<NameExpr>(field.loc, KeywordTable::getName(Keyword::THIS));
  auto *target = ast.make<MemberExpr>(field.loc, self, field.name);
  auto *set = ast.make<AssignExpr>(field.loc, std::nullopt, target, field.init);

  return ast.make<ExprStmt>(field.loc, set);
}

FunctionDecl *DeclarationClassesRegistry::makeInitDecl(ClassDecl *classDecl,
                                                       std::vector<Stmt *> sets) {
  auto &ast = this->state.ast;
  auto initName = InternalNameTable::getName(InternalName::FIELD_INIT);
  auto *init = ast.make<FunctionDecl>(classDecl->loc(), initName);
  init->role = ClassMember{.cls = classDecl};
  init->body = ast.make<BlockStmt>(classDecl->loc());
  init->body->body = std::move(sets);

  return init;
}

Expr *DeclarationClassesRegistry::literalValue(const Type *literal, llvm::SMLoc loc) {
  auto &ast = this->state.ast;
  const Type *base = literal->inner();
  if (base != nullptr && base->isString()) {
    return ast.make<StringLitExpr>(loc, literal->literalText());
  }
  if (base != nullptr && base->isBool()) {
    return ast.make<BoolLitExpr>(loc, literal->literalNumber() != 0);
  }
  double number = literal->literalNumber();
  double size = number < 0 ? -number : number;
  Expr *value = this->sizeLiteral(base, size, loc);
  if (number < 0) {
    return ast.make<UnaryExpr>(loc, UnaryOp::Neg, value);
  }

  return value;
}

Expr *DeclarationClassesRegistry::sizeLiteral(const Type *base, double size, llvm::SMLoc loc) {
  auto &ast = this->state.ast;
  if (base != nullptr && base->isFloat()) {
    return ast.make<FloatLitExpr>(loc, size);
  }
  auto magnitude = static_cast<uint64_t>(size);

  return ast.make<IntLitExpr>(loc, magnitude);
}

void DeclarationClassesRegistry::applyDefaultStep(const DefaultStep &step, DefaultRun &run) {
  switch (step.kind()) {
    case DefaultStepKind::VALUE:
      return this->defaultFor(*step.as<DefaultFor>(), run);
    case DefaultStepKind::ENTER_EMPTY:
      return this->enterEmptyClass(step.as<EnterEmpty>()->classDecl, run);
    case DefaultStepKind::RESOLVE_FAMILY:
      return this->resolveFamily(step.as<ResolveFamily>()->classDecl, run);
    case DefaultStepKind::LEAVE_EMPTY:
      return this->leaveEmptyClass(step.as<LeaveEmpty>()->classDecl, run);
    case DefaultStepKind::MAKE_NEW:
      return this->makeEmptyNew(step.as<MakeNew>()->classDecl, run);
    case DefaultStepKind::FILL_OBJECT:
      return this->fillObject(*step.as<FillObject>(), run);
  }
}

void DeclarationClassesRegistry::defaultFor(const DefaultFor &wanted, DefaultRun &run) {
  const Type *type = wanted.type;
  if (type == nullptr || type->isError() || type->isParam()) {
    run.hasNoDefault = true;
    return;
  }
  auto *simple = this->simpleDefault(type, wanted.ref, run.loc);
  if (simple != nullptr) {
    run.values.push_back(simple);
    return;
  }
  auto *classDecl = this->classOf(type);
  if (classDecl != nullptr) {
    return this->planClassDefault(classDecl, run);
  }

  return this->planObjectDefault(type, run);
}

void DeclarationClassesRegistry::planClassDefault(ClassDecl *classDecl, DefaultRun &run) const {
  if (classDecl->name.empty()) {
    run.hasNoDefault = true;
    return;
  }
  run.steps.push_back(MakeNew{.classDecl = classDecl});
  run.steps.push_back(EnterEmpty{.classDecl = classDecl});
}

void DeclarationClassesRegistry::planObjectDefault(const Type *type, DefaultRun &run) {
  if (!this->isPlainObject(type)) {
    run.hasNoDefault = true;
    return;
  }
  auto *object = this->state.ast.make<ObjectLitExpr>(run.loc);
  auto fields = this->requiredFields(type);
  run.steps.push_back(FillObject{.object = object, .fields = fields});
  for (const Field *field : llvm::reverse(fields)) {
    run.steps.push_back(DefaultFor{.type = field->type, .ref = nullptr});
  }
}

std::vector<const Field *> DeclarationClassesRegistry::requiredFields(const Type *type) const {
  auto required = std::vector<const Field *>();
  for (const Field &field : type->fields()) {
    bool isOptional = field.type != nullptr && field.type->isOptional();
    if (!isOptional) {
      required.push_back(&field);
    }
  }

  return required;
}

Expr *DeclarationClassesRegistry::simpleDefault(const Type *type, const TypeRef *ref,
                                                llvm::SMLoc loc) {
  auto &ast = this->state.ast;
  if (type->isString()) {
    return ast.make<StringLitExpr>(loc, u"");
  }
  if (type->isInteger() || type->isDecimal()) {
    return ast.make<IntLitExpr>(loc, uint64_t(0));
  }
  if (type->isFloat()) {
    return ast.make<FloatLitExpr>(loc, 0.0);
  }
  if (type->isBool()) {
    return ast.make<BoolLitExpr>(loc, false);
  }
  if (type->isArray() && !type->isTuple()) {
    return ast.make<ArrayLitExpr>(loc);
  }
  if (this->isWrittenCollection(type, ref)) {
    TypeName collection = type->isMap() ? TypeName::MAP : TypeName::SET;
    llvm::StringRef name = TypeNameTable::getName(collection);

    return ast.make<NewExpr>(loc, name, ref->args, std::vector<Expr *>());
  }
  if (type->isLiteral()) {
    return this->literalValue(type, loc);
  }
  if (this->isLiteralUnion(type)) {
    return this->literalValue(type->members().front(), loc);
  }

  return nullptr;
}

bool DeclarationClassesRegistry::isWrittenCollection(const Type *type, const TypeRef *ref) const {
  bool isCollection = type->isMap() || type->isSet();

  return isCollection && ref != nullptr && ref->kind() == TypeRefKind::GENERIC_REF;
}

bool DeclarationClassesRegistry::isLiteralUnion(const Type *type) const {
  if (!type->isUnion() || type->members().empty()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isLiteral()) {
      return false;
    }
  }

  return true;
}

bool DeclarationClassesRegistry::isPlainObject(const Type *type) const {
  bool isSpecial = type->isTuple() || type->isAnyObject() || type->isDate() || type->isRegExp() ||
                   type->isMatch() || type->isIntl() || type->isWeakRef() ||
                   type->isRuntimeObject() || type->isBytes();

  return type->isObject() && !isSpecial && !this->hasInternalField(type);
}

bool DeclarationClassesRegistry::hasInternalField(const Type *type) const {
  for (const Field &field : type->fields()) {
    if (llvm::StringRef(field.name).starts_with(kInternalPrefix)) {
      return true;
    }
  }

  return false;
}

void DeclarationClassesRegistry::enterEmptyClass(ClassDecl *classDecl, DefaultRun &run) const {
  if (this->isMadeByConstructor(classDecl)) {
    run.hasNoDefault = true;
    return;
  }
  bool isFirstVisit = run.visiting.insert(classDecl).second;
  if (!isFirstVisit) {
    run.hasNoDefault = true;
    return;
  }
  run.steps.push_back(LeaveEmpty{.classDecl = classDecl});
  run.steps.push_back(ResolveFamily{.classDecl = classDecl});
}

bool DeclarationClassesRegistry::isMadeByConstructor(const ClassDecl *classDecl) const {
  bool isOpenClass = classDecl->isAbstract || classDecl->builtin() || classDecl->isGeneric() ||
                     !classDecl->typeParams.empty();

  return isOpenClass || !classDecl->findConstructors().empty();
}

void DeclarationClassesRegistry::leaveEmptyClass(const ClassDecl *classDecl,
                                                 DefaultRun &run) const {
  run.visiting.erase(classDecl);
}

void DeclarationClassesRegistry::resolveFamily(ClassDecl *classDecl, DefaultRun &run) {
  if (classDecl->parent != nullptr) {
    run.steps.push_back(ResolveFamily{.classDecl = classDecl->parent});
  }
  auto inner = std::vector<ClassDecl *>();
  for (const ClassField &field : classDecl->fields) {
    ClassDecl *held = this->defaultedClass(field);
    if (held != nullptr) {
      inner.push_back(held);
    }
  }
  for (ClassDecl *held : llvm::reverse(inner)) {
    run.steps.push_back(EnterEmpty{.classDecl = held});
  }
}

ClassDecl *DeclarationClassesRegistry::defaultedClass(const ClassField &field) const {
  bool isValued = field.isStatic() || field.init != nullptr || field.method() != nullptr;
  if (isValued || field.optional()) {
    return nullptr;
  }

  return this->classOf(field.resolved);
}

ClassDecl *DeclarationClassesRegistry::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

void DeclarationClassesRegistry::makeEmptyNew(const ClassDecl *classDecl, DefaultRun &run) {
  auto *made = this->state.ast.make<NewExpr>(run.loc, classDecl->name, std::vector<TypeRef>(),
                                             std::vector<Expr *>());
  run.values.push_back(made);
}

void DeclarationClassesRegistry::fillObject(const FillObject &fill, DefaultRun &run) const {
  auto values = ChildrenFirst::takeAnswers(run.values, fill.fields.size());
  for (size_t index = 0; index < values.size(); ++index) {
    ObjectLitExpr::Property property;
    property.name = fill.fields[index]->name;
    property.loc = run.loc;
    property.value = values[index];
    fill.object->props.push_back(property);
  }
  run.values.push_back(fill.object);
}

void DeclarationClassesRegistry::walkVariantList(llvm::ArrayRef<FunctionDecl *> functions) {
  for (size_t index = 0; index < functions.size(); ++index) {
    for (FunctionDecl *earlier : functions.take_front(index)) {
      if (this->isVariantOf(earlier, functions[index])) {
        this->reportVariantPair(earlier, functions[index]);
      }
    }
  }
}

void DeclarationClassesRegistry::walkVariants(ClassDecl *classDecl) {
  this->walkVariantList(classDecl->methods);
  this->walkVariantList(classDecl->constructors);
}

std::optional<int> DeclarationClassesRegistry::ambiguousCount(const FuncSymbol *first,
                                                              const FuncSymbol *second) const {
  size_t firstFixed = first->params.size() - first->hasRest;
  size_t secondFixed = second->params.size() - second->hasRest;
  size_t last = std::min(firstFixed, secondFixed);
  for (size_t count = std::max(first->minArgs, second->minArgs); count <= last; ++count) {
    auto firstParams = llvm::ArrayRef<const Type *>(first->params).take_front(count);
    auto secondParams = llvm::ArrayRef<const Type *>(second->params).take_front(count);
    if (firstParams == secondParams) {
      return int(count);
    }
  }

  return std::nullopt;
}

bool DeclarationClassesRegistry::isFullyTyped(const FuncSymbol *function) const {
  return function != nullptr && !llvm::is_contained(function->params, nullptr);
}

bool DeclarationClassesRegistry::hasOtherReturn(const FuncSymbol *first,
                                                const FuncSymbol *second) const {
  bool isReturnKnown = first->hasReturnType() && second->hasReturnType();

  return isReturnKnown && first->getKnownReturnType() != second->getKnownReturnType();
}

DeclarationClassesRegistry::ResultVariantsUnequal
DeclarationClassesRegistry::variantPair(const FunctionDecl *first,
                                        const FunctionDecl *second) const {
  const FuncSymbol *firstFunction = first->sym;
  const FuncSymbol *secondFunction = second->sym;
  bool isTyped = this->isFullyTyped(firstFunction) && this->isFullyTyped(secondFunction);
  if (!isTyped) {
    return ok(second);
  }
  bool isSameParams = this->hasSameSignature(firstFunction, secondFunction);
  if (isSameParams && this->hasOtherReturn(firstFunction, secondFunction)) {
    auto message = ErrorTextVariantPair::returnOnly(first->isConstructor(), first->name);

    return err(DeclarationClassesRegistryVariantPairError{
        .code = DeclarationClassesRegistryVariantPairErrorCode::ONLY_RETURN_DIFFERS,
        .message = message,
    });
  }
  if (isSameParams) {
    auto variant = ErrorTextVariant::text(second);
    auto message = ErrorTextVariantPair::duplicate(variant);

    return err(DeclarationClassesRegistryVariantPairError{
        .code = DeclarationClassesRegistryVariantPairErrorCode::SAME_PARAMETERS,
        .message = message,
    });
  }
  auto count = this->ambiguousCount(firstFunction, secondFunction);
  if (count.has_value()) {
    auto firstText = ErrorTextVariant::text(first);
    auto secondText = ErrorTextVariant::text(second);
    auto message = ErrorTextVariantPair::ambiguous(firstText, secondText, *count);

    return err(DeclarationClassesRegistryVariantPairError{
        .code = DeclarationClassesRegistryVariantPairErrorCode::SAME_FOR_COUNT,
        .message = message,
    });
  }

  return ok(second);
}

void DeclarationClassesRegistry::reportVariantPair(FunctionDecl *first, FunctionDecl *second) {
  auto unequal = this->variantPair(first, second);
  if (unequal.isErr()) {
    auto other = ErrorTextVariantPair::other();
    this->state.diagnostics.error(second->loc(), unequal.error().message);
    this->state.diagnostics.note(first->loc(), other);
  }
}

DeclarationClassesRegistry::ResultNotParentMethod
DeclarationClassesRegistry::parentMethod(const ClassDecl *classDecl,
                                         const ClassField &field) const {
  if (classDecl->parent == nullptr) {
    return ok(&field);
  }
  auto inherited = classDecl->parent->findMethodsNamed(field.name);
  if (!inherited.empty()) {
    auto message = ErrorTextParentMethod::clash(field.name, classDecl->parent->name);

    return err(DeclarationClassesRegistryParentMethodError{
        .code = DeclarationClassesRegistryParentMethodErrorCode::PARENT_METHOD,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultNarrowingLayout
DeclarationClassesRegistry::narrowedField(const ClassDecl *classDecl, const ClassField &field,
                                          const Field &inherited, const ClassDecl *owner) const {
  bool isOtherLayout = inherited.type != field.resolved &&
                       !WideningRelations::isSameReference(field.resolved, inherited.type);
  if (isOtherLayout) {
    auto message = ErrorTextNarrowedField::layout(
        field.name, classDecl->name, field.resolved->name(), owner->name, inherited.type->name());

    return err(DeclarationClassesRegistryNarrowedFieldError{
        .code = DeclarationClassesRegistryNarrowedFieldErrorCode::OTHER_LAYOUT,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationClassesRegistry::completeClass(const CompleteClass &task) {
  ClassDecl *classDecl = task.classDecl;
  bool isIncomplete = classDecl->type->isIncomplete() && classDecl->literal() == nullptr;
  if (!isIncomplete || this->state.declaration.classes.isCompleting(classDecl)) {
    return;
  }
  this->state.declaration.classes.enterCompletion(classDecl);
  this->state.walk.tasks.pushTask(FinishCompletion{.classDecl = classDecl});
  ClassDecl *generic = classDecl->generic();
  if (generic != nullptr) {
    this->state.walk.tasks.pushTask(SubstituteInstanceFields{.instance = classDecl});
    this->state.walk.tasks.pushTask(CompleteClass{.classDecl = generic});
  }
  if (classDecl->parent != nullptr) {
    this->state.walk.tasks.pushTask(CompleteClass{.classDecl = classDecl->parent});
  }
}

void DeclarationClassesRegistry::finishCompletion(const FinishCompletion &task) {
  ClassDecl *classDecl = task.classDecl;
  auto *type = const_cast<Type *>(classDecl->type);
  const ClassDecl *generic = classDecl->generic();
  if (generic != nullptr) {
    this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  }
  auto fields = this->inheritedFields(classDecl);
  for (ClassField &field : classDecl->fields) {
    if (!field.isStatic() && field.method() == nullptr) {
      this->completeField(classDecl, field, fields);
    }
  }
  type->defineFields(std::move(fields));
  this->state.declaration.classes.leaveCompletion(classDecl);
  if (generic != nullptr) {
    this->state.diagnostics.popList();
  }
}

std::vector<Field> DeclarationClassesRegistry::inheritedFields(const ClassDecl *classDecl) const {
  if (classDecl->parent == nullptr) {
    return {};
  }

  return classDecl->parent->type->fields();
}

void DeclarationClassesRegistry::completeField(const ClassDecl *classDecl, ClassField &field,
                                               std::vector<Field> &fields) {
  if (field.resolved == nullptr) {
    field.resolved = Type::error();
  }
  if (field.redeclared) {
    return this->narrowInheritedField(classDecl, field, fields);
  }

  return this->addOwnField(classDecl, field, fields);
}

void DeclarationClassesRegistry::addOwnField(const ClassDecl *classDecl, const ClassField &field,
                                             std::vector<Field> &fields) {
  auto own = this->parentMethod(classDecl, field);
  if (own.isErr()) {
    this->state.diagnostics.error(field.loc, own.error().message);
  }
  auto added = Field{
      .name = field.name.str(),
      .type = field.resolved,
      .readonly = field.readonly,
  };
  fields.push_back(added);
}

void DeclarationClassesRegistry::narrowInheritedField(const ClassDecl *classDecl,
                                                      const ClassField &field,
                                                      std::vector<Field> &fields) {
  Field *inherited = this->fieldNamed(fields, field.name);
  if (inherited == nullptr || field.resolved->isError() || inherited->type->isError()) {
    return;
  }
  const auto *owner = classDecl->findInheritedFieldOwner(field.name);
  auto layout = this->narrowedField(classDecl, field, *inherited, owner);
  if (layout.isErr()) {
    this->state.diagnostics.error(field.loc, layout.error().message);
    return;
  }
  bool isNarrowed = inherited->type != field.resolved || (field.readonly && !inherited->readonly);
  if (isNarrowed) {
    this->state.inventory.wideningStore.markFieldsNarrowed();
  }
  inherited->type = field.resolved;
  inherited->readonly = field.readonly;
}

Field *DeclarationClassesRegistry::fieldNamed(std::vector<Field> &fields,
                                              llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

bool DeclarationClassesRegistry::isErrorClass(llvm::StringRef name) const {
  return ErrorClassTable::isErrorClass(name);
}

void DeclarationClassesRegistry::addBase(const Type *type, const Type *base) {
  const_cast<Type *>(type)->addBase(base);
  bool isEmitted =
      !this->state.walk.functions.isInsideTrial() &&
      llvm::none_of(this->state.walk.functions.getFunctions(),
                    std::bind_front(&DeclarationClassesRegistry::isUninstantiatedGeneric, this));
  auto &subtypes = this->state.inventory.subtypes;
  bool isNew = !llvm::is_contained(subtypes, type);
  if (isEmitted && isNew) {
    subtypes.push_back(type);
  }
}

bool DeclarationClassesRegistry::isUninstantiatedGeneric(const FunctionDecl *functionDecl) const {
  return !functionDecl->typeParams.empty() && !functionDecl->isInstance;
}

DeclarationClassesRegistry::ResultParentClassFound
DeclarationClassesRegistry::checkBaseClass(const ClassDecl *classDecl, const TypeRef &ref,
                                           bool isNamed, const ParentLookup &lookup) const {
  if (!isNamed) {
    auto message = ErrorTextExtends::byName();

    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::NOT_NAMED,
        .message = message,
    });
  }
  if (lookup.isInstance && lookup.parent == nullptr) {
    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (lookup.parent != nullptr) {
    return this->checkUsableParent(classDecl, lookup.parent);
  }

  return this->checkMissingParent(classDecl, ref, lookup.found);
}

DeclarationClassesRegistry::ResultParentClassFound
DeclarationClassesRegistry::checkUsableParent(const ClassDecl *classDecl, ClassDecl *parent) const {
  if (parent == classDecl || parent->stage == ClassStage::RESOLVING_BASE) {
    llvm::StringRef through = parent == classDecl ? llvm::StringRef() : parent->name;
    auto message = ErrorTextExtends::itself(classDecl->name, through);

    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::EXTENDS_ITSELF,
        .message = message,
    });
  }
  if (this->state.library.filesystem.isFileClass(parent)) {
    auto message = ErrorTextExtends::runtime(classDecl->name, parent->name);

    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (parent->baseExpr != nullptr && parent->stage == ClassStage::UNRESOLVED) {
    auto message = ErrorTextExtends::valueBase(classDecl->name, parent->name);

    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::VALUE_BASE,
        .message = message,
    });
  }

  return ok(parent);
}

DeclarationClassesRegistry::ResultParentClassFound
DeclarationClassesRegistry::checkMissingParent(const ClassDecl *classDecl, const TypeRef &ref,
                                               const Stmt *found) const {
  if (found == nullptr && this->isErrorClass(ref.name)) {
    auto message = ErrorTextExtends::errorClass(ref.name, classDecl->name);

    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::ERROR_CLASS,
        .message = message,
    });
  }
  if (found != nullptr) {
    bool isAlias = isa<TypeAliasDecl>(found);
    auto message = ErrorTextExtends::notClass(ref.name, classDecl->name, isAlias);

    return err(DeclarationClassesRegistryBaseClassError{
        .code = DeclarationClassesRegistryBaseClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }
  auto message = ErrorTextExtends::noClass(ref.name);

  return err(DeclarationClassesRegistryBaseClassError{
      .code = DeclarationClassesRegistryBaseClassErrorCode::NO_SUCH_CLASS,
      .message = message,
  });
}

DeclarationClassesRegistry::ResultImplementedShape
DeclarationClassesRegistry::checkImplementedType(const Type *type,
                                                 const ClassDecl *classDecl) const {
  if (type->isClass()) {
    auto message = ErrorTextExtends::classIface(type->name(), classDecl->base.has_value());

    return err(DeclarationClassesRegistryImplementedTypeError{
        .code = DeclarationClassesRegistryImplementedTypeErrorCode::CLASS_AS_INTERFACE,
        .message = message,
    });
  }
  bool isShape =
      type->isObject() && !type->isTuple() && !type->isAnyObject() && !type->isIncomplete();
  if (!isShape) {
    auto message = ErrorTextExtends::notIface(type->name());

    return err(DeclarationClassesRegistryImplementedTypeError{
        .code = DeclarationClassesRegistryImplementedTypeErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(type);
}

DeclarationClassesRegistry::ResultOverrideTargeted
DeclarationClassesRegistry::checkOverrideTarget(const ClassDecl *classDecl,
                                                const ClassField &field) const {
  if (field.isOverride && classDecl->parent != nullptr) {
    auto message = ErrorTextRedeclared::noField(field.name, classDecl->parent->name);

    return err(DeclarationClassesRegistryOverrideTargetError{
        .code = DeclarationClassesRegistryOverrideTargetErrorCode::NO_FIELD,
        .message = message,
    });
  }
  if (field.isOverride) {
    auto message = ErrorTextRedeclared::noParent(field.name, classDecl->name);

    return err(DeclarationClassesRegistryOverrideTargetError{
        .code = DeclarationClassesRegistryOverrideTargetErrorCode::NO_PARENT,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultInheritedVisible
DeclarationClassesRegistry::checkInheritedVisible(const ClassField &field,
                                                  const ClassField *inherited,
                                                  const ClassDecl *owner) const {
  if (inherited->access == Access::Private) {
    auto message = ErrorTextRedeclared::isPrivate(field.name, owner->name);

    return err(DeclarationClassesRegistryInheritedVisibleError{
        .code = DeclarationClassesRegistryInheritedVisibleErrorCode::PRIVATE_IN_BASE,
        .message = message,
    });
  }

  return ok(inherited);
}

DeclarationClassesRegistry::ResultAccessIsNotLower
DeclarationClassesRegistry::checkAccessKept(const ClassField &field, const ClassField *inherited,
                                            const ClassDecl *owner,
                                            const ClassDecl *classDecl) const {
  if (field.access > inherited->access) {
    auto baseAccess = AccessKeywordTable::getName(inherited->access);
    auto access = AccessKeywordTable::getName(field.access);
    auto message =
        ErrorTextRedeclared::narrower(field.name, baseAccess, owner->name, access, classDecl->name);

    return err(DeclarationClassesRegistryAccessKeptError{
        .code = DeclarationClassesRegistryAccessKeptErrorCode::NARROWER_ACCESS,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultValueOnRedeclare
DeclarationClassesRegistry::checkValueGiven(const ClassDecl *classDecl, const ClassField &field,
                                            const ClassDecl *owner) const {
  if (field.init == nullptr && !field.parameter() && classDecl->constructors.empty()) {
    auto message = ErrorTextRedeclared::noValue(field.name, owner->name);

    return err(DeclarationClassesRegistryValueGivenError{
        .code = DeclarationClassesRegistryValueGivenErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationClassesRegistry::startClassBases(const StartClassBases &task) {
  ClassDecl *classDecl = task.classDecl;
  classDecl->stage = ClassStage::RESOLVING_BASE;
  this->state.walk.tasks.pushTask(FinishClassBases{.classDecl = classDecl});
  for (TypeRef &ref : llvm::reverse(classDecl->interfaces)) {
    this->state.walk.tasks.pushTask(FinishImplemented{.classDecl = classDecl, .ref = &ref});
    this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &ref, .isVoidAllowed = false});
  }
  if (classDecl->base.has_value()) {
    this->startParentClass(classDecl);
  }
}

void DeclarationClassesRegistry::finishClassBases(const FinishClassBases &task) {
  ClassDecl *classDecl = task.classDecl;
  classDecl->stage = ClassStage::RESOLVED;
  for (ClassField &field : classDecl->fields) {
    this->walkRedeclaredField(classDecl, field);
  }
}

void DeclarationClassesRegistry::startParentClass(ClassDecl *classDecl) {
  TypeRef &ref = *classDecl->base;
  bool isNamed = this->isNamedRef(ref);
  Stmt *found = isNamed ? this->state.findTypeDeclaration(ref.name) : nullptr;
  auto *parent = llvm::dyn_cast_or_null<ClassDecl>(found);
  if (this->isEmitterParent(parent, ref)) {
    this->state.walk.tasks.pushTask(FinishParentClass{.classDecl = classDecl, .found = found});

    return this->state.walk.tasks.pushTask(
        InstantiateEmitterParent{.classDecl = classDecl, .emitter = parent, .ref = &ref});
  }
  if (this->isGenericParent(parent, ref)) {
    this->state.walk.tasks.pushTask(FinishParentClass{.classDecl = classDecl, .found = found});

    return this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &ref, .isVoidAllowed = false});
  }
  auto lookup = ParentLookup{
      .parent = parent,
      .found = found,
      .isInstance = false,
  };

  return this->linkFoundParent(classDecl, lookup);
}

bool DeclarationClassesRegistry::isNamedRef(const TypeRef &ref) const {
  return ref.kind() == TypeRefKind::NAME_REF || ref.kind() == TypeRefKind::GENERIC_REF;
}

bool DeclarationClassesRegistry::isEmitterParent(const ClassDecl *parent,
                                                 const TypeRef &ref) const {
  return parent != nullptr && parent->library() &&
         LibraryClassTable::isName(parent->name, LibraryClass::EVENT_EMITTER) &&
         ref.kind() == TypeRefKind::NAME_REF;
}

bool DeclarationClassesRegistry::isGenericParent(const ClassDecl *parent,
                                                 const TypeRef &ref) const {
  return parent != nullptr && (parent->isGeneric() || ref.kind() == TypeRefKind::GENERIC_REF);
}

void DeclarationClassesRegistry::finishParentClass(const FinishParentClass &task) {
  ClassDecl *classDecl = task.classDecl;
  auto *instance = this->classOf(classDecl->base->resolved);
  auto lookup = ParentLookup{
      .parent = instance,
      .found = instance != nullptr ? task.found : nullptr,
      .isInstance = true,
  };

  return this->linkFoundParent(classDecl, lookup);
}

void DeclarationClassesRegistry::linkFoundParent(ClassDecl *classDecl, const ParentLookup &lookup) {
  const TypeRef &ref = *classDecl->base;
  bool isNamed = this->isNamedRef(ref);
  auto base = this->checkBaseClass(classDecl, ref, isNamed, lookup);
  if (base.isErr() &&
      base.error().code != DeclarationClassesRegistryBaseClassErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(ref.loc, base.error().message);
  }
  if (base.isErr()) {
    return;
  }

  return this->linkParent(classDecl, lookup.parent);
}

void DeclarationClassesRegistry::finishLinkParent(const FinishLinkParent &task) {
  ClassDecl *classDecl = task.classDecl;
  ClassDecl *parent = task.parent;
  this->state.walk.scopes.leaveOnlyTypes();
  classDecl->parent = parent;
  parent->children.push_back(classDecl);
  this->addBase(classDecl->type, parent->type);
}

void DeclarationClassesRegistry::finishImplemented(const FinishImplemented &task) {
  ClassDecl *classDecl = task.classDecl;
  const TypeRef &ref = *task.ref;
  const auto *type = ref.resolved;
  if (type->isError()) {
    return;
  }
  auto shape = this->checkImplementedType(type, classDecl);
  if (shape.isErr()) {
    this->state.diagnostics.error(ref.loc, shape.error().message);
    return;
  }
  if (llvm::is_contained(classDecl->implemented, type)) {
    return;
  }
  classDecl->implemented.push_back(type);
  this->addBase(classDecl->type, type);
}

void DeclarationClassesRegistry::linkParent(ClassDecl *classDecl, ClassDecl *parent) {
  auto topTypes = this->state.walk.scopes.getTopTypes();
  this->state.walk.scopes.enterOnlyTypes(topTypes);
  auto *home = this->state.declaration.modules.findDeclModule(parent);
  auto *here = this->state.declaration.modules.getCurrentModule();
  bool isForeign = home != nullptr && home != here && this->state.walk.scopes.isTopLevel();
  this->state.walk.tasks.pushTask(FinishLinkParent{.classDecl = classDecl, .parent = parent});
  if (!isForeign) {
    return this->state.walk.tasks.pushTask(ResolveClass{.classDecl = parent});
  }
  this->state.walk.tasks.pushTask(UseModule{.module = here});
  this->state.walk.tasks.pushTask(ResolveClass{.classDecl = parent});
  this->state.walk.tasks.pushTask(UseModule{.module = home});
}

void DeclarationClassesRegistry::walkRedeclaredField(ClassDecl *classDecl, ClassField &field) {
  if (field.isStatic()) {
    return;
  }
  const auto *inherited = classDecl->findInheritedField(field.name);
  const auto *owner = classDecl->findInheritedFieldOwner(field.name);
  if (inherited != nullptr && inherited->method() != nullptr) {
    return;
  }
  if (inherited == nullptr) {
    auto target = this->checkOverrideTarget(classDecl, field);
    if (target.isErr()) {
      this->state.diagnostics.error(field.loc, target.error().message);
    }
    return;
  }
  auto visible = this->checkInheritedVisible(field, inherited, owner);
  if (visible.isErr()) {
    this->state.diagnostics.error(field.loc, visible.error().message);
    return;
  }
  auto access = this->checkAccessKept(field, inherited, owner, classDecl);
  if (access.isErr()) {
    this->state.diagnostics.error(field.loc, access.error().message);
  }
  auto valued = this->checkValueGiven(classDecl, field, owner);
  if (valued.isErr()) {
    this->state.diagnostics.error(field.loc, valued.error().message);
  }
  field.redeclared = true;
}

DeclarationClassesRegistry::ResultInterfaceNotSelf
DeclarationClassesRegistry::checkNotSelf(const TypeAliasDecl *alias,
                                         const TypeAliasDecl *other) const {
  if (other != nullptr && (other == alias || other->resolving)) {
    llvm::StringRef through = other == alias ? llvm::StringRef() : other->name;
    auto message = ErrorTextInterfaceBases::itself(alias->name, through);

    return err(DeclarationClassesRegistryNotSelfError{
        .code = DeclarationClassesRegistryNotSelfErrorCode::EXTENDS_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

DeclarationClassesRegistry::ResultObjectTypeAsBase
DeclarationClassesRegistry::checkInterfaceBase(const Type *base) const {
  if (base->isClass()) {
    auto message = ErrorTextInterfaceBases::classBase(base->name());

    return err(DeclarationClassesRegistryInterfaceBaseError{
        .code = DeclarationClassesRegistryInterfaceBaseErrorCode::CLASS_BASE,
        .message = message,
    });
  }
  bool isShape =
      base->isObject() && !base->isTuple() && !base->isIncomplete() && !base->isAnyObject();
  if (!isShape) {
    auto message = ErrorTextInterfaceBases::notIface(base->name());

    return err(DeclarationClassesRegistryInterfaceBaseError{
        .code = DeclarationClassesRegistryInterfaceBaseErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(base);
}

DeclarationClassesRegistry::ResultMethodSignatures
DeclarationClassesRegistry::checkSameSignatures(const TypeAliasDecl *alias, const Overload &known,
                                                const Overload &other) const {
  if (known.signatures != other.signatures) {
    auto message = ErrorTextInterfaceBases::twoMethods(alias->name, other.name);

    return err(DeclarationClassesRegistrySameSignaturesError{
        .code = DeclarationClassesRegistrySameSignaturesErrorCode::OVERLOAD_CONFLICT,
        .message = message,
    });
  }

  return ok(&known);
}

DeclarationClassesRegistry::ResultFieldTypesAreOne
DeclarationClassesRegistry::checkSameFieldType(const TypeAliasDecl *alias, const Field &known,
                                               const Field &field, llvm::StringRef origin,
                                               llvm::StringRef baseName) const {
  if (known.type != field.type) {
    auto message = ErrorTextInterfaceBases::twoFields(
        alias->name, field.name, origin, known.type->name(), baseName, field.type->name());

    return err(DeclarationClassesRegistrySameFieldTypeError{
        .code = DeclarationClassesRegistrySameFieldTypeErrorCode::FIELD_CONFLICT,
        .message = message,
    });
  }

  return ok(&known);
}

DeclarationClassesRegistry::ResultUnusedMethodName
DeclarationClassesRegistry::checkNewOverload(const TypeAliasDecl *alias, const Overload &overload,
                                             llvm::ArrayRef<Field> fields,
                                             llvm::ArrayRef<Overload> overloads) const {
  bool isClash = this->fieldIndex(fields, overload.name) != fields.size() ||
                 this->overloadNamed(overloads, overload.name) != nullptr;
  if (isClash) {
    auto message = ErrorTextInterfaceBases::method(alias->name, overload.name);

    return err(DeclarationClassesRegistryNewOverloadError{
        .code = DeclarationClassesRegistryNewOverloadErrorCode::REDECLARED_METHOD,
        .message = message,
    });
  }

  return ok(&overload);
}

DeclarationClassesRegistry::ResultUnusedMemberName
DeclarationClassesRegistry::checkNewField(const TypeAliasDecl *alias, const Field &field,
                                          llvm::ArrayRef<Overload> overloads) const {
  if (this->overloadNamed(overloads, field.name) != nullptr) {
    auto message = ErrorTextInterfaceBases::member(alias->name, field.name);

    return err(DeclarationClassesRegistryNewFieldError{
        .code = DeclarationClassesRegistryNewFieldErrorCode::REDECLARED_MEMBER,
        .message = message,
    });
  }

  return ok(&field);
}

DeclarationClassesRegistry::ResultFieldNarrowsDown
DeclarationClassesRegistry::checkNarrowsField(const TypeAliasDecl *alias, const Field &field,
                                              const Field &known, llvm::StringRef origin) const {
  bool isWidened = known.type != field.type && !field.type->isError() && !known.type->isError() &&
                   !WideningRelations::isSameReference(field.type, known.type);
  if (isWidened) {
    auto message = ErrorTextInterfaceBases::narrowed(field.name, alias->name, field.type->name(),
                                                     origin, known.type->name());

    return err(DeclarationClassesRegistryNarrowsFieldError{
        .code = DeclarationClassesRegistryNarrowsFieldErrorCode::WIDENED_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

void DeclarationClassesRegistry::startInterfaceFields(const StartInterfaceFields &task) {
  auto draft = InterfaceDraft{
      .alias = task.alias,
      .fields = {},
      .origins = {},
      .overloads = {},
      .bases = {},
  };

  return this->pushInterfaceBase(draft, 0);
}

void DeclarationClassesRegistry::pushInterfaceBase(const InterfaceDraft &draft, size_t index) {
  TypeAliasDecl *alias = draft.alias;
  if (index == alias->bases.size()) {
    this->state.walk.tasks.pushTask(FinishInterfaceFields{.draft = draft});

    return this->state.walk.tasks.pushTask(ResolveObjectFields{.ref = &alias->type});
  }
  this->state.walk.tasks.pushTask(StartInterfaceBase{.draft = draft, .index = index});
}

void DeclarationClassesRegistry::startInterfaceBase(const StartInterfaceBase &task) {
  TypeAliasDecl *alias = task.draft.alias;
  TypeRef &ref = alias->bases[task.index];
  Stmt *found =
      ref.kind() == TypeRefKind::NAME_REF ? this->state.findTypeDeclaration(ref.name) : nullptr;
  auto *other = llvm::dyn_cast_or_null<TypeAliasDecl>(found);
  auto self = this->checkNotSelf(alias, other);
  if (self.isErr()) {
    this->state.diagnostics.error(ref.loc, self.error().message);

    return this->pushInterfaceBase(task.draft, task.index + 1);
  }
  this->state.walk.tasks.pushTask(FinishInterfaceBase{.draft = task.draft, .index = task.index});
  this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &ref, .isVoidAllowed = false});
}

void DeclarationClassesRegistry::finishInterfaceBase(const FinishInterfaceBase &task) {
  InterfaceDraft draft = task.draft;
  const TypeRef &ref = draft.alias->bases[task.index];
  const auto *base = this->acceptedInterfaceBase(ref, draft.bases);
  if (base != nullptr) {
    draft.bases.push_back(base);
    this->mergeBaseOverloads(ref, base, draft);
    this->mergeBaseFields(ref, base, draft);
  }

  return this->pushInterfaceBase(draft, task.index + 1);
}

const Type *DeclarationClassesRegistry::acceptedInterfaceBase(const TypeRef &ref,
                                                              llvm::ArrayRef<const Type *> bases) {
  const auto *base = ref.resolved;
  if (base->isError()) {
    return nullptr;
  }
  auto shape = this->checkInterfaceBase(base);
  if (shape.isErr()) {
    this->state.diagnostics.error(ref.loc, shape.error().message);
    return nullptr;
  }
  if (llvm::is_contained(bases, base)) {
    return nullptr;
  }

  return base;
}

void DeclarationClassesRegistry::finishInterfaceFields(const FinishInterfaceFields &task) {
  InterfaceDraft draft = task.draft;
  TypeAliasDecl *alias = draft.alias;
  ObjectTypeRef *shape = alias->type.form.as<ObjectTypeRef>();
  for (const Overload &overload : shape->resolvedOverloads) {
    auto unused = this->checkNewOverload(alias, overload, draft.fields, draft.overloads);
    if (unused.isErr()) {
      this->state.diagnostics.error(alias->loc(), unused.error().message);
    } else {
      draft.overloads.push_back(overload);
    }
  }
  for (const Field &field : shape->resolvedFields) {
    this->addOwnField(field, draft);
  }
  shape->resolvedFields = std::move(draft.fields);
  shape->resolvedOverloads = std::move(draft.overloads);
  shape->resolvedBases = std::move(draft.bases);
}

void DeclarationClassesRegistry::mergeBaseOverloads(const TypeRef &ref, const Type *base,
                                                    InterfaceDraft &draft) {
  for (const Overload &overload : base->overloads()) {
    const auto *known = this->overloadNamed(draft.overloads, overload.name);
    if (known == nullptr) {
      draft.overloads.push_back(overload);
    } else {
      this->walkSameSignatures(draft.alias, ref, *known, overload);
    }
  }
}

void DeclarationClassesRegistry::walkSameSignatures(const TypeAliasDecl *alias, const TypeRef &ref,
                                                    const Overload &known,
                                                    const Overload &overload) {
  auto signatures = this->checkSameSignatures(alias, known, overload);
  if (signatures.isErr()) {
    this->state.diagnostics.error(ref.loc, signatures.error().message);
  }
}

const Overload *DeclarationClassesRegistry::overloadNamed(llvm::ArrayRef<Overload> overloads,
                                                          llvm::StringRef name) const {
  for (const Overload &overload : overloads) {
    if (overload.name == name) {
      return &overload;
    }
  }

  return nullptr;
}

size_t DeclarationClassesRegistry::fieldIndex(llvm::ArrayRef<Field> fields,
                                              llvm::StringRef name) const {
  for (size_t index = 0; index < fields.size(); ++index) {
    if (fields[index].name == name) {
      return index;
    }
  }

  return fields.size();
}

void DeclarationClassesRegistry::mergeBaseFields(const TypeRef &ref, const Type *base,
                                                 InterfaceDraft &draft) {
  std::string baseName = ref.name.str();
  for (const Field &field : base->fields()) {
    this->mergeBaseField(ref, field, baseName, draft);
  }
}

void DeclarationClassesRegistry::mergeBaseField(const TypeRef &ref, const Field &field,
                                                const std::string &baseName,
                                                InterfaceDraft &draft) {
  size_t at = this->fieldIndex(draft.fields, field.name);
  if (at == draft.fields.size()) {
    draft.fields.push_back(field);
    draft.origins.push_back(baseName);
    return;
  }
  Field &known = draft.fields[at];
  auto one = this->checkSameFieldType(draft.alias, known, field, draft.origins[at], baseName);
  if (one.isErr()) {
    this->state.diagnostics.error(ref.loc, one.error().message);
    return;
  }
  known.readonly = known.readonly && field.readonly;
}

void DeclarationClassesRegistry::addOwnField(const Field &field, InterfaceDraft &draft) {
  const TypeAliasDecl *alias = draft.alias;
  auto unused = this->checkNewField(alias, field, draft.overloads);
  if (unused.isErr()) {
    this->state.diagnostics.error(alias->loc(), unused.error().message);
    return;
  }
  auto loc = this->ownFieldLoc(alias, field.name);
  size_t at = this->fieldIndex(draft.fields, field.name);
  if (at == draft.fields.size()) {
    draft.fields.push_back(field);
    draft.origins.push_back(alias->name.str());
    return;
  }
  Field &known = draft.fields[at];
  auto narrows = this->checkNarrowsField(alias, field, known, draft.origins[at]);
  if (narrows.isErr()) {
    this->state.diagnostics.error(loc, narrows.error().message);
    return;
  }
  if (known.type != field.type || (field.readonly && !known.readonly)) {
    this->state.inventory.wideningStore.markFieldsNarrowed();
  }
  known.type = field.type;
  known.readonly = field.readonly;
}

llvm::SMLoc DeclarationClassesRegistry::ownFieldLoc(const TypeAliasDecl *alias,
                                                    llvm::StringRef name) const {
  const TypeRef &own = alias->type;
  for (size_t index = own.names().size(); index > 0; --index) {
    if (own.names()[index - 1] == name) {
      return own.locs()[index - 1];
    }
  }

  return alias->loc();
}

void DeclarationClassesRegistry::declareItemMethods(Stmt *item, Module *module) {
  auto *classDecl = dyn_cast<ClassDecl>(item);
  bool isDeclared =
      classDecl != nullptr && classDecl->type != nullptr && classDecl->baseExpr == nullptr;
  if (!isDeclared) {
    return;
  }
  this->state.walk.tasks.pushTask(FinishItemMethods{.classDecl = classDecl, .module = module});
  this->state.walk.tasks.pushTask(DeclareMethods{.classDecl = classDecl});
}

void DeclarationClassesRegistry::finishItemMethods(const FinishItemMethods &task) {
  for (FunctionDecl *method : task.classDecl->collectMemberFunctions()) {
    this->state.declaration.modules.rememberDeclModule(method, task.module);
    this->state.inference.cycles.addDeclaredFunction(method);
  }
}

}
