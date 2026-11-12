#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

namespace tz {

namespace {

constexpr llvm::StringLiteral kThisName = "this";
constexpr llvm::StringLiteral kInitName = "init";
constexpr llvm::StringLiteral kInternalPrefix = "%";

}

ClassDeclarer::ClassDeclarer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->classBases = &all.classBases;
  this->classLinks = &all.classLinks;
  this->typeResolver = &all.typeResolver;
  this->functionChecker = &all.functionChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

ClassDecl *ClassDeclarer::classOf(const Type *type) const {
  return type != nullptr ? const_cast<ClassDecl *>(type->classDecl()) : nullptr;
}

bool ClassDeclarer::hasMethodFields(const ClassDecl *classDecl) const {
  if (classDecl->literal() != nullptr) {
    return true;
  }
  for (const ClassDecl *owner = classDecl; owner != nullptr; owner = owner->parent) {
    if (llvm::any_of(owner->fields, std::bind_front(&ClassDeclarer::isMethodMember, this))) {
      return true;
    }
  }

  return false;
}

bool ClassDeclarer::isMethodMember(const ClassField &field) const {
  return field.method() != nullptr;
}

std::vector<FunctionDecl *> ClassDeclarer::methodsNamed(const ClassDecl *classDecl,
                                                        llvm::StringRef name) const {
  auto found = std::vector<FunctionDecl *>();
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    for (FunctionDecl *method : this->ownMethodsNamed(current, name)) {
      if (!this->isOverriddenByAny(found, method)) {
        found.push_back(method);
      }
    }
  }

  return found;
}

std::vector<FunctionDecl *> ClassDeclarer::staticMethodsNamed(const ClassDecl *classDecl,
                                                              llvm::StringRef name) const {
  auto found = std::vector<FunctionDecl *>();
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    for (FunctionDecl *method : this->ownStaticMethodsNamed(current, name)) {
      if (!this->isHiddenByAny(found, method)) {
        found.push_back(method);
      }
    }
  }

  return found;
}

