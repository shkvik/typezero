#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker_errors.h"
#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr llvm::StringLiteral kValuePlaceholder = "x";

}

JsonChecker::JsonChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->binaryChecker = &all.binaryChecker;
  this->argumentChecker = &all.argumentChecker;
  this->arrayLiteralChecker = &all.arrayLiteralChecker;
}

const VarSymbol *JsonChecker::jsonVariable(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name != nullptr && name->var() != nullptr && name->var()->type->nonOptional()->isJson()
             ? name->var()
             : nullptr;
}

bool JsonChecker::isNullName(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);

  return name != nullptr && KeywordTable::find(name->name) == Keyword::NULL_KEYWORD &&
         name->var() == nullptr && name->func() == nullptr;
}

bool JsonChecker::isNothing(const Expr *expr) const {
  return this->isNullName(expr) || (expr->type != nullptr && expr->type->isUndefined());
}

const Expr *JsonChecker::bare(const Expr *expr) const {
  if (expr == nullptr) {
    return nullptr;
  }

  return ExprParts::withoutImplicitCasts(expr);
}

bool JsonChecker::isSameRef(const Expr *first, const Expr *second) const {
  const auto *left = this->bare(first);
  const auto *right = this->bare(second);
  const auto *leftMember = dyn_cast_or_null<MemberExpr>(left);
  const auto *rightMember = dyn_cast_or_null<MemberExpr>(right);
  while (leftMember != nullptr && rightMember != nullptr) {
    if (leftMember->name != rightMember->name) {
      return false;
    }
    left = this->bare(leftMember->object);
    right = this->bare(rightMember->object);
    leftMember = dyn_cast_or_null<MemberExpr>(left);
    rightMember = dyn_cast_or_null<MemberExpr>(right);
  }
  const auto *leftName = dyn_cast_or_null<NameExpr>(left);
  const auto *rightName = dyn_cast_or_null<NameExpr>(right);
  if (leftName == nullptr || rightName == nullptr) {
    return false;
  }

  return leftName->var() != nullptr && leftName->var() == rightName->var();
}

std::string JsonChecker::refText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = this->bare(expr);
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    names.push_back(member->name);
    link = this->bare(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  const auto *root = dyn_cast<NameExpr>(link);
  std::string text = root != nullptr ? root->name.str() : kValuePlaceholder.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

void JsonChecker::walkRedundantTests(const BinaryExpr *binary) {
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto terms = ExprParts::sameOperatorParts(binary->lhs, binary->op);
  const auto *rhs = this->bare(binary->rhs);
  auto test = this->jsonTest(rhs);
  if (!test.has_value()) {
    return;
  }
  for (const Expr *term : terms) {
    if (this->reportRedundantTest(term, test.value(), isAnd, rhs)) {
      return;
    }
  }
}

std::optional<JsonChecker::JsonTest> JsonChecker::jsonTest(const Expr *rhs) const {
  auto found = this->testedSide(rhs);
  if (!found.has_value()) {
    return std::nullopt;
  }
  const auto *tested = this->bare(found->tested);
  if (tested->type == nullptr || !tested->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return found;
}

std::optional<JsonChecker::JsonTest> JsonChecker::testedSide(const Expr *rhs) const {
  const auto *equality = dyn_cast<BinaryExpr>(rhs);
  bool isEquality =
      equality != nullptr && (equality->op == BinaryOp::Eq || equality->op == BinaryOp::Ne);
  if (isEquality && this->isNullComparison(equality)) {
    bool isLeftNull = this->isNullName(this->bare(equality->lhs));
    const Expr *tested = isLeftNull ? equality->rhs : equality->lhs;
    return JsonTest{.tested = tested, .isNullTest = true};
  }
  const auto *inner = this->withoutNot(rhs);
  const auto *call = dyn_cast<CallExpr>(inner);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    return JsonTest{.tested = call->args[0], .isNullTest = false};
  }

  return std::nullopt;
}

bool JsonChecker::isNullComparison(const BinaryExpr *equality) const {
  return this->isNullName(this->bare(equality->lhs)) || this->isNullName(this->bare(equality->rhs));
}

const Expr *JsonChecker::withoutNot(const Expr *expr) const {
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    return this->bare(unary->operand);
  }

  return expr;
}

