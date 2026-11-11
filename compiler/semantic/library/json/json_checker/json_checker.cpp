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
  this->argumentChecker = &all.argumentChecker;
  this->arrayLiteralChecker = &all.arrayLiteralChecker;
}

const VarSymbol *JsonChecker::jsonVariable(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var() && name->var()->type->nonOptional()->isJson() ? name->var() : nullptr;
}

bool JsonChecker::nullName(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->name == "null" && !name->var() && !name->func();
}

bool JsonChecker::isNothing(const Expr *expr) {
  return JsonChecker::nullName(expr) || (expr->type && expr->type->isUndefined());
}

const Expr *JsonChecker::bare(const Expr *expr) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool JsonChecker::sameRef(const Expr *a, const Expr *b) {
  const Expr *left = JsonChecker::bare(a);
  const Expr *right = JsonChecker::bare(b);
  auto *leftMember = dyn_cast_or_null<MemberExpr>(left);
  auto *rightMember = dyn_cast_or_null<MemberExpr>(right);
  while (leftMember != nullptr && rightMember != nullptr) {
    if (leftMember->name != rightMember->name) {
      return false;
    }
    left = JsonChecker::bare(leftMember->object);
    right = JsonChecker::bare(rightMember->object);
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

std::string JsonChecker::refText(const Expr *expr) {
  std::vector<llvm::StringRef> names;
  const Expr *link = JsonChecker::bare(expr);
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    names.push_back(member->name);
    link = JsonChecker::bare(member->object);
  }
  auto *root = dyn_cast<NameExpr>(link);
  std::string text = root != nullptr ? root->name.str() : "x";
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

void JsonChecker::walkRedundantTests(const BinaryExpr *bin) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  std::vector<const Expr *> terms = ExprParts::sameOperatorParts(bin->lhs, bin->op);
  const Expr *tested = nullptr;
  bool isNullTest = false;
  const Expr *rhs = JsonChecker::bare(bin->rhs);
  if (auto *eq = dyn_cast<BinaryExpr>(rhs); eq &&
                                            (eq->op == BinaryOp::Eq || eq->op == BinaryOp::Ne) &&
                                            (JsonChecker::nullName(JsonChecker::bare(eq->lhs)) ||
                                             JsonChecker::nullName(JsonChecker::bare(eq->rhs)))) {
    tested = JsonChecker::nullName(JsonChecker::bare(eq->lhs)) ? eq->rhs : eq->lhs;
    isNullTest = true;
  } else {
    const Expr *inner = rhs;
    if (auto *unary = dyn_cast<UnaryExpr>(rhs); unary && unary->op == UnaryOp::Not) {
      inner = JsonChecker::bare(unary->operand);
    }
    if (auto *call = dyn_cast<CallExpr>(inner); call && call->builtin() == Builtin::ArrayIsArray) {
      tested = call->args[0];
    }
  }
  if (!tested || !JsonChecker::bare(tested)->type ||
      !JsonChecker::bare(tested)->type->nonOptional()->isJson()) {
    return;
  }
  for (const Expr *term : terms) {
    auto *cmp = dyn_cast<BinaryExpr>(JsonChecker::bare(term));
    if (!cmp || cmp->op != (isAnd ? BinaryOp::Eq : BinaryOp::Ne)) {
      continue;
    }
    auto *typeOf = dyn_cast<UnaryExpr>(JsonChecker::bare(cmp->lhs));
    auto *text = dyn_cast<StringLitExpr>(JsonChecker::bare(cmp->rhs));
    if (!typeOf || !text) {
      typeOf = dyn_cast<UnaryExpr>(JsonChecker::bare(cmp->rhs));
      text = dyn_cast<StringLitExpr>(JsonChecker::bare(cmp->lhs));
    }
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text ||
        !JsonChecker::sameRef(typeOf->operand, tested)) {
      continue;
    }
    std::string name = ToUtf8::toUtf8(text->value);
    ResultTypeOfAgrees agrees = JsonChecker::typeOfAgrees(tested, name, isNullTest, isAnd);
    if (agrees.isErr()) {
      this->state.getDiagnostics().error(rhs->loc(), agrees.error().message);
      return;
    }
  }
}

JsonChecker::ResultTypeOfAgrees JsonChecker::typeOfAgrees(const Expr *tested, llvm::StringRef name,
                                                          bool nullTest, bool isAnd) {
  if (nullTest && name != "null" && name != "undefined") {
    std::string message = ErrorTextJsonTest::neverNull(JsonChecker::refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_NULL,
        .message = message,
    });
  }
  if (!nullTest && name != "array") {
    std::string message = ErrorTextJsonTest::neverArray(JsonChecker::refText(tested), isAnd, name);

    return err(JsonCheckerTypeOfAgreesError{
        .code = JsonCheckerTypeOfAgreesErrorCode::NEVER_ARRAY,
        .message = message,
    });
  }

  return ok(tested);
}

