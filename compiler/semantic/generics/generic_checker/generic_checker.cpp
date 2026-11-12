#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char16_t *kUndefinedTypeOf = u"undefined";

}

GenericChecker::GenericChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->concatChecker = &all.concatChecker;
  this->typeRelations = &all.typeRelations;
}

bool GenericChecker::isBoundedParam(const Type *type) const {
  return type->isParam() && type->constraint() != nullptr && !type->isListParam();
}

bool GenericChecker::hasOneLiteralBase(const Type *type) const {
  const Type *first = type->members().front();
  if (!first->isLiteral()) {
    return false;
  }
  const Type *base = first->literalBase();
  for (const Type *member : type->members()) {
    if (!member->isLiteral() || member->literalBase() != base) {
      return false;
    }
  }

  return true;
}

bool GenericChecker::hasBoundedMember(const Type *type) const {
  for (const Type *member : type->members()) {
    if (this->isBoundedParam(member)) {
      return true;
    }
  }

  return false;
}

const Type *GenericChecker::apparentType(const Type *type) const {
  if (this->isBoundedParam(type)) {
    return type->constraint();
  }
  if (type->isLiteral()) {
    return type->literalBase();
  }
  if (type->isUnion() && this->hasOneLiteralBase(type)) {
    return type->members().front()->literalBase();
  }
  if (type->isUnion() && this->hasBoundedMember(type)) {
    return this->apparentUnion(type);
  }

  return type;
}

const Type *GenericChecker::apparentUnion(const Type *type) const {
  auto members = std::vector<const Type *>();
  for (const Type *member : type->members()) {
    const Type *seen = this->isBoundedParam(member) ? member->constraint() : member;
    if (!seen->isObject() || seen->isClass()) {
      return type;
    }
    members.push_back(seen);
  }
  const auto *joined = Type::unionOf(members);

  return joined->isTagged() ? type : joined;
}

Expr *GenericChecker::apparent(Expr *expr) {
  const auto *type = this->apparentType(expr->type);
  if (type == expr->type) {
    return expr;
  }

  return this->typeRelations->convert(expr, type);
}

FuncSymbol *GenericChecker::genericNamed(const NameExpr *name) const {
  auto entry = this->scopes->lookup(name->name);
  FuncSymbol *function = entry.func();
  if (function == nullptr || entry.var() != nullptr || !function->isGeneric()) {
    return nullptr;
  }

  return function;
}

const Type *GenericChecker::numberParam(const Type *param, const Type *only) const {
  const auto *known = this->state.getGenericStore().findNumberParam(param, only);
  if (known != nullptr) {
    return known;
  }
  const Type *constraint = only != nullptr ? only : this->commonNumericAlternative(param);
  auto *type = Type::param(param->name());
  type->setConstraint(constraint != nullptr ? constraint : Type::doubleTy(), {});
  this->state.getGenericStore().rememberNumberParam(param, only, type);

  return type;
}

const Type *GenericChecker::commonNumericAlternative(const Type *param) const {
  const Type *common = nullptr;
  for (const Type *candidate : param->alternatives()) {
    if (candidate->isNumeric()) {
      common = common != nullptr ? TypeRelations::commonNumericType(common, candidate) : candidate;
    }
  }

  return common;
}

const Type *GenericChecker::narrowedType(const VarSymbol *variable) const {
  return this->state.getFlowState().findNarrowed(variable);
}

void GenericChecker::noteForced(const Expr *expr, const Type *to) {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  this->state.getGenericStore().rememberForcedType(bare, to);
}

Expr *GenericChecker::forceConversion(Expr *expr, const Type *to) {
  const Type *type = expr->type;
  if (type == nullptr || type->isError() || type == to) {
    return expr;
  }
  if (to->isString()) {
    return this->concatChecker->toStringOperand(expr);
  }
  if (TypeRelations::isImplicitlyConvertible(type, to)) {
    return this->typeRelations->convert(expr, to);
  }

  return expr;
}

std::optional<bool> GenericChecker::instanceConstant(const Expr *cond) const {
  if (!this->state.getGenericStore().isCheckingInstance()) {
    return std::nullopt;
  }

  return this->constantCondition(cond);
}

std::optional<bool> GenericChecker::constantCondition(const Expr *cond) const {
  auto answers = std::vector<std::optional<bool>>();
  for (const Expr *node : ChildrenFirst::order(cond, this, &GenericChecker::conditionParts)) {
    auto value = this->nodeCondition(node, answers);
    answers.push_back(value);
  }

  return answers.back();
}

