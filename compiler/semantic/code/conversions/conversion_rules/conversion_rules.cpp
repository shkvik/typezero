#include "compiler/semantic/code/conversions/conversion_rules/conversion_rules.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cctype>

using llvm::dyn_cast;

namespace tz {

ConversionRules::ConversionRules(SemanticState &owner)
    : state(owner) {}

Expr *ConversionRules::convertExpr(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError()) {
    return expr;
  }
  if (expr->type->isParam() && !TypeQueries::mentionsParam(to)) {
    const Expr *bare = ExprParts::withoutImplicitCasts(expr);
    this->state.generics.copies.rememberForcedType(bare, to);
  }
  IntLitExpr *literal = this->retypedLiteral(expr, to);
  if (literal != nullptr) {
    literal->type = to;
    expr->type = to;

    return expr;
  }
  this->rememberView(expr->type, to);
  auto *castExpr = this->state.ast.make<CastExpr>(expr->loc(), expr, std::nullopt);
  castExpr->type = to;
  if (this->isWideningWrites(expr->type, to)) {
    this->state.inventory.wideningStore.rememberWideningCast(castExpr);
  }
  if (this->isReflectingTarget(to)) {
    this->state.walk.tasks.pushTask(NoteReflectedConversion{
        .from = expr->type,
        .to = to,
        .castExpr = castExpr,
    });
  }
  this->state.walk.tasks.pushTask(NoteConstructorFlow{.from = expr->type, .to = to});

  return castExpr;
}

Expr *ConversionRules::adaptLiteralExpr(Expr *expr, const Type *to) {
  const auto *target = this->state.code.conversions.literalTarget(expr, to);
  if (target == nullptr) {
    return expr;
  }
  auto *adapted = this->convertExpr(expr, target);
  if (target == to) {
    return adapted;
  }

  return this->convertExpr(adapted, to);
}

bool ConversionRules::isWideningWrites(const Type *from, const Type *to) {
  const Type *bareFrom = from->nonOptional();
  const Type *bareTo = to->nonOptional();
  if (bareFrom == bareTo || GenericWidening::isWidening(bareFrom, bareTo)) {
    return false;
  }
  bool hasArrays = bareFrom->isArray() && bareTo->isArray();
  bool hasObjects = WideningRelations::areObjects(bareFrom, bareTo);
  bool isUnionToObject = bareFrom->isUnion() && bareTo->isObject();
  if (!hasArrays && !hasObjects && !isUnionToObject) {
    return false;
  }
  auto &widenings = this->state.types.widenings;
  if (widenings.hasKnownWidening(bareFrom, bareTo)) {
    return widenings.isNarrowingWidening(bareFrom, bareTo);
  }
  auto narrowings = WideningRelations::narrowingsOf(bareFrom, bareTo);
  bool isNarrowed = !narrowings.empty();
  widenings.rememberWidening(bareFrom, bareTo, isNarrowed);

  return isNarrowed;
}

std::string ConversionRules::objectMismatch(const Type *from, const Type *to,
                                            bool isConflict) const {
  for (const Field &field : from->fields()) {
    if (to->field(field.name) == nullptr) {
      return ErrorTextMismatch::extraField(field.name);
    }
  }
  for (const Field &target : to->fields()) {
    if (from->field(target.name) == nullptr) {
      return ErrorTextMismatch::noField(target.name);
    }
  }
  for (const Field &field : from->fields()) {
    const auto *target = to->field(field.name);
    if (target->type != field.type) {
      return this->fieldTypeMismatch(field, *target, to, isConflict);
    }
    if (field.readonly && !target->readonly) {
      return ErrorTextMismatch::readonly(field.name, to->name());
    }
  }

  return "";
}

std::string ConversionRules::fieldTypeMismatch(const Field &field, const Field &target,
                                               const Type *to, bool isConflict) const {
  auto message =
      ErrorTextMismatch::fieldType(field.name, field.type->name(), target.type->name(), to->name());
  bool isNumberPair =
      field.type->nonOptional()->isNumeric() && target.type->nonOptional()->isNumeric();
  if (!isConflict && isNumberPair) {
    message += ErrorTextMismatch::typeHint(to->name());
  }

  return message;
}

std::string ConversionRules::hintName(const Expr *expr) const {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr) {
    return "x";
  }

  return name->name.str();
}

std::string ConversionRules::unknownHint(const Expr *expr, const Type *to) const {
  auto shown = this->hintName(expr);
  auto check = this->narrowingCheck(shown, expr, to);
  bool isObject = expr->type != nullptr && expr->type->isAnyObject();
  if (isObject) {
    return ErrorTextUnknownHint::anyClass(check);
  }

  return ErrorTextUnknownHint::anything(check);
}

