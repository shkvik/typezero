#include "compiler/semantic/code/conversions/code_conversions.h"
#include "compiler/program/library/number/number_method_table/number_method_table.h"
#include "compiler/semantic/code/conversions/code_conversions_registry_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/v2/code/code_storage.h"
#include <cctype>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenNamePrefix = "%";

}

CodeConversions::CodeConversions(CodeStorage &owner)
    : code(owner) {}

bool CodeConversions::isIntLiteralTree(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool CodeConversions::isIntLiteralNode(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);

  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (name != nullptr) {
    return name->var() != nullptr && name->var()->literalConst;
  }
  if (unary != nullptr) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  if (binary != nullptr) {
    return binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub ||
           binary->op == BinaryOp::Mul || binary->op == BinaryOp::Rem ||
           binary->op == BinaryOp::Pow;
  }

  return false;
}

bool CodeConversions::isNumberLiteralTree(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isNumberLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool CodeConversions::isNumberLiteralNode(const Expr *expr) const {
  if (isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr)) {
    return true;
  }
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  const auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary == nullptr) {
    return false;
  }

  return this->isArithmeticOperator(binary->op);
}

bool CodeConversions::isArithmeticOperator(BinaryOp op) const {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
         op == BinaryOp::Rem || op == BinaryOp::Pow;
}

const Type *CodeConversions::literalHint(const Expr *other, const Type *hint) const {
  return other->type != nullptr && other->type->nonOptional()->isDecimal() ? Type::decimalTy()
                                                                           : hint;
}

const Type *CodeConversions::literalTarget(const Expr *expr, const Type *to) const {
  const Type *target = this->numberTarget(to);
  bool isLiteral = target != nullptr && expr->type != nullptr && expr->type->isInteger() &&
                   expr->type != target && this->isIntLiteralTree(expr);
  if (!isLiteral) {
    return nullptr;
  }
  if (target->isFloat() || target->isDecimal()) {
    return target;
  }
  if (!target->isInteger()) {
    return nullptr;
  }
  auto value = this->code.constants.intConstant(expr);
  if (!value.has_value() || !this->isValueFitting(*value, expr->type, target)) {
    return nullptr;
  }

  return target;
}

const Type *CodeConversions::numberTarget(const Type *to) const {
  const Type *target = to->nonOptional();
  if (!target->isUnion()) {
    return target;
  }

  return this->wholeNumberMember(target);
}

const Type *CodeConversions::wholeNumberMember(const Type *type) const {
  for (const Type *candidate : type->members()) {
    bool isNumber = candidate->isInteger() || candidate->isFloat() || candidate->isDecimal();
    if (isNumber) {
      return candidate;
    }
  }

  return nullptr;
}

bool CodeConversions::isValueFitting(int64_t value, const Type *from, const Type *target) const {
  if (value < 0 && !from->isUnsigned()) {
    return !target->isUnsigned() && value >= target->intMin();
  }

  return uint64_t(value) <= target->intMax();
}

bool CodeConversions::isLiteralAdapting(const Expr *expr, const Type *to) const {
  return this->literalTarget(expr, to) != nullptr;
}

std::string CodeConversions::operandText(llvm::StringRef text) const {
  bool isLiteral = !text.empty() && std::isdigit(static_cast<unsigned char>(text.front()));
  bool isPlain = this->isPlainOperand(text) || this->isWrappedInParens(text);
  if (isPlain && !isLiteral) {
    return text.str();
  }

  return "(" + text.str() + ")";
}

bool CodeConversions::isPlainOperand(llvm::StringRef text) const {
  if (text.empty() || text.front() == '-' || text.front() == '+') {
    return false;
  }
  int depth = 0;
  for (char unit : text) {
    bool isOpening = unit == '(' || unit == '[';
    bool isClosing = unit == ')' || unit == ']';
    bool isBracket = isOpening || isClosing;
    if (isOpening) {
      depth += 1;
    }
    if (isClosing) {
      depth -= 1;
    }
    if (!isBracket && depth == 0 && !this->isNameUnit(unit)) {
      return false;
    }
  }

  return true;
}

bool CodeConversions::isNameUnit(char unit) const {
  bool isWordUnit = std::isalnum(static_cast<unsigned char>(unit)) != 0 || unit == '_';

  return isWordUnit || unit == '.' || unit == '#' || unit == '$' || unit == '"' || unit == '\'';
}

bool CodeConversions::isWrappedInParens(llvm::StringRef text) const {
  if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
    return false;
  }
  int level = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '(') {
      level += 1;
    }
    if (text[index] == ')') {
      level -= 1;
    }
    if (level == 0 && index + 1 < text.size()) {
      return false;
    }
  }

  return true;
}

std::string CodeConversions::conversionHint(const Expr *expr, const Type *to) const {
  if (expr->type == nullptr) {
    return "";
  }
  const Type *from = expr->type->nonOptional();
  const Type *target = to->nonOptional();
  if (!this->isNumberLike(from) || !this->isNumberLike(target)) {
    return "";
  }
  auto text = this->shownOperand(expr);
  auto method = this->conversionMethod(target);
  bool isKnownTarget = target->isInteger() || target->isFloat() || target->isDecimal();
  if (from->isInteger() && isKnownTarget) {
    return ErrorTextConvert::direct(text, method);
  }
  if (from->isFloat() && target->isInteger()) {
    return ErrorTextConvert::rounded(text, method);
  }

  return "";
}

bool CodeConversions::isNumberLike(const Type *type) const {
  return type->isNumeric() || type->isDecimal();
}

std::string CodeConversions::shownOperand(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);

  if (name != nullptr && !name->name.starts_with(kHiddenNamePrefix)) {
    return name->name.str();
  }
  if (name == nullptr && member != nullptr) {
    return this->shownMember(member);
  }

  return "x";
}

std::string CodeConversions::shownMember(const MemberExpr *member) const {
  auto *owner = dyn_cast<NameExpr>(member->object);
  if (owner == nullptr || owner->name.starts_with(kHiddenNamePrefix)) {
    return "x";
  }

  return owner->name.str() + "." + member->name.str();
}

std::string CodeConversions::conversionMethod(const Type *to) const {
  return NumberMethodTable::getConversionName(to->name());
}

}