NodeChildren<const Expr *> GenericChecker::conditionParts(const Expr *cond) const {
  auto *conversion = dyn_cast<CastExpr>(cond);
  if (conversion != nullptr && this->isBoolPass(conversion)) {
    return {conversion->operand};
  }
  auto *unary = dyn_cast<UnaryExpr>(cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    return {unary->operand};
  }
  auto *binary = dyn_cast<BinaryExpr>(cond);
  bool isLogic =
      binary != nullptr && (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr);
  if (isLogic) {
    return {binary->lhs, binary->rhs};
  }

  return {};
}

bool GenericChecker::isBoolPass(const CastExpr *conversion) const {
  return conversion->isImplicit() && conversion->type->isBool() &&
         conversion->operand->type->isBool();
}

std::optional<bool> GenericChecker::nodeCondition(const Expr *cond,
                                                  std::vector<std::optional<bool>> &answers) const {
  auto *conversion = dyn_cast<CastExpr>(cond);
  if (conversion != nullptr && this->isBoolPass(conversion)) {
    return ChildrenFirst::takeAnswer(answers);
  }
  auto *literal = dyn_cast<BoolLitExpr>(cond);
  if (literal != nullptr) {
    return literal->value;
  }
  auto *unary = dyn_cast<UnaryExpr>(cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    std::optional<bool> value = ChildrenFirst::takeAnswer(answers);
    return value.has_value() ? std::optional<bool>(!*value) : std::nullopt;
  }
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::JsonIsJson) {
    return this->isJsonCondition(call);
  }
  auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary == nullptr) {
    return std::nullopt;
  }
  if (binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr) {
    return this->logicalCondition(binary, answers);
  }
  if (binary->op != BinaryOp::Eq && binary->op != BinaryOp::Ne) {
    return std::nullopt;
  }
  auto undefinedCheck = this->undefinedCondition(binary);
  if (undefinedCheck.has_value()) {
    return undefinedCheck;
  }

  return this->typeOfCondition(binary);
}

std::optional<bool> GenericChecker::isJsonCondition(const CallExpr *call) const {
  const Type *type = call->args[0]->type;
  if (type->isUnknown() || type->nonOptional()->isParam()) {
    return std::nullopt;
  }

  return type->nonOptional()->isJson();
}

std::optional<bool>
GenericChecker::logicalCondition(const BinaryExpr *binary,
                                 std::vector<std::optional<bool>> &answers) const {
  std::optional<bool> right = ChildrenFirst::takeAnswer(answers);
  std::optional<bool> left = ChildrenFirst::takeAnswer(answers);
  bool isAnd = binary->op == BinaryOp::LogAnd;
  if (left.has_value() && *left != isAnd) {
    return left;
  }
  if (left.has_value()) {
    return right;
  }

  return std::nullopt;
}

std::optional<bool> GenericChecker::undefinedCondition(const BinaryExpr *binary) const {
  auto leftSide = this->undefinedSide(binary, binary->lhs, binary->rhs);
  if (leftSide.has_value()) {
    return leftSide;
  }

  return this->undefinedSide(binary, binary->rhs, binary->lhs);
}

std::optional<bool> GenericChecker::undefinedSide(const BinaryExpr *binary, const Expr *side,
                                                  const Expr *other) const {
  const Expr *operand = ExprParts::withoutImplicitCasts(side);
  const Type *type = operand->type;
  bool isAlwaysUndefined = type->isUndefined() || (type->isOptional() && type->inner()->isNever());
  if (isa<UndefinedExpr>(other) && isa<NameExpr>(operand) && isAlwaysUndefined) {
    return binary->op == BinaryOp::Eq;
  }

  return std::nullopt;
}

bool GenericChecker::isTypeOfTest(const Expr *side, const Expr *other) const {
  auto *typeOf = dyn_cast<UnaryExpr>(side);

  return typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && isa<StringLitExpr>(other);
}

std::optional<bool> GenericChecker::typeOfCondition(const BinaryExpr *binary) const {
  if (this->isTypeOfTest(binary->lhs, binary->rhs)) {
    return this->typeOfSide(binary, binary->lhs, binary->rhs);
  }
  if (this->isTypeOfTest(binary->rhs, binary->lhs)) {
    return this->typeOfSide(binary, binary->rhs, binary->lhs);
  }

  return std::nullopt;
}