bool JsonChecker::reportRedundantTest(const Expr *term, const JsonTest &test, bool isAnd,
                                      const Expr *rhs) {
  const auto *text = this->typeOfTextFor(term, test.tested, isAnd);
  if (text == nullptr) {
    return false;
  }
  auto name = ToUtf8::toUtf8(text->value);
  auto agrees = this->checkTypeOfAgrees(test.tested, name, test.isNullTest, isAnd);
  if (agrees.isOk()) {
    return false;
  }
  this->state.getDiagnostics().error(rhs->loc(), agrees.error().message);

  return true;
}

const StringLitExpr *JsonChecker::typeOfTextFor(const Expr *term, const Expr *tested,
                                                bool isAnd) const {
  const auto *comparison = dyn_cast<BinaryExpr>(this->bare(term));
  BinaryOp wanted = isAnd ? BinaryOp::Eq : BinaryOp::Ne;
  if (comparison == nullptr || comparison->op != wanted) {
    return nullptr;
  }
  auto pair = this->typeOfPair(comparison);
  bool isTypeOfTest = pair.typeOf != nullptr && pair.typeOf->op == UnaryOp::TypeOf &&
                      pair.text != nullptr && this->isSameRef(pair.typeOf->operand, tested);

  return isTypeOfTest ? pair.text : nullptr;
}

JsonChecker::TypeOfPair JsonChecker::typeOfPair(const BinaryExpr *comparison) const {
  const auto *left = this->bare(comparison->lhs);
  const auto *right = this->bare(comparison->rhs);
  auto straight = TypeOfPair{
      .typeOf = dyn_cast<UnaryExpr>(left),
      .text = dyn_cast<StringLitExpr>(right),
  };
  if (straight.typeOf != nullptr && straight.text != nullptr) {
    return straight;
  }

  return TypeOfPair{
      .typeOf = dyn_cast<UnaryExpr>(right),
      .text = dyn_cast<StringLitExpr>(left),
  };
}

JsonChecker::ResultTypeOfAgrees JsonChecker::checkTypeOfAgrees(const Expr *tested,
                                                               llvm::StringRef name, bool nullTest,
                                                               bool isAnd) const {
  auto typeOf = TypeOfNameTable::find(name);
  bool isNullName = typeOf == TypeOfName::NULL_TYPE || typeOf == TypeOfName::UNDEFINED_TYPE;
  if (nullTest && !isNullName) {
    auto message = ErrorTextJsonTest::neverNull(this->refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_NULL,
        .message = message,
    });
  }
  if (!nullTest && typeOf != TypeOfName::ARRAY) {
    auto message = ErrorTextJsonTest::neverArray(this->refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_ARRAY,
        .message = message,
    });
  }

  return ok(tested);
}

JsonChecker::ResultNullPossible JsonChecker::checkNullPossible(const Type *type,
                                                               llvm::StringRef op) const {
  if (!type->isError() && !type->nonOptional()->isJson()) {
    auto message = ErrorTextJsonNull::never(type->name(), op);

    return err(JsonCheckerNullPossibleError{
        .code = JsonCheckerNullPossibleErrorCode::NEVER_NULL,
        .message = message,
    });
  }

  return ok(type);
}

