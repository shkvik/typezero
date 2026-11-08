#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/DenseMap.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

struct ConstantValue {
  bool isString = false;
  double number = 0;
  std::u16string text;
};

class ConstantStore {
public:
  void rememberIntConstant(const Expr *expr, int64_t value);
  std::optional<int64_t> findIntConstant(const Expr *expr) const;
  void rememberBigUnsigned(const Expr *expr, uint64_t value);
  std::optional<uint64_t> findBigUnsigned(const Expr *expr) const;
  void rememberBigUnsignedVar(const VarSymbol *var, uint64_t value);
  std::optional<uint64_t> findBigUnsignedVar(const VarSymbol *var) const;
  void rememberConstVar(const VarSymbol *var, int64_t value);
  std::optional<int64_t> findConstVar(const VarSymbol *var) const;
  void rememberConstKey(const VarSymbol *var, std::string key);
  std::optional<std::string> findConstKey(const VarSymbol *var) const;
  void rememberConstPrefix(const VarSymbol *var, std::string prefix);
  std::string findConstPrefix(const VarSymbol *var) const;
  void rememberConstLiteral(const VarSymbol *var, const Expr *literal);
  const Expr *findConstLiteral(const VarSymbol *var) const;

private:
  llvm::DenseMap<const Expr *, int64_t> intConstants;
  llvm::DenseMap<const Expr *, uint64_t> bigUnsigned;
  llvm::DenseMap<const VarSymbol *, uint64_t> bigUnsignedVars;
  llvm::DenseMap<const VarSymbol *, int64_t> constVars;
  llvm::DenseMap<const VarSymbol *, std::string> constKeys;
  llvm::DenseMap<const VarSymbol *, std::string> constPrefixes;
  llvm::DenseMap<const VarSymbol *, const Expr *> constLiterals;
};

}