JsonChecker::ResultNullPossible JsonChecker::nullPossible(const Type *type, llvm::StringRef op) {
  if (!type->isError() && !type->nonOptional()->isJson()) {
    std::string message = ErrorTextJsonNull::never(type->name(), op);

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
    std::string message = ErrorTextJsonIsJson::known(type->name(), type->nonOptional()->isJson());

    return err(JsonCheckerTestsUnknownError{
        .code = JsonCheckerTestsUnknownErrorCode::KNOWN_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

bool JsonChecker::holdsJson(const Type *type) {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }

  return type->isJson();
}

bool JsonChecker::isNullName(const Expr *expr) {
  return JsonChecker::nullName(expr);
}

Expr *JsonChecker::checkNull(NameExpr *name, const Type *expected) {
  if (!JsonChecker::isNullName(name) || !expected || !expected->nonOptional()->isJson()) {
    return nullptr;
  }
  name->type = Type::json();

  return name;
}

Expr *JsonChecker::checkJsonMember(MemberExpr *member) {
  member->type = Type::optional(Type::json());
  if (Expr *narrowed = this->fieldFacts->narrowedField(member); narrowed != member) {
    return narrowed;
  }
  if (std::optional<FieldPath> path = this->fieldFacts->fieldPath(member);
      path && this->fieldFacts->isFieldDefined(*path)) {
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
  std::string message = ErrorTextJsonKey::index(key->name());
  this->state.getDiagnostics().error(index->index->loc(), message);
  index->type = Type::error();

  return index;
}

const Type *JsonChecker::checkJsonTarget(Expr *target) {
  if (auto *index = dyn_cast<IndexExpr>(target)) {
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
  if (auto *conv = dyn_cast<CastExpr>(object); conv && conv->isImplicit()) {
    object = conv->operand;
  }
  llvm::StringRef variable;
  if (auto *name = dyn_cast<NameExpr>(object); name && name->var()) {
    variable = name->name;
  }
  std::string message = ErrorTextJsonMethod::none(member->name, variable);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *JsonChecker::checkJsonObjectLit(ObjectLitExpr *lit) {
  const Type *json = Type::json();
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread() || prop.method()) {
      std::string message =
          prop.spread() ? ErrorTextJsonLiteral::spread() : ErrorTextJsonLiteral::function();
      this->state.getDiagnostics().error(prop.loc, message);
      prop.value = this->exprChecker->checkExpr(prop.value);
      continue;
    }
    if (prop.defaulted()) {
      std::string message = ErrorTextJsonLiteral::defaulted(prop.name);
      this->state.getDiagnostics().error(prop.loc, message);
      prop.value = this->exprChecker->checkExpr(cast<AssignExpr>(prop.value)->value);
      prop.form = ValueProperty{};
      continue;
    }
    if (ComputedProperty *computed = prop.form.as<ComputedProperty>()) {
      computed->key = this->exprChecker->checkExpr(computed->key);
      if (!prop.key()->type->isString() && !prop.key()->type->isError()) {
        std::string message = ErrorTextJsonKey::literal(prop.key()->type->name());
        this->state.getDiagnostics().error(prop.key()->loc(), message);
      }
    }
    Expr *value = this->exprChecker->checkExpr(prop.value, json);
    const Type *type = value->type;
    std::string label = ErrorTextJsonKey::label(prop.name);
    prop.value = this->typeRelations->coerce(
        value, type->isOptional() || type->isUndefined() ? Type::optional(json) : json, label);
  }
  lit->type = json;

  return lit;
}

Expr *JsonChecker::checkJsonArrayLit(ArrayLitExpr *lit) {
  Expr *array = this->arrayLiteralChecker->checkArrayLit(lit, Type::array(Type::json()));
  if (array->type->isError()) {
    return array;
  }

  return this->typeRelations->coerce(array, Type::json());
}

Expr *JsonChecker::checkNullEquality(BinaryExpr *bin) {
  bool isLhsNull = JsonChecker::isNullName(bin->lhs);
  bool isRhsNull = JsonChecker::isNullName(bin->rhs);
  if (!isLhsNull && !isRhsNull) {
    return nullptr;
  }
  bin->type = Type::boolTy();
  Expr *&null = isLhsNull ? bin->lhs : bin->rhs;
  Expr *&other = isLhsNull ? bin->rhs : bin->lhs;
  const Type *type = Type::undefinedTy();
  if (!(isLhsNull && isRhsNull)) {
    other = this->exprChecker->checkExpr(other);
    type = other->type;
  }
  null->type = Type::json();
  ResultNullPossible possible = JsonChecker::nullPossible(type, Operators::opSpelling(bin->op));
  if (possible.isOk()) {
    return bin;
  }
  this->state.getDiagnostics().error(null->loc(), possible.error().message);
  null->type = Type::error();

  return bin;
}

Expr *JsonChecker::checkJsonEquality(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bool isLhsJson = l->nonOptional()->isJson();
  bool isRhsJson = r->nonOptional()->isJson();
  const Type *json =
      l->isOptional() || r->isOptional() ? Type::optional(Type::json()) : Type::json();
  if (isLhsJson && isRhsJson) {
    bin->lhs = this->typeRelations->convert(bin->lhs, json);
    bin->rhs = this->typeRelations->convert(bin->rhs, json);
    return bin;
  }
  Expr *&other = isLhsJson ? bin->rhs : bin->lhs;
  const Type *type = other->type;
  if (type->isString() || type->isBool()) {
    return bin;
  }
  if (type->isNumeric()) {
    other = this->typeRelations->convert(other, Type::doubleTy());
    return bin;
  }
  if (type->isOptional() && TypeRelations::isImplicitlyConvertible(type->inner(), Type::json())) {
    bin->lhs = this->typeRelations->convert(bin->lhs, json);
    bin->rhs = this->typeRelations->convert(bin->rhs, json);
    return bin;
  }
  std::string message = BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
  this->state.getDiagnostics().error(bin->loc(), message);

  return bin;
}

std::optional<FieldPath> JsonChecker::jsonField(const Expr *expr) const {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    expr = conv->operand;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || !member->object->type || !member->object->type->nonOptional()->isJson()) {
    return std::nullopt;
  }

  return this->fieldFacts->fieldPath(member);
}

void JsonChecker::narrowJsonField(Flow &flow, const FieldPath &path, const Type *type) {
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first == path; });
  flow.narrowedFields.push_back({path, type});
  FieldFacts::defineField(flow, path);
}

void JsonChecker::addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    this->addArrayFacts(call->args[0], whenTrue);
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (bin == nullptr || (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne)) {
    return;
  }
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    if (JsonChecker::isNothing(other)) {
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
  const VarSymbol *var = JsonChecker::jsonVariable(arg);
  if (var != nullptr) {
    whenTrue.narrowed.push_back({var, Type::array(Type::json())});
    if (var->type->isOptional()) {
      whenTrue.defined.insert(var);
    }
    return;
  }
  std::optional<FieldPath> path = this->jsonField(arg);
  if (path.has_value()) {
    JsonChecker::narrowJsonField(whenTrue, *path, Type::array(Type::json()));
  }
}

void JsonChecker::addNothingFacts(const Expr *side, const Expr *other, Flow &equal,
                                  Flow &different) const {
  bool isNull = JsonChecker::nullName(other);
  Flow &defined = isNull ? equal : different;
  const VarSymbol *var = JsonChecker::jsonVariable(side);
  if (var != nullptr && var->type->isOptional()) {
    defined.defined.insert(var);
    return;
  }
  std::optional<FieldPath> path = this->jsonField(side);
  if (path.has_value()) {
    FieldFacts::defineField(defined, *path);
  }
}

void JsonChecker::addTypeOfFacts(const Expr *operand, const std::u16string &text, Flow &equal,
                                 Flow &different) const {
  const Type *narrowed = JsonChecker::typeOfJson(text);
  bool isPresent = narrowed != nullptr || text == u"object" || text == u"null";
  const VarSymbol *var = JsonChecker::jsonVariable(operand);
  if (var == nullptr) {
    std::optional<FieldPath> path = this->jsonField(operand);
    if (!path.has_value()) {
      return;
    }
    if (narrowed != nullptr) {
      JsonChecker::narrowJsonField(equal, *path, narrowed);
    } else if (isPresent) {
      FieldFacts::defineField(equal, *path);
    } else if (text == u"undefined") {
      FieldFacts::defineField(different, *path);
    }
    return;
  }
  if (narrowed != nullptr) {
    equal.narrowed.push_back({var, narrowed});
  }
  if (!var->type->isOptional()) {
    return;
  }
  if (isPresent) {
    equal.defined.insert(var);
  } else if (text == u"undefined") {
    different.defined.insert(var);
  }
}

const Type *JsonChecker::typeOfJson(const std::u16string &text) {
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
  ArgumentChecker::ResultArityMatches arity =
      isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
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
  ResultTestsUnknown tests = this->testsUnknown(type);
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
  ArgumentChecker::ResultArityMatches arity =
      isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  if (call->args[0]->type->isVoid()) {
    std::string message = ErrorTextTruthy::voidValue();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::ArrayIsArray};
  call->type = Type::boolTy();

  return call;
}

}
