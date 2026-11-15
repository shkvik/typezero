#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

class ConstantContext {
public:
  void rememberIntConstant(const Expr *expr, int64_t value);
  void rememberBigUnsigned(const Expr *expr, uint64_t value);
  void rememberConstVar(const VarSymbol *variable, int64_t value);
  void rememberConstKey(const VarSymbol *variable, std::string key);
  void rememberBigUnsignedVar(const VarSymbol *variable, uint64_t value);
  void rememberConstPrefix(const VarSymbol *variable, std::string prefix);
  void rememberConstLiteral(const VarSymbol *variable, const Expr *literal);


  std::optional<int64_t> intConstant(const Expr *expr) const;
  std::optional<int64_t> findIntConstant(const Expr *expr) const;
  std::optional<int64_t> findConstVar(const VarSymbol *variable) const;


  std::optional<uint64_t> findBigUnsigned(const Expr *expr) const;
  std::optional<uint64_t> findBigUnsignedVar(const VarSymbol *variable) const;


  std::string findConstPrefix(const VarSymbol *variable) const;
  const Expr *findConstLiteral(const VarSymbol *variable) const;
  std::optional<std::string> constantKey(const Expr *key) const;
  std::optional<std::string> findConstKey(const VarSymbol *variable) const;

private:
  bool isFitting(const Type *type, int64_t value) const;


  std::optional<int64_t> plainIntConstant(const Expr *expr) const;
  std::optional<int64_t> literalConstant(const IntLitExpr *literal) const;

  llvm::DenseMap<const Expr *, int64_t> intConstants;
  llvm::DenseMap<const Expr *, uint64_t> bigUnsigned;
  llvm::DenseMap<const VarSymbol *, int64_t> constVars;
  llvm::DenseMap<const VarSymbol *, std::string> constKeys;
  llvm::DenseMap<const VarSymbol *, uint64_t> bigUnsignedVars;
  llvm::DenseMap<const VarSymbol *, std::string> constPrefixes;
  llvm::DenseMap<const VarSymbol *, const Expr *> constLiterals;
};

}
