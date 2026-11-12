#include "compiler/semantic/semantic_state/constant_store/constant_store.h"

namespace tz {

void ConstantStore::rememberIntConstant(const Expr *expr, int64_t value) {
  this->intConstants[expr] = value;
}

std::optional<int64_t> ConstantStore::findIntConstant(const Expr *expr) const {
  auto found = this->intConstants.find(expr);
  if (found == this->intConstants.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberBigUnsigned(const Expr *expr, uint64_t value) {
  this->bigUnsigned[expr] = value;
}

std::optional<uint64_t> ConstantStore::findBigUnsigned(const Expr *expr) const {
  auto found = this->bigUnsigned.find(expr);
  if (found == this->bigUnsigned.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberBigUnsignedVar(const VarSymbol *variable, uint64_t value) {
  this->bigUnsignedVars[variable] = value;
}

std::optional<uint64_t> ConstantStore::findBigUnsignedVar(const VarSymbol *variable) const {
  auto found = this->bigUnsignedVars.find(variable);
  if (found == this->bigUnsignedVars.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberConstVar(const VarSymbol *variable, int64_t value) {
  this->constVars[variable] = value;
}

std::optional<int64_t> ConstantStore::findConstVar(const VarSymbol *variable) const {
  auto found = this->constVars.find(variable);
  if (found == this->constVars.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberConstKey(const VarSymbol *variable, std::string key) {
  this->constKeys[variable] = std::move(key);
}

std::optional<std::string> ConstantStore::findConstKey(const VarSymbol *variable) const {
  auto found = this->constKeys.find(variable);
  if (found == this->constKeys.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberConstPrefix(const VarSymbol *variable, std::string prefix) {
  this->constPrefixes[variable] = std::move(prefix);
}

std::string ConstantStore::findConstPrefix(const VarSymbol *variable) const {
  auto found = this->constPrefixes.find(variable);
  if (found == this->constPrefixes.end()) {
    return std::string();
  }

  return found->second;
}

void ConstantStore::rememberConstLiteral(const VarSymbol *variable, const Expr *literal) {
  this->constLiterals[variable] = literal;
}

const Expr *ConstantStore::findConstLiteral(const VarSymbol *variable) const {
  return this->constLiterals.lookup(variable);
}

}