std::vector<FunctionDecl *> ClassDeclarer::memberFunctions(const ClassDecl *classDecl) const {
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

std::vector<FunctionDecl *> ClassDeclarer::ownMethodsNamed(const ClassDecl *classDecl,
                                                           llvm::StringRef name) const {
  auto own = std::vector<FunctionDecl *>();
  for (FunctionDecl *method : classDecl->methods) {
    if (method->name == name && !method->isStatic()) {
      own.push_back(method);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr && field.name == name) {
      own.push_back(field.method());
    }
  }

  return own;
}

std::vector<FunctionDecl *> ClassDeclarer::ownStaticMethodsNamed(const ClassDecl *classDecl,
                                                                 llvm::StringRef name) const {
  auto own = std::vector<FunctionDecl *>();
  for (FunctionDecl *method : classDecl->staticOwner()->methods) {
    if (method->name == name && method->isStatic()) {
      own.push_back(method);
    }
  }

  return own;
}

bool ClassDeclarer::isOverriddenByAny(llvm::ArrayRef<FunctionDecl *> found,
                                      const FunctionDecl *method) const {
  for (const FunctionDecl *lower : found) {
    if (this->classLinks->isOverridingTransitively(lower, method)) {
      return true;
    }
  }

  return false;
}

bool ClassDeclarer::isHiddenByAny(llvm::ArrayRef<FunctionDecl *> found,
                                  const FunctionDecl *method) const {
  for (const FunctionDecl *lower : found) {
    if (this->hasSameSignature(lower->sym, method->sym)) {
      return true;
    }
  }

  return false;
}

bool ClassDeclarer::hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const {
  if (first == nullptr || second == nullptr) {
    return false;
  }

  return first->params == second->params && first->minArgs == second->minArgs &&
         first->hasRest == second->hasRest;
}

void ClassDeclarer::declareClass(ClassDecl *classDecl) {
  classDecl->id = this->state.getClassStore().takeClassId();
  classDecl->type = Type::classType(classDecl->name, classDecl);
  this->state.getClassStore().addClass(classDecl);
  auto added = this->scopes->declare(classDecl->name, {ClassEntry{.cls = classDecl}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(classDecl->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->scopes->lookup(classDecl->name);
    auto declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
}

const ClassField *ClassDeclarer::earlierField(const ClassDecl *classDecl, size_t index) const {
  const ClassField &field = classDecl->fields[index];
  auto earlier = llvm::ArrayRef<ClassField>(classDecl->fields).take_front(index);
  for (const ClassField &other : earlier) {
    if (other.name == field.name && other.isStatic() == field.isStatic()) {
      return &other;
    }
  }

  return nullptr;
}

ClassDeclarer::ResultFieldNameUnique ClassDeclarer::uniqueField(const ClassDecl *classDecl,
                                                                const ClassField &field,
                                                                const ClassField *earlier) const {
  if (earlier != nullptr) {
    auto message = ErrorTextUniqueField::duplicate(field.name, classDecl->name);

    return err(ClassDeclarerUniqueFieldError{
        .code = ClassDeclarerUniqueFieldErrorCode::DUPLICATE_FIELD,
        .message = message,
    });
  }

  return ok(&field);
}

ClassDeclarer::ResultFieldNameIsFree
ClassDeclarer::fieldOrMethod(const ClassDecl *classDecl, const ClassField &field,
                             const FunctionDecl *method) const {
  if (method->name == field.name && method->isStatic() == field.isStatic()) {
    auto message = ErrorTextFieldOrMethod::shared(field.name, classDecl->name);

    return err(ClassDeclarerFieldOrMethodError{
        .code = ClassDeclarerFieldOrMethodErrorCode::FIELD_AND_METHOD,
        .message = message,
    });
  }

  return ok(&field);
}

void ClassDeclarer::walkMemberName(const ClassDecl *classDecl, size_t index) {
  const ClassField &field = classDecl->fields[index];
  const auto *earlier = this->earlierField(classDecl, index);
  auto unique = this->uniqueField(classDecl, field, earlier);
  if (unique.isErr()) {
    auto previous = ErrorTextUniqueField::previous();
    this->state.getDiagnostics().error(field.loc, unique.error().message);
    this->state.getDiagnostics().note(earlier->loc, previous);
  }
  for (FunctionDecl *method : classDecl->methods) {
    auto distinct = this->fieldOrMethod(classDecl, field, method);
    if (distinct.isErr()) {
      auto fieldNote = ErrorTextFieldOrMethod::field();
      this->state.getDiagnostics().error(method->loc(), distinct.error().message);
      this->state.getDiagnostics().note(field.loc, fieldNote);
    }
  }
}

ClassDeclarer::ResultFieldNotLateSet ClassDeclarer::setLater(const ClassField &field) const {
  if (field.definite()) {
    auto message = ErrorTextSetLater::definite(field.name);

    return err(ClassDeclarerSetLaterError{
        .code = ClassDeclarerSetLaterErrorCode::SET_LATER,
        .message = message,
    });
  }

  return ok(&field);
}

ClassDeclarer::ResultFieldTypeIsSeen ClassDeclarer::typeSeen(const ClassField &field) const {
  bool isUntypedValue = !field.type.has_value() && field.init != nullptr &&
                        SharedHelpers::literalType(field.init) == nullptr;
  if (isUntypedValue && field.isStatic()) {
    auto message = ErrorTextTypeSeen::shared(field.name);

    return err(ClassDeclarerTypeSeenError{
        .code = ClassDeclarerTypeSeenErrorCode::STATIC_VALUE_NOT_LITERAL,
        .message = message,
    });
  }
  bool isFunction = llvm::isa_and_nonnull<FunctionExpr>(field.init);
  if (isUntypedValue && isFunction && field.method() == nullptr) {
    auto message = ErrorTextTypeSeen::function(field.name);

    return err(ClassDeclarerTypeSeenError{
        .code = ClassDeclarerTypeSeenErrorCode::FUNCTION_FIELD_UNTYPED,
        .message = message,
    });
  }

  return ok(&field);
}

ClassDeclarer::ResultFieldHasNoValue ClassDeclarer::fieldValue(const ClassDecl *classDecl,
                                                               const ClassField &field) const {
  bool isFunction = llvm::isa_and_nonnull<FunctionExpr>(field.init);
  bool isValued = !field.isStatic() && field.init != nullptr && !isFunction;
  if (!isValued) {
    return ok(&field);
  }
  if (!classDecl->constructors.empty()) {
    auto message = ErrorTextDeclaredValue::constructor(field.name);

    return err(ClassDeclarerFieldValueError{
        .code = ClassDeclarerFieldValueErrorCode::VALUE_OUTSIDE_CONSTRUCTOR,
        .message = message,
    });
  }
  bool isLiteral = SharedHelpers::literalType(field.init) != nullptr;
  if (!field.type.has_value() || !isLiteral) {
    auto message = ErrorTextDeclaredValue::literal(field.name);

    return err(ClassDeclarerFieldValueError{
        .code = ClassDeclarerFieldValueErrorCode::START_VALUE_NOT_LITERAL,
        .message = message,
    });
  }

  return ok(&field);
}

ClassDeclarer::ResultFieldTypeSource ClassDeclarer::typedField(const ClassDecl *classDecl,
                                                               const ClassField &field) const {
  const Param *property = field.parameter() ? this->propertyParam(classDecl, field.name) : nullptr;
  if (property != nullptr && !property->type.has_value()) {
    auto message = ErrorTextTypedField::property(field.name);

    return err(ClassDeclarerTypedFieldError{
        .code = ClassDeclarerTypedFieldErrorCode::PROPERTY_NO_TYPE,
        .message = message,
    });
  }
  if (!field.parameter() && !field.type.has_value() && field.init == nullptr) {
    auto message = ErrorTextTypedField::missing(field.name);

    return err(ClassDeclarerTypedFieldError{
        .code = ClassDeclarerTypedFieldErrorCode::NO_TYPE,
        .message = message,
    });
  }

  return ok(&field);
}

const Param *ClassDeclarer::propertyParam(const ClassDecl *classDecl, llvm::StringRef name) const {
  for (FunctionDecl *constructor : classDecl->constructors) {
    for (const Param &param : constructor->params) {
      if (param.property && param.name == name) {
        return &param;
      }
    }
  }

  return nullptr;
}

const Type *ClassDeclarer::resolvedFieldType(const ClassDecl *classDecl, ClassField &field) {
  const Type *written = this->writtenFieldType(classDecl, field);
  if (written != nullptr && field.optional()) {
    return Type::optional(written);
  }

  return written;
}

const Type *ClassDeclarer::writtenFieldType(const ClassDecl *classDecl, ClassField &field) {
  auto source = this->typedField(classDecl, field);
  if (source.isErr()) {
    this->state.getDiagnostics().error(field.loc, source.error().message);

    return Type::error();
  }

  return this->declaredFieldType(classDecl, field);
}

const Type *ClassDeclarer::declaredFieldType(const ClassDecl *classDecl, ClassField &field) {
  if (field.parameter()) {
    const auto *param = this->propertyParam(classDecl, field.name);
    if (param->optional()) {
      field.presence = FieldPresence::OPTIONAL;
    }

    return this->typeResolver->resolveType(*param->type);
  }
  if (field.type.has_value()) {
    return this->typeResolver->resolveType(*field.type);
  }
  const auto *literal = SharedHelpers::literalType(field.init);
  bool isFromValueLater = field.isStatic() || llvm::isa<FunctionExpr>(field.init);
  if (literal != nullptr || isFromValueLater) {
    return literal;
  }

  return Type::error();
}

void ClassDeclarer::arrowMethod(ClassDecl *classDecl, ClassField &field, const Type *type) {
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

bool ClassDeclarer::isMethodField(const ClassField &field) const {
  bool isPlainField =
      !field.isStatic() && !field.parameter() && !field.optional() && !field.redeclared;

  return isPlainField && !this->state.getFlowState().isFieldAssigned(field.name);
}

ClassDeclarer::ResultStaticTypeFixed ClassDeclarer::staticTypeParams(const ClassDecl *classDecl,
                                                                     const ClassField &field,
                                                                     const Type *type) const {
  if (field.isStatic() && type != nullptr && !classDecl->typeArgs.empty() &&
      TypeQueries::mentionsParam(type)) {
    auto message = ErrorTextStaticParams::shared(field.name, classDecl->name);

    return err(ClassDeclarerStaticTypeParamsError{
        .code = ClassDeclarerStaticTypeParamsErrorCode::USES_TYPE_PARAMS,
        .message = message,
    });
  }

  return ok(type);
}

ClassDeclarer::ResultStaticGetsValue ClassDeclarer::staticValue(const ClassField &field,
                                                                const Type *type) const {
  if (field.init == nullptr && !field.optional() && type != nullptr && !type->isError()) {
    auto message = ErrorTextStaticValue::missing(field.name, type->name());

    return err(ClassDeclarerStaticValueError{
        .code = ClassDeclarerStaticValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&field);
}

void ClassDeclarer::declareStaticVar(const ClassDecl *classDecl, const ClassField &field,
                                     StaticField *shared, const Type *type) {
  auto *variable = this->state.getAst().newVar();
  std::string name = (classDecl->name + "." + field.name).str();
  variable->name = this->state.getAst().keepName(name);
  variable->loc = field.loc;
  variable->type = type;
  variable->isConst = field.readonly;
  variable->place = GlobalVar{.usedInFunction = true};
  shared->var = variable;
  auto *literal = field.readonly ? llvm::dyn_cast_or_null<IntLitExpr>(field.init) : nullptr;
  bool isFittingInt =
      literal != nullptr && literal->value <= uint64_t(std::numeric_limits<int64_t>::max());
  if (isFittingInt) {
    this->state.getConstantStore().rememberConstVar(variable, int64_t(literal->value));
  }
  auto valued = this->staticValue(field, type);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(field.loc, valued.error().message);
  }
}

void ClassDeclarer::resolveField(ClassDecl *classDecl, ClassField &field) {
  auto early = this->setLater(field);
  if (early.isErr()) {
    this->state.getDiagnostics().error(field.loc, early.error().message);
  }
  auto valueless = this->fieldValue(classDecl, field);
  if (valueless.isErr()) {
    this->state.getDiagnostics().error(field.loc, valueless.error().message);
  }
  const Type *type = this->resolvedFieldType(classDecl, field);
  field.resolved = type;
  this->arrowMethod(classDecl, field, type);
  auto seen = this->typeSeen(field);
  if (seen.isErr()) {
    this->state.getDiagnostics().error(field.loc, seen.error().message);
    type = Type::error();
    field.resolved = type;
  }
  auto fixed = this->staticTypeParams(classDecl, field, type);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(field.loc, fixed.error().message);
    type = Type::error();
    field.resolved = type;
  }
  StaticField *shared = field.form.as<StaticField>();
  if (shared != nullptr) {
    this->declareStaticVar(classDecl, field, shared, type);
  }
}

void ClassDeclarer::resolveClass(ClassDecl *classDecl) {
  if (classDecl->stage != ClassStage::UNRESOLVED) {
    return;
  }
  classDecl->stage = ClassStage::RESOLVED;
  this->genericClassChecker->declareClassTypeParams(classDecl);
  this->state.getScopeStack().openTypeScope();
  this->genericClassChecker->bindClassTypeParams(classDecl);
  this->classBases->resolveClassBases(classDecl);
  for (size_t index = 0; index < classDecl->fields.size(); ++index) {
    this->walkMemberName(classDecl, index);
  }
  for (ClassField &field : classDecl->fields) {
    this->resolveField(classDecl, field);
  }
  this->state.getScopeStack().closeTypeScope();
}

ClassDeclarer::ResultParamTypePassed ClassDeclarer::paramType(const Param &param, const Type *type,
                                                              const Type *declared) const {
  if (type == nullptr) {
    auto message = ErrorTextParamType::extra(param.label(), declared->name());

    return err(ClassDeclarerParamTypeError{
        .code = ClassDeclarerParamTypeErrorCode::MORE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(type);
}

void ClassDeclarer::declareTyped(FunctionDecl *functionDecl, const Type *declared) {
  auto *function = this->state.getAst().newFunc();
  function->setCompleter(this->state.getCompleter());
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  functionDecl->sym = function;
  const auto &declaredParams = declared->params();
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    Param &param = functionDecl->params[index];
    const Type *given = index < declaredParams.size() ? declaredParams[index] : nullptr;
    const Type *type = this->typedParam(param, given, declared);
    param.declaredType = param.defaultValue() != nullptr ? type->nonOptional() : type;
    function->params.push_back(type);
  }
  for (size_t index = functionDecl->params.size(); index < declaredParams.size(); ++index) {
    this->addHiddenParam(functionDecl, declaredParams[index]);
  }
  function->minArgs = declared->minArgs();
  function->hasRest = declared->hasRest();
  function->setReturnType(declared->ret());
  function->discardsResult = declared->ret()->isVoid();
}

const Type *ClassDeclarer::typedParam(const Param &param, const Type *given, const Type *declared) {
  const Type *written =
      param.type.has_value() ? this->typeResolver->resolveType(*param.type) : given;
  auto passed = this->paramType(param, written, declared);
  if (passed.isErr()) {
    this->state.getDiagnostics().error(param.loc, passed.error().message);
  }
  const Type *type = passed.isOk() ? passed.value() : Type::error();
  if (param.optional() && !type->isOptional()) {
    return Type::optional(type);
  }

  return type;
}

void ClassDeclarer::addHiddenParam(FunctionDecl *functionDecl, const Type *type) {
  Param hidden;
  hidden.name = this->state.getAst().hiddenName();
  hidden.loc = functionDecl->loc();
  hidden.declaredType = type;
  functionDecl->params.push_back(hidden);
  functionDecl->sym->params.push_back(type);
}

ClassDeclarer::ResultAbstractReturns
ClassDeclarer::abstractReturn(const ClassDecl *classDecl, const FunctionDecl *method) const {
  if (method->isAbstract() && !method->returnType.has_value() && classDecl->isGeneric()) {
    auto message = ErrorTextAbstractReturn::missing(method->name, classDecl->name);

    return err(ClassDeclarerAbstractReturnError{
        .code = ClassDeclarerAbstractReturnErrorCode::NO_RETURN_TYPE,
        .message = message,
    });
  }

  return ok(method);
}

ClassDeclarer::ResultGenericIsDirect
ClassDeclarer::genericMethod(const FunctionDecl *method) const {
  bool isGeneric = !method->typeParams.empty();
  if (isGeneric && method->isAbstract()) {
    auto message = ErrorTextGenericMethod::abstract();

    return err(ClassDeclarerGenericMethodError{
        .code = ClassDeclarerGenericMethodErrorCode::ABSTRACT,
        .message = message,
    });
  }
  if (isGeneric && method->isOverride()) {
    auto message = ErrorTextGenericMethod::overrides();

    return err(ClassDeclarerGenericMethodError{
        .code = ClassDeclarerGenericMethodErrorCode::OVERRIDE,
        .message = message,
    });
  }

  return ok(method);
}

bool ClassDeclarer::isVariantOf(const FunctionDecl *earlier, const FunctionDecl *later) const {
  return earlier->name == later->name && earlier->isStatic() == later->isStatic();
}

void ClassDeclarer::numberVariants(llvm::ArrayRef<FunctionDecl *> functions) const {
  for (size_t index = 0; index < functions.size(); ++index) {
    for (const FunctionDecl *earlier : functions.take_front(index)) {
      if (this->isVariantOf(earlier, functions[index])) {
        functions[index]->member()->variant = earlier->variant() + 1;
      }
    }
  }
}

void ClassDeclarer::declareMethods(ClassDecl *classDecl) {
  for (FunctionDecl *constructor : classDecl->constructors) {
    this->functionChecker->declareSignature(constructor);
    constructor->sym->setReturnType(Type::voidTy());
  }
  for (FunctionDecl *method : classDecl->methods) {
    this->declareClassMethod(classDecl, method);
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      this->declareFieldMethod(classDecl, field);
    }
  }
  if (this->classLinks->constructorsOf(classDecl).empty()) {
    this->fillDefaultValues(classDecl);
  }
  this->makeFieldInit(classDecl);
  this->numberVariants(classDecl->methods);
  this->numberVariants(classDecl->constructors);
  this->walkVariants(classDecl);
}

void ClassDeclarer::declareClassMethod(const ClassDecl *classDecl, FunctionDecl *method) {
  this->functionChecker->declareSignature(method);
  bool isReturnOpen = method->isAbstract() && !method->returnType.has_value();
  auto returns = this->abstractReturn(classDecl, method);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(method->loc(), returns.error().message);
  }
  if (returns.isOk() && isReturnOpen) {
    method->sym->setReturnType(nullptr);
  }
  auto direct = this->genericMethod(method);
  if (direct.isErr()) {
    this->state.getDiagnostics().error(method->loc(), direct.error().message);
  }
}

void ClassDeclarer::declareFieldMethod(ClassDecl *classDecl, const ClassField &field) {
  FunctionDecl *method = field.method();
  this->state.getScopeStack().openTypeScope();
  this->genericClassChecker->bindClassTypeParams(classDecl);
  const Type *declared =
      field.type.has_value() ? this->typeResolver->resolveType(*field.type) : nullptr;
  this->state.getScopeStack().closeTypeScope();
  if (declared == nullptr || !declared->isFunction()) {
    return this->functionChecker->declareSignature(method);
  }

  return this->declareTyped(method, declared);
}

void ClassDeclarer::fillDefaultValues(ClassDecl *classDecl) {
  for (ClassField &field : classDecl->fields) {
    if (this->isNeedingDefault(field)) {
      auto visiting = llvm::SmallPtrSet<const ClassDecl *, 4>();
      visiting.insert(classDecl);
      const TypeRef *ref = field.type.has_value() ? &*field.type : nullptr;
      field.init = this->defaultValue(field.resolved, ref, field.loc, visiting);
    }
  }
}

bool ClassDeclarer::isNeedingDefault(const ClassField &field) const {
  bool isValued = field.isStatic() || field.init != nullptr || field.method() != nullptr;
  bool isLeftEmpty = field.optional() || field.parameter() || field.definite();

  return !isValued && !isLeftEmpty && field.resolved != nullptr;
}

void ClassDeclarer::makeFieldInit(ClassDecl *classDecl) {
  auto sets = this->fieldSets(classDecl);
  if (sets.empty()) {
    return;
  }
  auto *init = this->makeInitDecl(classDecl, std::move(sets));
  this->functionChecker->declareSignature(init);
  init->sym->setReturnType(Type::voidTy());
  classDecl->init = init;
}

std::vector<Stmt *> ClassDeclarer::fieldSets(const ClassDecl *classDecl) {
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

Stmt *ClassDeclarer::fieldSet(const ClassField &field) {
  auto &ast = this->state.getAst();
  auto *self = ast.make<NameExpr>(field.loc, kThisName);
  auto *target = ast.make<MemberExpr>(field.loc, self, field.name);
  auto *set = ast.make<AssignExpr>(field.loc, std::nullopt, target, field.init);

  return ast.make<ExprStmt>(field.loc, set);
}

FunctionDecl *ClassDeclarer::makeInitDecl(ClassDecl *classDecl, std::vector<Stmt *> sets) {
  auto &ast = this->state.getAst();
  auto *init = ast.make<FunctionDecl>(classDecl->loc(), kInitName);
  init->role = ClassMember{.cls = classDecl};
  init->body = ast.make<BlockStmt>(classDecl->loc());
  init->body->body = std::move(sets);

  return init;
}

Expr *ClassDeclarer::literalValue(const Type *literal, llvm::SMLoc loc) {
  auto &ast = this->state.getAst();
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

Expr *ClassDeclarer::sizeLiteral(const Type *base, double size, llvm::SMLoc loc) {
  auto &ast = this->state.getAst();
  if (base != nullptr && base->isFloat()) {
    return ast.make<FloatLitExpr>(loc, size);
  }
  auto magnitude = static_cast<uint64_t>(size);

  return ast.make<IntLitExpr>(loc, magnitude);
}

Expr *ClassDeclarer::defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                                  llvm::SmallPtrSetImpl<const ClassDecl *> &visiting) {
  auto run = DefaultRun{
      .visiting = visiting,
      .loc = loc,
      .steps = {DefaultFor{.type = type, .ref = ref}},
      .values = {},
      .hasNoDefault = false,
  };
  while (!run.steps.empty() && !run.hasNoDefault) {
    DefaultStep step = run.steps.back();
    run.steps.pop_back();
    this->applyDefaultStep(step, run);
  }
  if (run.hasNoDefault) {
    return nullptr;
  }

  return run.values.back();
}

void ClassDeclarer::applyDefaultStep(const DefaultStep &step, DefaultRun &run) {
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

void ClassDeclarer::defaultFor(const DefaultFor &wanted, DefaultRun &run) {
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

void ClassDeclarer::planClassDefault(ClassDecl *classDecl, DefaultRun &run) const {
  if (classDecl->name.empty()) {
    run.hasNoDefault = true;
    return;
  }
  run.steps.push_back(MakeNew{.classDecl = classDecl});
  run.steps.push_back(EnterEmpty{.classDecl = classDecl});
}

void ClassDeclarer::planObjectDefault(const Type *type, DefaultRun &run) {
  if (!this->isPlainObject(type)) {
    run.hasNoDefault = true;
    return;
  }
  auto *object = this->state.getAst().make<ObjectLitExpr>(run.loc);
  auto fields = this->requiredFields(type);
  run.steps.push_back(FillObject{.object = object, .fields = fields});
  for (const Field *field : llvm::reverse(fields)) {
    run.steps.push_back(DefaultFor{.type = field->type, .ref = nullptr});
  }
}

std::vector<const Field *> ClassDeclarer::requiredFields(const Type *type) const {
  auto required = std::vector<const Field *>();
  for (const Field &field : type->fields()) {
    bool isOptional = field.type != nullptr && field.type->isOptional();
    if (!isOptional) {
      required.push_back(&field);
    }
  }

  return required;
}

Expr *ClassDeclarer::simpleDefault(const Type *type, const TypeRef *ref, llvm::SMLoc loc) {
  auto &ast = this->state.getAst();
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

bool ClassDeclarer::isWrittenCollection(const Type *type, const TypeRef *ref) const {
  bool isCollection = type->isMap() || type->isSet();

  return isCollection && ref != nullptr && ref->kind() == TypeRefKind::GENERIC_REF;
}

bool ClassDeclarer::isLiteralUnion(const Type *type) const {
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

bool ClassDeclarer::isPlainObject(const Type *type) const {
  bool isSpecial = type->isTuple() || type->isAnyObject() || type->isDate() || type->isRegExp() ||
                   type->isMatch() || type->isIntl() || type->isWeakRef() ||
                   type->isRuntimeObject() || type->isBytes();

  return type->isObject() && !isSpecial && !this->hasInternalField(type);
}

bool ClassDeclarer::hasInternalField(const Type *type) const {
  for (const Field &field : type->fields()) {
    if (llvm::StringRef(field.name).starts_with(kInternalPrefix)) {
      return true;
    }
  }

  return false;
}

void ClassDeclarer::enterEmptyClass(ClassDecl *classDecl, DefaultRun &run) const {
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

bool ClassDeclarer::isMadeByConstructor(const ClassDecl *classDecl) const {
  bool isOpenClass = classDecl->isAbstract || classDecl->builtin() || classDecl->isGeneric() ||
                     !classDecl->typeParams.empty();

  return isOpenClass || !this->classLinks->constructorsOf(classDecl).empty();
}

void ClassDeclarer::leaveEmptyClass(const ClassDecl *classDecl, DefaultRun &run) const {
  run.visiting.erase(classDecl);
}

void ClassDeclarer::resolveFamily(ClassDecl *classDecl, DefaultRun &run) {
  this->resolveClass(classDecl);
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

ClassDecl *ClassDeclarer::defaultedClass(const ClassField &field) const {
  bool isValued = field.isStatic() || field.init != nullptr || field.method() != nullptr;
  if (isValued || field.optional()) {
    return nullptr;
  }

  return this->classOf(field.resolved);
}

void ClassDeclarer::makeEmptyNew(const ClassDecl *classDecl, DefaultRun &run) {
  auto *made = this->state.getAst().make<NewExpr>(run.loc, classDecl->name, std::vector<TypeRef>(),
                                                  std::vector<Expr *>());
  run.values.push_back(made);
}

void ClassDeclarer::fillObject(const FillObject &fill, DefaultRun &run) const {
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

void ClassDeclarer::walkVariantList(llvm::ArrayRef<FunctionDecl *> functions) {
  for (size_t index = 0; index < functions.size(); ++index) {
    for (FunctionDecl *earlier : functions.take_front(index)) {
      if (this->isVariantOf(earlier, functions[index])) {
        this->reportVariantPair(earlier, functions[index]);
      }
    }
  }
}

void ClassDeclarer::walkVariants(ClassDecl *classDecl) {
  this->walkVariantList(classDecl->methods);
  this->walkVariantList(classDecl->constructors);
}

std::optional<int> ClassDeclarer::ambiguousCount(const FuncSymbol *first,
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

bool ClassDeclarer::isFullyTyped(const FuncSymbol *function) const {
  return function != nullptr && !llvm::is_contained(function->params, nullptr);
}

bool ClassDeclarer::hasOtherReturn(const FuncSymbol *first, const FuncSymbol *second) const {
  bool isReturnKnown = first->hasReturnType() && second->hasReturnType();

  return isReturnKnown && first->getKnownReturnType() != second->getKnownReturnType();
}

ClassDeclarer::ResultVariantsUnequal ClassDeclarer::variantPair(const FunctionDecl *first,
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

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::ONLY_RETURN_DIFFERS,
        .message = message,
    });
  }
  if (isSameParams) {
    auto variant = ErrorTextVariant::text(second);
    auto message = ErrorTextVariantPair::duplicate(variant);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::SAME_PARAMETERS,
        .message = message,
    });
  }
  auto count = this->ambiguousCount(firstFunction, secondFunction);
  if (count.has_value()) {
    auto firstText = ErrorTextVariant::text(first);
    auto secondText = ErrorTextVariant::text(second);
    auto message = ErrorTextVariantPair::ambiguous(firstText, secondText, *count);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::SAME_FOR_COUNT,
        .message = message,
    });
  }

  return ok(second);
}

void ClassDeclarer::reportVariantPair(FunctionDecl *first, FunctionDecl *second) {
  auto unequal = this->variantPair(first, second);
  if (unequal.isErr()) {
    auto other = ErrorTextVariantPair::other();
    this->state.getDiagnostics().error(second->loc(), unequal.error().message);
    this->state.getDiagnostics().note(first->loc(), other);
  }
}

ClassDeclarer::ResultNotParentMethod ClassDeclarer::parentMethod(const ClassDecl *classDecl,
                                                                 const ClassField &field) const {
  if (classDecl->parent == nullptr) {
    return ok(&field);
  }
  auto inherited = this->methodsNamed(classDecl->parent, field.name);
  if (!inherited.empty()) {
    auto message = ErrorTextParentMethod::clash(field.name, classDecl->parent->name);

    return err(ClassDeclarerParentMethodError{
        .code = ClassDeclarerParentMethodErrorCode::PARENT_METHOD,
        .message = message,
    });
  }

  return ok(&field);
}

ClassDeclarer::ResultNarrowingLayout ClassDeclarer::narrowedField(const ClassDecl *classDecl,
                                                                  const ClassField &field,
                                                                  const Field &inherited,
                                                                  const ClassDecl *owner) const {
  bool isOtherLayout = inherited.type != field.resolved &&
                       !WideningRelations::isSameReference(field.resolved, inherited.type);
  if (isOtherLayout) {
    auto message = ErrorTextNarrowedField::layout(
        field.name, classDecl->name, field.resolved->name(), owner->name, inherited.type->name());

    return err(ClassDeclarerNarrowedFieldError{
        .code = ClassDeclarerNarrowedFieldErrorCode::OTHER_LAYOUT,
        .message = message,
    });
  }

  return ok(&field);
}

void ClassDeclarer::completeClass(ClassDecl *classDecl) {
  std::vector<CompletionStep> steps = {StartCompletion{.classDecl = classDecl}};
  while (!steps.empty()) {
    CompletionStep step = steps.back();
    steps.pop_back();
    const StartCompletion *start = step.as<StartCompletion>();
    if (start != nullptr) {
      this->startCompletion(start->classDecl, steps);
    } else {
      this->finishCompletion(step.as<FinishCompletion>()->classDecl);
    }
  }
}

void ClassDeclarer::startCompletion(ClassDecl *classDecl, std::vector<CompletionStep> &steps) {
  bool isIncomplete = classDecl->type->isIncomplete() && classDecl->literal() == nullptr;
  if (!isIncomplete || this->state.getClassStore().isCompleting(classDecl)) {
    return;
  }
  this->state.getClassStore().enterCompletion(classDecl);
  steps.push_back(FinishCompletion{.classDecl = classDecl});
  ClassDecl *generic = classDecl->generic();
  if (generic != nullptr) {
    steps.push_back(StartCompletion{.classDecl = generic});
  }
  if (classDecl->parent != nullptr) {
    steps.push_back(StartCompletion{.classDecl = classDecl->parent});
  }
}

void ClassDeclarer::finishCompletion(ClassDecl *classDecl) {
  auto *type = const_cast<Type *>(classDecl->type);
  const ClassDecl *generic = classDecl->generic();
  if (generic != nullptr) {
    this->resolveInstanceFields(classDecl, generic);
    this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  }
  auto fields = this->inheritedFields(classDecl);
  for (ClassField &field : classDecl->fields) {
    if (!field.isStatic() && field.method() == nullptr) {
      this->completeField(classDecl, field, fields);
    }
  }
  type->defineFields(std::move(fields));
  this->state.getClassStore().leaveCompletion(classDecl);
  if (generic != nullptr) {
    this->state.getDiagnostics().popList();
  }
}

void ClassDeclarer::resolveInstanceFields(ClassDecl *instance, const ClassDecl *generic) {
  for (size_t index = 0; index < instance->fields.size() && index < generic->fields.size();
       ++index) {
    ClassField &field = instance->fields[index];
    bool isUnresolved = !field.isStatic() && field.method() == nullptr && field.resolved == nullptr;
    if (isUnresolved) {
      field.resolved = this->instanceFieldType(instance, generic->fields[index].resolved);
    }
  }
}

const Type *ClassDeclarer::instanceFieldType(const ClassDecl *instance, const Type *original) {
  if (original == nullptr) {
    return Type::error();
  }

  return this->genericClassChecker->instanceValueType(instance, original);
}

std::vector<Field> ClassDeclarer::inheritedFields(const ClassDecl *classDecl) const {
  if (classDecl->parent == nullptr) {
    return {};
  }

  return classDecl->parent->type->fields();
}

void ClassDeclarer::completeField(const ClassDecl *classDecl, ClassField &field,
                                  std::vector<Field> &fields) {
  if (field.resolved == nullptr) {
    field.resolved = Type::error();
  }
  if (field.redeclared) {
    return this->narrowInheritedField(classDecl, field, fields);
  }

  return this->addOwnField(classDecl, field, fields);
}

void ClassDeclarer::addOwnField(const ClassDecl *classDecl, const ClassField &field,
                                std::vector<Field> &fields) {
  auto own = this->parentMethod(classDecl, field);
  if (own.isErr()) {
    this->state.getDiagnostics().error(field.loc, own.error().message);
  }
  auto added = Field{
      .name = field.name.str(),
      .type = field.resolved,
      .readonly = field.readonly,
  };
  fields.push_back(added);
}

void ClassDeclarer::narrowInheritedField(const ClassDecl *classDecl, const ClassField &field,
                                         std::vector<Field> &fields) {
  Field *inherited = this->fieldNamed(fields, field.name);
  if (inherited == nullptr || field.resolved->isError() || inherited->type->isError()) {
    return;
  }
  const ClassDecl *owner = nullptr;
  this->classBases->findField(classDecl->parent, field.name, &owner);
  auto layout = this->narrowedField(classDecl, field, *inherited, owner);
  if (layout.isErr()) {
    this->state.getDiagnostics().error(field.loc, layout.error().message);
    return;
  }
  bool isNarrowed = inherited->type != field.resolved || (field.readonly && !inherited->readonly);
  if (isNarrowed) {
    this->state.getWideningStore().markFieldsNarrowed();
  }
  inherited->type = field.resolved;
  inherited->readonly = field.readonly;
}

Field *ClassDeclarer::fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

}
