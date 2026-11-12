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
  void rememberBigUnsignedVar(const VarSymbol *variable, uint64_t value);
  std::optional<uint64_t> findBigUnsignedVar(const VarSymbol *variable) const;
  void rememberConstVar(const VarSymbol *variable, int64_t value);
  std::optional<int64_t> findConstVar(const VarSymbol *variable) const;
  void rememberConstKey(const VarSymbol *variable, std::string key);
  std::optional<std::string> findConstKey(const VarSymbol *variable) const;
  void rememberConstPrefix(const VarSymbol *variable, std::string prefix);
  std::string findConstPrefix(const VarSymbol *variable) const;
  void rememberConstLiteral(const VarSymbol *variable, const Expr *literal);
  const Expr *findConstLiteral(const VarSymbol *variable) const;

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