std::string ConversionRules::narrowingCheck(llvm::StringRef shown, const Expr *expr,
                                            const Type *to) const {
  bool isObject = expr->type != nullptr && expr->type->isAnyObject();
  if (this->isNamedClass(to)) {
    return ErrorTextUnknownHint::instanceOf(shown, to->nonOptional()->classDecl()->name);
  }
  if (isObject) {
    return ErrorTextUnknownHint::instanceOf(shown, "C");
  }
  if (to == nullptr) {
    return ErrorTextUnknownHint::typeOf(shown, TypeOfNameTable::getName(TypeOfName::STRING));
  }

  return this->typeOfCheck(shown, to->nonOptional());
}

bool ConversionRules::isNamedClass(const Type *to) const {
  if (to == nullptr || !to->nonOptional()->isClass()) {
    return false;
  }

  return to->nonOptional()->classDecl()->literal() == nullptr;
}

std::string ConversionRules::typeOfCheck(llvm::StringRef shown, const Type *target) const {
  if (target->isString()) {
    return ErrorTextUnknownHint::typeOf(shown, TypeOfNameTable::getName(TypeOfName::STRING));
  }
  if (target->isNumeric()) {
    return ErrorTextUnknownHint::typeOf(shown, TypeOfNameTable::getName(TypeOfName::NUMBER));
  }
  if (target->isBool()) {
    return ErrorTextUnknownHint::typeOf(shown, TypeOfNameTable::getName(TypeOfName::BOOLEAN));
  }
  if (target->isArray()) {
    return ErrorTextUnknownHint::isArray(shown);
  }
  if (target->isAnyObject()) {
    return ErrorTextUnknownHint::typeOf(shown, TypeOfNameTable::getName(TypeOfName::OBJECT));
  }

  return ErrorTextUnknownHint::typeOf(shown, TypeOfNameTable::getName(TypeOfName::STRING));
}

bool ConversionRules::isReflectingTarget(const Type *to) const {
  const Type *reflected = to->nonOptional();
  bool isUnknownArray = reflected->isArray() && reflected->inner()->isUnknown();

  return reflected->isConstructor() || reflected->isAnyObject() || reflected->isUnknown() ||
         isUnknownArray;
}

IntLitExpr *ConversionRules::retypedLiteral(Expr *expr, const Type *to) const {
  bool isNumberChange = this->state.code.conversions.isNumberLike(to) && expr->type->isInteger();
  if (!isNumberChange) {
    return nullptr;
  }
  auto *unary = dyn_cast<UnaryExpr>(expr);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  Expr *written = isNegated ? unary->operand : expr;

  return dyn_cast<IntLitExpr>(written);
}

void ConversionRules::rememberView(const Type *from, const Type *to) {
  const Type *fromObject = this->innerObject(from, to);
  const Type *toObject = this->innerObject(to, from);
  bool isPlainPair = fromObject->isObject() && toObject->isObject() && !fromObject->isClass() &&
                     !toObject->isClass() && !fromObject->isAnyObject() &&
                     !toObject->isAnyObject() && !fromObject->isBytes();
  bool isViewed = isPlainPair && fromObject != toObject && !fromObject->isSubtypeOf(toObject);
  if (isViewed) {
    this->addView(fromObject, toObject);
  }
}

void ConversionRules::walkAssignable(const Expr *expr, const Type *to, llvm::StringRef what) {
  auto assigned = this->assignable(expr, to, what);
  const auto *other = this->otherPlace(expr, to);
  if (assigned.isErr()) {
    this->state.diagnostics.error(expr->loc(), assigned.error().message);
  }
  if (assigned.isErr() && other != nullptr) {
    auto otherLoc = this->placeLoc(expr, other);
    auto alsoHere = ErrorTextAssignable::alsoHere(other->name());
    this->state.diagnostics.note(otherLoc, alsoHere);
  }
}

const Type *ConversionRules::innerObject(const Type *type, const Type *other) const {
  const Type *inner = type->nonOptional();
  const Type *otherInner = other->nonOptional();
  while (inner->isArray() && otherInner->isArray()) {
    inner = inner->inner()->nonOptional();
    otherInner = otherInner->inner()->nonOptional();
  }

  return inner;
}

