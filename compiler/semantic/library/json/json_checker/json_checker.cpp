#include "compiler/semantic/library/json/json_checker/json_checker.h"
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

bool JsonChecker::nullName(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);

  return name != nullptr && name->name == "null" && name->var() == nullptr &&
         name->func() == nullptr;
}

bool JsonChecker::isNothing(const Expr *expr) const {
  return this->nullName(expr) || (expr->type != nullptr && expr->type->isUndefined());
}

const Expr *JsonChecker::bare(const Expr *expr) const {
  while (auto *conversion = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conversion->isImplicit()) {
      break;
    }
    expr = conversion->operand;
  }

  return expr;
}

bool JsonChecker::sameRef(const Expr *a, const Expr *b) const {
  const auto *left = this->bare(a);
  const auto *right = this->bare(b);
  auto *leftMember = dyn_cast_or_null<MemberExpr>(left);
  auto *rightMember = dyn_cast_or_null<MemberExpr>(right);
  while (leftMember != nullptr && rightMember != nullptr) {
    if (leftMember->name != rightMember->name) {
      return false;
    }
    left = this->bare(leftMember->object);
    right = this->bare(rightMember->object);
    leftMember = dyn_cast_or_null<MemberExpr>(left);
    rightMember = dyn_cast_or_null<MemberExpr>(right);
  }
  auto *leftName = dyn_cast_or_null<NameExpr>(left);
  auto *rightName = dyn_cast_or_null<NameExpr>(right);
  if (leftName == nullptr || rightName == nullptr) {
    return false;
  }

  return leftName->var() != nullptr && leftName->var() == rightName->var();
}

std::string JsonChecker::refText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = this->bare(expr);
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    names.push_back(member->name);
    link = this->bare(member->object);
  }
  auto *root = dyn_cast<NameExpr>(link);
  std::string text = root != nullptr ? root->name.str() : "x";
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

void JsonChecker::walkRedundantTests(const BinaryExpr *binary) {
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto terms = ExprParts::sameOperatorParts(binary->lhs, binary->op);
  const Expr *tested = nullptr;
  bool isNullTest = false;
  const auto *rhs = this->bare(binary->rhs);
  auto *eq = dyn_cast<BinaryExpr>(rhs);
  if (eq != nullptr && (eq->op == BinaryOp::Eq || eq->op == BinaryOp::Ne) &&
      (this->nullName(this->bare(eq->lhs)) || this->nullName(this->bare(eq->rhs)))) {
    tested = this->nullName(this->bare(eq->lhs)) ? eq->rhs : eq->lhs;
    isNullTest = true;
  } else {
    const Expr *inner = rhs;
    auto *unary = dyn_cast<UnaryExpr>(rhs);
    if (unary != nullptr && unary->op == UnaryOp::Not) {
      inner = this->bare(unary->operand);
    }
    auto *call = dyn_cast<CallExpr>(inner);
    if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
      tested = call->args[0];
    }
  }
  if (tested == nullptr || this->bare(tested)->type == nullptr ||
      !this->bare(tested)->type->nonOptional()->isJson()) {
    return;
  }
  for (const Expr *term : terms) {
    auto *cmp = dyn_cast<BinaryExpr>(this->bare(term));
    if (cmp == nullptr || cmp->op != (isAnd ? BinaryOp::Eq : BinaryOp::Ne)) {
      continue;
    }
    auto *typeOf = dyn_cast<UnaryExpr>(this->bare(cmp->lhs));
    auto *text = dyn_cast<StringLitExpr>(this->bare(cmp->rhs));
    if (typeOf == nullptr || text == nullptr) {
      typeOf = dyn_cast<UnaryExpr>(this->bare(cmp->rhs));
      text = dyn_cast<StringLitExpr>(this->bare(cmp->lhs));
    }
    if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr ||
        !this->sameRef(typeOf->operand, tested)) {
      continue;
    }
    auto name = ToUtf8::toUtf8(text->value);
    auto agrees = this->typeOfAgrees(tested, name, isNullTest, isAnd);
    if (agrees.isErr()) {
      this->state.getDiagnostics().error(rhs->loc(), agrees.error().message);
      return;
    }
  }
}

