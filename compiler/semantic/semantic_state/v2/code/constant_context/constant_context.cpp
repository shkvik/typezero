#include "compiler/semantic/semantic_state/v2/code/constant_context/constant_context.h"

namespace tz {

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