const Type *ConversionRules::otherPlace(const Expr *expr, const Type *to) const {
  const auto *toObject = this->innerObject(to, expr->type);
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || name->var() == nullptr) {
    return nullptr;
  }
  for (auto [type, loc] : this->state.inference.destinations.getDestinations(name->var())) {
    if (type->nonOptional() != toObject) {
      return type->nonOptional();
    }
  }

  return nullptr;
}

llvm::SMLoc ConversionRules::placeLoc(const Expr *expr, const Type *place) const {
  auto *name = dyn_cast<NameExpr>(expr);
  for (auto [type, loc] : this->state.inference.destinations.getDestinations(name->var())) {
    if (type->nonOptional() == place) {
      return loc;
    }
  }

  return llvm::SMLoc();
}

ConversionRules::ResultTypeAssignable ConversionRules::assignable(const Expr *expr, const Type *to,
                                                                  llvm::StringRef what) const {
  auto attempt = this->describeAttempt(expr, to);
  auto code = this->mismatchOf(attempt);
  auto prefix = this->mismatchPrefix(attempt, what);
  auto reason = this->mismatchReason(attempt, code);
  std::string message = prefix + reason;

  return err(CodeConversionsRegistryAssignableError{
      .code = code,
      .message = message,
  });
}

ConversionRules::AssignAttempt ConversionRules::describeAttempt(const Expr *expr,
                                                                const Type *to) const {
  const Type *from = expr->type;

  return AssignAttempt{
      .expr = expr,
      .from = from,
      .to = to,
      .fromObject = this->innerObject(from, to),
      .toObject = this->innerObject(to, from),
  };
}

CodeConversionsRegistryAssignableErrorCode
ConversionRules::mismatchOf(const AssignAttempt &attempt) const {
  const Type *from = attempt.from;
  const Type *to = attempt.to;
  const Type *fromObject = attempt.fromObject;
  const Type *toObject = attempt.toObject;
  const ClassDecl *fromClass = fromObject->classDecl();
  const ClassDecl *genericClass = from->classRef();
  bool isGenericValue =
      genericClass != nullptr && genericClass->isGeneric() && toObject->isConstructor();
  if (isGenericValue) {
    return CodeConversionsRegistryAssignableErrorCode::GENERIC_CLASS_VALUE;
  }
  if (from->isUnknown() || from->isAnyObject()) {
    return CodeConversionsRegistryAssignableErrorCode::UNKNOWN_VALUE;
  }
  bool isLiteralObject = fromClass == nullptr || fromClass->literal() != nullptr;
  if (fromObject->isObject() && toObject->isClass() && isLiteralObject) {
    return CodeConversionsRegistryAssignableErrorCode::LITERAL_NOT_INSTANCE;
  }
  if (fromClass != nullptr && fromClass->literal() != nullptr && toObject->isObject()) {
    return CodeConversionsRegistryAssignableErrorCode::LITERAL_WITH_METHODS;
  }
  auto widening = this->state.generics.variance.whyNot(fromObject, toObject);
  if (!widening.empty()) {
    return CodeConversionsRegistryAssignableErrorCode::GENERIC_WIDENING;
  }
  bool isClassValue = fromObject->isClass() && toObject->isObject() && fromObject != toObject;
  if (isClassValue && toObject->isClass()) {
    return CodeConversionsRegistryAssignableErrorCode::CLASS_NOT_EXTENDED;
  }
  if (isClassValue) {
    return CodeConversionsRegistryAssignableErrorCode::CLASS_NOT_IMPLEMENTED;
  }
  bool isObjectIntoUnion = fromObject->isObject() && !fromObject->isClass() &&
                           toObject->isUnion() && this->hasObjectMember(toObject);
  if (isObjectIntoUnion) {
    return CodeConversionsRegistryAssignableErrorCode::OBJECT_INTO_UNION;
  }
  if (fromObject->isTuple() && toObject->isArray()) {
    return CodeConversionsRegistryAssignableErrorCode::TUPLE_NOT_ARRAY;
  }
  bool hasDifferentObjects =
      fromObject->isObject() && toObject->isObject() &&
      !this->state.types.relations.isImplicitlyConvertible(fromObject, toObject);
  if (hasDifferentObjects) {
    return CodeConversionsRegistryAssignableErrorCode::OBJECT_MISMATCH;
  }
  if (from->isOptional() &&
      this->state.types.relations.isImplicitlyConvertible(from->inner(), to)) {
    return CodeConversionsRegistryAssignableErrorCode::MAY_BE_UNDEFINED;
  }
  if (from->nonOptional()->isJson()) {
    return CodeConversionsRegistryAssignableErrorCode::JSON_UNCHECKED;
  }
  if (to->nonOptional()->isAnyObject()) {
    return CodeConversionsRegistryAssignableErrorCode::NOT_AN_OBJECT;
  }
  const Type *value = from->nonOptional();
  bool isIntoJson = to->nonOptional()->isJson();
  if (isIntoJson && (fromObject->isObject() || value->isArray())) {
    return CodeConversionsRegistryAssignableErrorCode::OBJECT_INTO_JSON;
  }
  if (isIntoJson && value->isRecord() &&
      this->state.types.relations.isHoldingWideInteger(value->valueType())) {
    return CodeConversionsRegistryAssignableErrorCode::WIDE_INTEGERS_INTO_JSON;
  }
  if (isIntoJson && value->isDecimal()) {
    return CodeConversionsRegistryAssignableErrorCode::DECIMAL_INTO_JSON;
  }
  if (from->isFloat() && to->isDecimal()) {
    return CodeConversionsRegistryAssignableErrorCode::DOUBLE_INTO_DECIMAL;
  }
  if (from->isDecimal() && (to->isNumeric() || to->nonOptional()->isNumeric())) {
    return CodeConversionsRegistryAssignableErrorCode::DECIMAL_INTO_NUMBER;
  }

  return CodeConversionsRegistryAssignableErrorCode::NOT_ASSIGNABLE;
}