JsonChecker::ResultTestsUnknown JsonChecker::checkTestsUnknown(const Type *type) {
  if (!type->isUnknown() && !type->nonOptional()->isParam() &&
      !this->state.getGenericStore().isCheckingInstance()) {
    auto message = ErrorTextJsonIsJson::known(type->name(), type->nonOptional()->isJson());

    return err(JsonCheckerTestsUnknownError{
        .code = JsonCheckerTestsUnknownErrorCode::KNOWN_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

bool JsonChecker::isHoldingJson(const Type *type) const {
  const Type *current = type;
  while (current->isOptional() || current->isArray()) {
    current = current->inner();
  }

  return current->isJson();
}

Expr *JsonChecker::checkNull(NameExpr *name, const Type *expected) {
  if (!this->isNullName(name) || expected == nullptr || !expected->nonOptional()->isJson()) {
    return nullptr;
  }
  name->type = Type::json();

  return name;
}

Expr *JsonChecker::checkJsonMember(MemberExpr *member) {
  member->type = Type::optional(Type::json());
  auto *narrowed = this->fieldFacts->narrowedField(member);
  if (narrowed != member) {
    return narrowed;
  }
  auto path = this->fieldFacts->fieldPath(member);
  if (path && this->fieldFacts->isFieldDefined(*path)) {
    return this->typeRelations->convert(member, Type::json());
  }

  return member;
}

Expr *JsonChecker::checkJsonIndex(IndexExpr *index) {
  index->index = this->exprChecker->checkExpr(index->index);
  index->type = Type::optional(Type::json());
  const Type *key = index->index->type;
  if (key->isError() || key->isString()) {
    return index;
  }
  if (key->isNumeric()) {
    index->index = this->typeRelations->convert(index->index, Type::doubleTy());
    return index;
  }
  auto message = ErrorTextJsonKey::index(key->name());
  this->state.getDiagnostics().error(index->index->loc(), message);
  index->type = Type::error();

  return index;
}

const Type *JsonChecker::checkJsonTarget(Expr *target) {
  auto *index = dyn_cast<IndexExpr>(target);
  if (index != nullptr) {
    if (this->checkJsonIndex(index)->type->isError()) {
      return Type::error();
    }
  } else {
    this->checkJsonMember(cast<MemberExpr>(target));
  }

  return Type::optional(Type::json());
}

Expr *JsonChecker::reportJsonMethod(CallExpr *call, MemberExpr *member) {
  const Expr *object = member->object;
  auto *conversion = dyn_cast<CastExpr>(object);
  if (conversion != nullptr && conversion->isImplicit()) {
    object = conversion->operand;
  }
  llvm::StringRef variable;
  auto *name = dyn_cast<NameExpr>(object);
  if (name != nullptr && name->var() != nullptr) {
    variable = name->name;
  }
  auto message = ErrorTextJsonMethod::none(member->name, variable);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *JsonChecker::checkJsonObjectLit(ObjectLitExpr *literal) {
  const auto *json = Type::json();
  for (ObjectLitExpr::Property &property : literal->props) {
    this->walkJsonProperty(property, json);
  }
  literal->type = json;

  return literal;
}

void JsonChecker::walkJsonProperty(ObjectLitExpr::Property &property, const Type *json) {
  if (property.spread() || property.method()) {
    std::string message =
        property.spread() ? ErrorTextJsonLiteral::spread() : ErrorTextJsonLiteral::function();
    this->state.getDiagnostics().error(property.loc, message);
    property.value = this->exprChecker->checkExpr(property.value);
    return;
  }
  if (property.defaulted()) {
    auto message = ErrorTextJsonLiteral::defaulted(property.name);
    this->state.getDiagnostics().error(property.loc, message);
    auto *assign = cast<AssignExpr>(property.value);
    property.value = this->exprChecker->checkExpr(assign->value);
    property.form = ValueProperty{};
    return;
  }
  auto *computed = property.form.as<ComputedProperty>();
  if (computed != nullptr) {
    this->walkJsonKey(property, computed);
  }
  auto *value = this->exprChecker->checkExpr(property.value, json);
  const Type *type = value->type;
  auto label = ErrorTextJsonKey::label(property.name);
  const Type *target = type->isOptional() || type->isUndefined() ? Type::optional(json) : json;
  property.value = this->typeRelations->coerce(value, target, label);
}

void JsonChecker::walkJsonKey(ObjectLitExpr::Property &property, ComputedProperty *computed) {
  computed->key = this->exprChecker->checkExpr(computed->key);
  const Type *keyType = property.key()->type;
  if (!keyType->isString() && !keyType->isError()) {
    auto message = ErrorTextJsonKey::literal(keyType->name());
    this->state.getDiagnostics().error(property.key()->loc(), message);
  }
}

Expr *JsonChecker::checkJsonArrayLit(ArrayLitExpr *literal) {
  auto *array = this->arrayLiteralChecker->checkArrayLit(literal, Type::array(Type::json()));
  if (array->type->isError()) {
    return array;
  }

  return this->typeRelations->coerce(array, Type::json());
}

Expr *JsonChecker::checkNullEquality(BinaryExpr *binary) {
  bool isLhsNull = this->isNullName(binary->lhs);
  bool isRhsNull = this->isNullName(binary->rhs);
  if (!isLhsNull && !isRhsNull) {
    return nullptr;
  }
  binary->type = Type::boolTy();
  Expr *&null = isLhsNull ? binary->lhs : binary->rhs;
  Expr *&other = isLhsNull ? binary->rhs : binary->lhs;
  const auto *type = Type::undefinedTy();
  if (!(isLhsNull && isRhsNull)) {
    other = this->exprChecker->checkExpr(other);
    type = other->type;
  }
  null->type = Type::json();
  auto possible = this->checkNullPossible(type, Operators::opSpelling(binary->op));
  if (possible.isOk()) {
    return binary;
  }
  this->state.getDiagnostics().error(null->loc(), possible.error().message);
  null->type = Type::error();

  return binary;
}

Expr *JsonChecker::checkJsonEquality(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isLhsJson = leftType->nonOptional()->isJson();
  bool isRhsJson = rightType->nonOptional()->isJson();
  bool isOptional = leftType->isOptional() || rightType->isOptional();
  const Type *json = isOptional ? Type::optional(Type::json()) : Type::json();
  if (isLhsJson && isRhsJson) {
    binary->lhs = this->typeRelations->convert(binary->lhs, json);
    binary->rhs = this->typeRelations->convert(binary->rhs, json);
    return binary;
  }
  Expr *&other = isLhsJson ? binary->rhs : binary->lhs;
  const Type *type = other->type;
  if (type->isString() || type->isBool()) {
    return binary;
  }
  if (type->isNumeric()) {
    other = this->typeRelations->convert(other, Type::doubleTy());
    return binary;
  }
  if (type->isOptional() && TypeRelations::isImplicitlyConvertible(type->inner(), Type::json())) {
    binary->lhs = this->typeRelations->convert(binary->lhs, json);
    binary->rhs = this->typeRelations->convert(binary->rhs, json);
    return binary;
  }
  const char *spelling = Operators::opSpelling(binary->op);
  auto message = this->binaryChecker->operandTypesText(spelling, leftType, rightType);
  this->state.getDiagnostics().error(binary->loc(), message);

  return binary;
}

std::optional<FieldPath> JsonChecker::jsonField(const Expr *expr) const {
  const auto *conversion = dyn_cast<CastExpr>(expr);
  bool isImplicit = conversion != nullptr && conversion->isImplicit();
  const Expr *field = isImplicit ? conversion->operand : expr;
  const auto *member = dyn_cast<MemberExpr>(field);
  if (member == nullptr || member->object->type == nullptr ||
      !member->object->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return this->fieldFacts->fieldPath(member);
}

void JsonChecker::narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) const {
  auto isSamePath = std::bind_front(&JsonChecker::isNarrowedPath, this, std::cref(path));
  llvm::erase_if(flow.narrowedFields, isSamePath);
  flow.narrowedFields.push_back({path, type});
  this->fieldFacts->defineField(flow, path);
}

bool JsonChecker::isNarrowedPath(const FieldPath &path,
                                 const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first == path;
}

void JsonChecker::addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  const auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    this->addArrayFacts(call->args[0], whenTrue);
    return;
  }
  const auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary == nullptr || (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne)) {
    return;
  }
  Flow &equal = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = binary->op == BinaryOp::Eq ? whenFalse : whenTrue;
  this->addSideFacts(binary->lhs, binary->rhs, equal, different);
  this->addSideFacts(binary->rhs, binary->lhs, equal, different);
}

void JsonChecker::addSideFacts(const Expr *side, const Expr *other, Flow &equal,
                               Flow &different) const {
  if (this->isNothing(other)) {
    return this->addNothingFacts(side, other, equal, different);
  }
  const auto *typeOf = dyn_cast<UnaryExpr>(side);
  const auto *text = dyn_cast<StringLitExpr>(other);
  if (typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && text != nullptr) {
    this->addTypeOfFacts(typeOf->operand, text->value, equal, different);
  }
}

void JsonChecker::addArrayFacts(const Expr *arg, Flow &whenTrue) const {
  const auto *variable = this->jsonVariable(arg);
  if (variable != nullptr) {
    whenTrue.narrowed.push_back({variable, Type::array(Type::json())});
    if (variable->type->isOptional()) {
      whenTrue.defined.insert(variable);
    }
    return;
  }
  auto path = this->jsonField(arg);
  if (path.has_value()) {
    this->narrowJsonField(whenTrue, *path, Type::array(Type::json()));
  }
}

void JsonChecker::addNothingFacts(const Expr *side, const Expr *other, Flow &equal,
                                  Flow &different) const {
  bool isNull = this->isNullName(other);
  Flow &defined = isNull ? equal : different;
  const auto *variable = this->jsonVariable(side);
  if (variable != nullptr && variable->type->isOptional()) {
    defined.defined.insert(variable);
    return;
  }
  auto path = this->jsonField(side);
  if (path.has_value()) {
    this->fieldFacts->defineField(defined, *path);
  }
}

void JsonChecker::addTypeOfFacts(const Expr *operand, const std::u16string &text, Flow &equal,
                                 Flow &different) const {
  const auto *narrowed = this->typeOfJson(text);
  auto typeOf = TypeOfNameTable::find(ToUtf8::toUtf8(text));
  bool isPresent =
      narrowed != nullptr || typeOf == TypeOfName::OBJECT || typeOf == TypeOfName::NULL_TYPE;
  const auto *variable = this->jsonVariable(operand);
  if (variable == nullptr) {
    auto path = this->jsonField(operand);
    if (!path.has_value()) {
      return;
    }
    if (narrowed != nullptr) {
      this->narrowJsonField(equal, *path, narrowed);
    } else if (isPresent) {
      this->fieldFacts->defineField(equal, *path);
    } else if (typeOf == TypeOfName::UNDEFINED_TYPE) {
      this->fieldFacts->defineField(different, *path);
    }
    return;
  }
  if (narrowed != nullptr) {
    equal.narrowed.push_back({variable, narrowed});
  }
  if (!variable->type->isOptional()) {
    return;
  }
  if (isPresent) {
    equal.defined.insert(variable);
  } else if (typeOf == TypeOfName::UNDEFINED_TYPE) {
    different.defined.insert(variable);
  }
}

const Type *JsonChecker::typeOfJson(const std::u16string &text) const {
  auto typeOf = TypeOfNameTable::find(ToUtf8::toUtf8(text));
  if (typeOf == TypeOfName::STRING) {
    return Type::string();
  }
  if (typeOf == TypeOfName::DOUBLE) {
    return Type::doubleTy();
  }
  if (typeOf == TypeOfName::BOOLEAN) {
    return Type::boolTy();
  }
  if (typeOf == TypeOfName::ARRAY) {
    return Type::array(Type::json());
  }

  return nullptr;
}

Expr *JsonChecker::checkJsonIsJson(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    return call;
  }
  auto tests = this->checkTestsUnknown(type);
  if (tests.isErr()) {
    this->state.getDiagnostics().error(call->args[0]->loc(), tests.error().message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::JsonIsJson};
  call->type = Type::boolTy();

  return call;
}

Expr *JsonChecker::checkArrayIsArray(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  if (call->args[0]->type->isVoid()) {
    auto message = ErrorTextTruthy::voidValue();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::ArrayIsArray};
  call->type = Type::boolTy();

  return call;
}

}
