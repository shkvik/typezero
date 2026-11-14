#include "compiler/semantic/semantic_state/v2/code/constant_context/constant_context.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include <limits>
#include <vector>

using llvm::dyn_cast;

namespace tz {

std::optional<int64_t> ConstantContext::intConstant(const Expr *expr) const {
  auto casts = std::vector<const CastExpr *>();
  const Expr *inner = expr;
  auto *conversion = dyn_cast<CastExpr>(inner);
  while (conversion != nullptr) {
    bool isInteger = conversion->type != nullptr && conversion->type->isInteger();
    if (!isInteger || !conversion->isImplicit()) {
      return std::nullopt;
    }
    casts.push_back(conversion);
    inner = conversion->operand;
    conversion = dyn_cast<CastExpr>(inner);
  }
  auto value = this->plainIntConstant(inner);
  if (!value.has_value()) {
    return std::nullopt;
  }
  for (const CastExpr *outer : casts) {
    if (!this->isFitting(outer->type, *value)) {
      return std::nullopt;
    }
  }

  return value;
}

std::optional<int64_t> ConstantContext::plainIntConstant(const Expr *expr) const {
  if (expr->type == nullptr || !expr->type->isInteger()) {
    return std::nullopt;
  }
  auto *literal = dyn_cast<IntLitExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  if (literal != nullptr) {
    return this->literalConstant(literal);
  }
  if (name != nullptr) {
    return this->findConstVar(name->var());
  }

  return this->findIntConstant(expr);
}

std::optional<int64_t> ConstantContext::literalConstant(const IntLitExpr *literal) const {
  uint64_t value = literal->value;
  if (value > uint64_t(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }
  auto signedValue = static_cast<int64_t>(value);
  if (!this->isFitting(literal->type, signedValue)) {
    return std::nullopt;
  }

  return signedValue;
}

std::optional<std::string> ConstantContext::constantKey(const Expr *key) const {
  const Expr *plain = ExprParts::withoutImplicitCasts(key);
  const auto *literal = dyn_cast<StringLitExpr>(plain);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  const auto *templateExpr = dyn_cast<TemplateExpr>(plain);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }
  const auto *name = dyn_cast<NameExpr>(plain);
  if (name == nullptr || name->var() == nullptr) {
    return std::nullopt;
  }

  return this->findConstKey(name->var());
}

bool ConstantContext::isFitting(const Type *type, int64_t value) const {
  if (value < type->intMin()) {
    return false;
  }

  return value < 0 || static_cast<uint64_t>(value) <= type->intMax();
}

void ConstantContext::rememberIntConstant(const Expr *expr, int64_t value) {
  this->intConstants[expr] = value;
}

std::optional<int64_t> ConstantContext::findIntConstant(const Expr *expr) const {
  auto found = this->intConstants.find(expr);
  if (found == this->intConstants.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantContext::rememberBigUnsigned(const Expr *expr, uint64_t value) {
  this->bigUnsigned[expr] = value;
}

std::optional<uint64_t> ConstantContext::findBigUnsigned(const Expr *expr) const {
  auto found = this->bigUnsigned.find(expr);
  if (found == this->bigUnsigned.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantContext::rememberBigUnsignedVar(const VarSymbol *variable, uint64_t value) {
  this->bigUnsignedVars[variable] = value;
}

std::optional<uint64_t> ConstantContext::findBigUnsignedVar(const VarSymbol *variable) const {
  auto found = this->bigUnsignedVars.find(variable);
  if (found == this->bigUnsignedVars.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantContext::rememberConstVar(const VarSymbol *variable, int64_t value) {
  this->constVars[variable] = value;
}

std::optional<int64_t> ConstantContext::findConstVar(const VarSymbol *variable) const {
  auto found = this->constVars.find(variable);
  if (found == this->constVars.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantContext::rememberConstKey(const VarSymbol *variable, std::string key) {
  this->constKeys[variable] = std::move(key);
}

std::optional<std::string> ConstantContext::findConstKey(const VarSymbol *variable) const {
  auto found = this->constKeys.find(variable);
  if (found == this->constKeys.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantContext::rememberConstPrefix(const VarSymbol *variable, std::string prefix) {
  this->constPrefixes[variable] = std::move(prefix);
}

std::string ConstantContext::findConstPrefix(const VarSymbol *variable) const {
  auto found = this->constPrefixes.find(variable);
  if (found == this->constPrefixes.end()) {
    return std::string();
  }

  return found->second;
}

void ConstantContext::rememberConstLiteral(const VarSymbol *variable, const Expr *literal) {
  this->constLiterals[variable] = literal;
}

const Expr *ConstantContext::findConstLiteral(const VarSymbol *variable) const {
  return this->constLiterals.lookup(variable);
}

}