std::string ConversionRules::mismatchPrefix(const AssignAttempt &attempt,
                                            llvm::StringRef what) const {
  const Type *from = attempt.from;
  const Type *to = attempt.to;
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  auto prefix = ErrorTextAssignable::notTo(from->name(), to->name(), what);
  if (from->isFunction() && function->isFunction()) {
    prefix += ErrorTextAssignable::functions();
  }
  if (this->isStreamIntoBody(attempt)) {
    prefix += ErrorTextAssignable::stream();
  }
  if (this->isRequestInit(attempt)) {
    prefix += ErrorTextAssignable::request();
  }
  if (to->isNever()) {
    prefix += ErrorTextAssignable::never();
  }

  return prefix;
}

bool ConversionRules::hasObjectMember(const Type *unionType) const {
  for (const Type *member : unionType->members()) {
    if (member->isObject()) {
      return true;
    }
  }

  return false;
}

bool ConversionRules::isStreamIntoBody(const AssignAttempt &attempt) const {
  if (!this->isWebStream(attempt.fromObject)) {
    return false;
  }
  const Type *toObject = attempt.toObject;
  llvm::ArrayRef<const Type *> wanted =
      toObject->isUnion() ? toObject->members() : llvm::ArrayRef<const Type *>(toObject);
  bool hasFormData = false;
  bool hasWebStream = false;
  for (const Type *member : wanted) {
    hasFormData = hasFormData || this->isFormData(member);
    hasWebStream = hasWebStream || this->isWebStream(member);
  }

  return hasFormData && !hasWebStream;
}

bool ConversionRules::isWebStream(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  bool isLibraryGeneric =
      classDecl != nullptr && classDecl->library() && classDecl->generic() != nullptr;

  return isLibraryGeneric &&
         LibraryClassTable::isName(classDecl->generic()->name, LibraryClass::READABLE_STREAM) &&
         classDecl->typeArgs.size() == 2;
}

bool ConversionRules::isFormData(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();

  return classDecl != nullptr && classDecl->library() &&
         LibraryClassTable::isName(classDecl->name, LibraryClass::FORM_DATA);
}

bool ConversionRules::isRequestInit(const AssignAttempt &attempt) const {
  llvm::StringRef fromName = attempt.fromObject->name();

  auto requestInit = LibraryTypeTable::getName(LibraryType::REQUEST_INIT);
  auto requestOptions = LibraryTypeTable::getName(LibraryType::REQUEST_OPTIONS);

  return fromName.starts_with(requestInit) && attempt.toObject->name() == requestOptions;
}