JsonChecker::ResultTypeOfAgrees JsonChecker::typeOfAgrees(const Expr *tested, llvm::StringRef name,
                                                          bool nullTest, bool isAnd) const {
  if (nullTest && name != "null" && name != "undefined") {
    auto message = ErrorTextJsonTest::neverNull(this->refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_NULL,
        .message = message,
    });
  }
  if (!nullTest && name != "array") {
    auto message = ErrorTextJsonTest::neverArray(this->refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_ARRAY,
        .message = message,
    });
  }

  return ok(tested);
}

JsonChecker::ResultNullPossible JsonChecker::nullPossible(const Type *type,
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

JsonChecker::ResultTestsUnknown JsonChecker::testsUnknown(const Type *type) {
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

bool JsonChecker::holdsJson(const Type *type) const {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }

  return type->isJson();
}

bool JsonChecker::isNullName(const Expr *expr) const {
  return this->nullName(expr);
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
    if (property.spread() || property.method()) {
      std::string message =
          property.spread() ? ErrorTextJsonLiteral::spread() : ErrorTextJsonLiteral::function();
      this->state.getDiagnostics().error(property.loc, message);
      property.value = this->exprChecker->checkExpr(property.value);
      continue;
    }
    if (property.defaulted()) {
      auto message = ErrorTextJsonLiteral::defaulted(property.name);
      this->state.getDiagnostics().error(property.loc, message);
      property.value = this->exprChecker->checkExpr(cast<AssignExpr>(property.value)->value);
      property.form = ValueProperty{};
      continue;
    }
    ComputedProperty *computed = property.form.as<ComputedProperty>();
    if (computed != nullptr) {
      computed->key = this->exprChecker->checkExpr(computed->key);
      if (!property.key()->type->isString() && !property.key()->type->isError()) {
        auto message = ErrorTextJsonKey::literal(property.key()->type->name());
        this->state.getDiagnostics().error(property.key()->loc(), message);
      }
    }
    auto *value = this->exprChecker->checkExpr(property.value, json);
    const Type *type = value->type;
    auto label = ErrorTextJsonKey::label(property.name);
    property.value = this->typeRelations->coerce(
        value, type->isOptional() || type->isUndefined() ? Type::optional(json) : json, label);
  }
  literal->type = json;

  return literal;
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
  auto possible = this->nullPossible(type, Operators::opSpelling(binary->op));
  if (possible.isOk()) {
    return binary;
  }
  this->state.getDiagnostics().error(null->loc(), possible.error().message);
  null->type = Type::error();

  return binary;
}

Expr *JsonChecker::checkJsonEquality(BinaryExpr *binary) {
  const Type *l = binary->lhs->type;
  const Type *r = binary->rhs->type;
  bool isLhsJson = l->nonOptional()->isJson();
  bool isRhsJson = r->nonOptional()->isJson();
  const Type *json =
      l->isOptional() || r->isOptional() ? Type::optional(Type::json()) : Type::json();
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
  auto message = this->binaryChecker->operandTypesText(Operators::opSpelling(binary->op), l, r);
  this->state.getDiagnostics().error(binary->loc(), message);

  return binary;
}

std::optional<FieldPath> JsonChecker::jsonField(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || member->object->type == nullptr ||
      !member->object->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return this->fieldFacts->fieldPath(member);
}

void JsonChecker::narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) const {
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first == path; });
  flow.narrowedFields.push_back({path, type});
  this->fieldFacts->defineField(flow, path);
}

void JsonChecker::addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    this->addArrayFacts(call->args[0], whenTrue);
    return;
  }
  auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary == nullptr || (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne)) {
    return;
  }
  Flow &equal = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = binary->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] :
       {std::pair{binary->lhs, binary->rhs}, std::pair{binary->rhs, binary->lhs}}) {
    if (this->isNothing(other)) {
      this->addNothingFacts(side, other, equal, different);
      continue;
    }
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && text != nullptr) {
      this->addTypeOfFacts(typeOf->operand, text->value, equal, different);
    }
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
  bool isNull = this->nullName(other);
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
  bool isPresent = narrowed != nullptr || text == u"object" || text == u"null";
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
    } else if (text == u"undefined") {
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
  } else if (text == u"undefined") {
    different.defined.insert(variable);
  }
}

const Type *JsonChecker::typeOfJson(const std::u16string &text) const {
  if (text == u"string") {
    return Type::string();
  }
  if (text == u"double") {
    return Type::doubleTy();
  }
  if (text == u"boolean") {
    return Type::boolTy();
  }
  if (text == u"array") {
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
  auto tests = this->testsUnknown(type);
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
