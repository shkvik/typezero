#include "compiler/pipeline/check_program/check_state/constant_store/constant_store.h"

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

void ConstantStore::rememberBigUnsignedVar(const VarSymbol *var, uint64_t value) {
  this->bigUnsignedVars[var] = value;
}

std::optional<uint64_t> ConstantStore::findBigUnsignedVar(const VarSymbol *var) const {
  auto found = this->bigUnsignedVars.find(var);
  if (found == this->bigUnsignedVars.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberConstVar(const VarSymbol *var, int64_t value) {
  this->constVars[var] = value;
}

std::optional<int64_t> ConstantStore::findConstVar(const VarSymbol *var) const {
  auto found = this->constVars.find(var);
  if (found == this->constVars.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberConstKey(const VarSymbol *var, std::string key) {
  this->constKeys[var] = std::move(key);
}

std::optional<std::string> ConstantStore::findConstKey(const VarSymbol *var) const {
  auto found = this->constKeys.find(var);
  if (found == this->constKeys.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ConstantStore::rememberConstPrefix(const VarSymbol *var, std::string prefix) {
  this->constPrefixes[var] = std::move(prefix);
}

std::string ConstantStore::findConstPrefix(const VarSymbol *var) const {
  auto found = this->constPrefixes.find(var);
  if (found == this->constPrefixes.end()) {
    return std::string();
  }

  return found->second;
}

void ConstantStore::rememberConstLiteral(const VarSymbol *var, const Expr *literal) {
  this->constLiterals[var] = literal;
}

const Expr *ConstantStore::findConstLiteral(const VarSymbol *var) const {
  return this->constLiterals.lookup(var);
}

}