std::string ConversionRules::mismatchReason(const AssignAttempt &attempt,
                                            CodeConversionsRegistryAssignableErrorCode code) const {
  const Type *fromObject = attempt.fromObject;
  const Type *toObject = attempt.toObject;
  switch (code) {
    case CodeConversionsRegistryAssignableErrorCode::GENERIC_CLASS_VALUE:
      return this->genericClassText(attempt.from->classRef());
    case CodeConversionsRegistryAssignableErrorCode::UNKNOWN_VALUE:
      return this->unknownHint(attempt.expr, attempt.to);
    case CodeConversionsRegistryAssignableErrorCode::LITERAL_NOT_INSTANCE:
      return ErrorTextAssignable::literal(toObject->name());
    case CodeConversionsRegistryAssignableErrorCode::LITERAL_WITH_METHODS:
      return ErrorTextAssignable::methods();
    case CodeConversionsRegistryAssignableErrorCode::GENERIC_WIDENING:
      return this->state.generics.variance.whyNot(fromObject, toObject);
    case CodeConversionsRegistryAssignableErrorCode::CLASS_NOT_EXTENDED:
      return ErrorTextAssignable::classes(fromObject->name(), toObject->name());
    case CodeConversionsRegistryAssignableErrorCode::CLASS_NOT_IMPLEMENTED:
      return ErrorTextAssignable::implements(toObject->name());
    case CodeConversionsRegistryAssignableErrorCode::OBJECT_INTO_UNION:
      return this->unionText(toObject);
    case CodeConversionsRegistryAssignableErrorCode::TUPLE_NOT_ARRAY:
      return ErrorTextAssignable::tuple();
    case CodeConversionsRegistryAssignableErrorCode::OBJECT_MISMATCH:
      return this->objectMismatchText(attempt);
    case CodeConversionsRegistryAssignableErrorCode::MAY_BE_UNDEFINED:
      return ErrorTextAssignable::undefined();
    case CodeConversionsRegistryAssignableErrorCode::JSON_UNCHECKED:
      return ErrorTextAssignable::json();
    case CodeConversionsRegistryAssignableErrorCode::NOT_AN_OBJECT:
      return ErrorTextAssignable::object();
    case CodeConversionsRegistryAssignableErrorCode::OBJECT_INTO_JSON:
      return ErrorTextAssignable::jsonObject();
    case CodeConversionsRegistryAssignableErrorCode::WIDE_INTEGERS_INTO_JSON:
      return ErrorTextAssignable::wideJson();
    case CodeConversionsRegistryAssignableErrorCode::DECIMAL_INTO_JSON:
      return ErrorTextAssignable::noDecimal();
    case CodeConversionsRegistryAssignableErrorCode::DOUBLE_INTO_DECIMAL:
      return ErrorTextAssignable::toDecimal();
    case CodeConversionsRegistryAssignableErrorCode::DECIMAL_INTO_NUMBER:
      return ErrorTextAssignable::toNumber();
    case CodeConversionsRegistryAssignableErrorCode::NOT_ASSIGNABLE:
      return this->state.code.conversions.conversionHint(attempt.expr, attempt.to);
  }

  return this->state.code.conversions.conversionHint(attempt.expr, attempt.to);
}

std::string ConversionRules::genericClassText(const ClassDecl *generic) const {
  auto name = generic->name.str();
  auto example = generic->library() ? ErrorTextAssignable::extendsBus(name)
                                    : ErrorTextAssignable::extendsInt(name);

  return ErrorTextAssignable::generic(name, example);
}

std::string ConversionRules::unionText(const Type *toObject) const {
  auto text = ErrorTextAssignable::inUnion();
  const Type *front = toObject->members().front();
  if (!front->isObject()) {
    return text;
  }
  const Field *tag = this->sharedLiteralField(toObject);
  if (tag != nullptr) {
    text += ErrorTextAssignable::withValue(tag->name);
  }

  return text;
}

const Field *ConversionRules::sharedLiteralField(const Type *unionType) const {
  for (const Field &field : unionType->members().front()->fields()) {
    if (this->hasLiteralFieldInAll(unionType->members(), field.name)) {
      return &field;
    }
  }

  return nullptr;
}

bool ConversionRules::hasLiteralFieldInAll(llvm::ArrayRef<const Type *> members,
                                           llvm::StringRef name) const {
  for (const Type *member : members) {
    const Field *field = member->isObject() ? member->field(name) : nullptr;
    bool isLiteral = field != nullptr && field->type->isLiteral();
    if (!isLiteral) {
      return false;
    }
  }

  return true;
}

std::string ConversionRules::objectMismatchText(const AssignAttempt &attempt) const {
  const auto *other = this->otherPlace(attempt.expr, attempt.to);
  auto text = this->objectMismatch(attempt.fromObject, attempt.toObject, other != nullptr);
  if (other != nullptr) {
    text += ErrorTextAssignable::both(other->name());
  }

  return text;
}

void ConversionRules::addView(const Type *from, const Type *to) {
  std::pair<const Type *, const Type *> view(from, to);
  if (!llvm::is_contained(this->state.inventory.views, view)) {
    this->state.inventory.views.push_back(view);
  }
}

}