std::optional<bool> GenericChecker::typeOfSide(const BinaryExpr *binary, const Expr *side,
                                               const Expr *other) const {
  auto *typeOf = cast<UnaryExpr>(side);
  auto *text = cast<StringLitExpr>(other);
  const Expr *operand = ExprParts::withoutImplicitCasts(typeOf->operand);
  if (!isa<NameExpr>(operand)) {
    return std::nullopt;
  }

  return this->typeOfAnswer(binary, typeOf->operand->type, text);
}

std::optional<bool> GenericChecker::typeOfAnswer(const BinaryExpr *binary, const Type *type,
                                                 const StringLitExpr *text) const {
  llvm::StringRef name = TypeSpelling::typeOfName(type->nonOptional());
  if (name.empty()) {
    return std::nullopt;
  }
  std::u16string wanted(name.begin(), name.end());
  bool isEqual = text->value == wanted;
  bool isUndefinedText = text->value == kUndefinedTypeOf;
  if (type->isOptional() && (isUndefinedText || isEqual)) {
    return std::nullopt;
  }

  return binary->op == BinaryOp::Eq ? isEqual : !isEqual;
}

const Type *GenericChecker::literalShape(const Expr *expr) const {
  auto answers = std::vector<const Type *>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &GenericChecker::shapeParts)) {
    const auto *shape = this->nodeShape(node, answers);
    if (shape == nullptr) {
      return nullptr;
    }
    answers.push_back(shape);
  }

  return answers.back();
}

NodeChildren<const Expr *> GenericChecker::shapeParts(const Expr *expr) const {
  if (SharedHelpers::literalType(expr) != nullptr) {
    return {};
  }
  auto *array = dyn_cast<ArrayLitExpr>(expr);
  auto *object = dyn_cast<ObjectLitExpr>(expr);
  auto parts = NodeChildren<const Expr *>();
  if (array != nullptr && !this->hasGaps(array)) {
    for (const ArrayLitExpr::Element &element : array->elements) {
      parts.push_back(element.expr);
    }
  }
  if (object != nullptr) {
    for (const ObjectLitExpr::Property &property : object->props) {
      parts.push_back(property.value);
    }
  }

  return parts;
}

bool GenericChecker::hasGaps(const ArrayLitExpr *literal) const {
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread || element.hole) {
      return true;
    }
  }

  return false;
}

const Type *GenericChecker::nodeShape(const Expr *expr, std::vector<const Type *> &answers) const {
  const auto *type = SharedHelpers::literalType(expr);
  if (type != nullptr) {
    return isa<TemplateExpr>(expr) ? nullptr : type;
  }
  auto *array = dyn_cast<ArrayLitExpr>(expr);
  auto *object = dyn_cast<ObjectLitExpr>(expr);
  if (isa<UndefinedExpr>(expr)) {
    return Type::undefinedTy();
  }
  if (array != nullptr) {
    return this->arrayShape(array, answers);
  }
  if (object != nullptr) {
    return this->objectShape(object, answers);
  }

  return nullptr;
}

const Type *GenericChecker::arrayShape(const ArrayLitExpr *literal,
                                       std::vector<const Type *> &answers) const {
  if (this->hasGaps(literal)) {
    return nullptr;
  }
  std::vector<const Type *> types = ChildrenFirst::takeAnswers(answers, literal->elements.size());
  const Type *element = nullptr;
  for (const Type *type : types) {
    element = element != nullptr ? this->typeRelations->unify(element, type) : type;
    if (element == nullptr) {
      return nullptr;
    }
  }

  return element != nullptr && !element->isUndefined() ? Type::array(element) : nullptr;
}

const Type *GenericChecker::objectShape(const ObjectLitExpr *literal,
                                        std::vector<const Type *> &answers) const {
  std::vector<const Type *> types = ChildrenFirst::takeAnswers(answers, literal->props.size());
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < types.size(); ++index) {
    const ObjectLitExpr::Property &property = literal->props[index];
    bool isRepeated = this->hasFieldNamed(fields, property.name);
    if (types[index]->isUndefined() || isRepeated) {
      return nullptr;
    }
    auto field = Field{.name = property.name.str(), .type = types[index], .readonly = false};
    fields.push_back(field);
  }

  return Type::object(std::move(fields));
}

bool GenericChecker::hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return true;
    }
  }

  return false;
}

}
