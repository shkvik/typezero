#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

namespace tz {

struct Untyped {
  VarSymbol *var;
  bool initUndefined;
  std::vector<const Type *> assigned;
};

struct ModuleLoadMarks {
  llvm::SmallVector<llvm::StringRef, 2> initializing;
  BinaryExpr *concatChain = nullptr;
  const BinaryExpr *mixedConcat = nullptr;
};

class ExpressionMarks {
public:
  void rememberConcatChain(BinaryExpr *chain);
  void forgetConcatChain();
  BinaryExpr *takeConcatChain();
  void rememberMixedConcat(const BinaryExpr *mixed);
  const BinaryExpr *takeMixedConcat();
  const BinaryExpr *getMixedConcat() const;
  void enterVariableInit(llvm::StringRef name);
  void leaveVariableInit();
  bool isInitializing(llvm::StringRef name) const;
  void enterModuleLoad();
  void leaveModuleLoad();
  void markPrechecked(const Expr *expr);
  bool isPrechecked(const Expr *expr) const;
  void forgetPrechecked(const Expr *expr);
  void markPairLiteral(const Expr *literal);
  bool isPairLiteral(const Expr *literal) const;
  void forgetPairLiteral(const Expr *literal);
  void markSeveralVariants(const CallExpr *call);
  bool hasSeveralVariants(const CallExpr *call) const;
  void rememberArgumentList(const VarSymbol *variable, std::vector<llvm::StringRef> parts);
  std::optional<std::vector<llvm::StringRef>> findArgumentList(const VarSymbol *variable) const;
  void rememberIterateSymbol(FuncSymbol *function);
  FuncSymbol *getIterateSymbol() const;
  void addUntyped(VarSymbol *variable, bool initUndefined);
  bool hasUntyped() const;
  void addUntypedAssignment(const VarSymbol *variable, const Type *type);
  std::vector<Untyped> getUntyped() const;
  void forgetUntyped(const VarSymbol *variable);
  void addResultVar(VarSymbol *variable);
  std::vector<VarSymbol *> getResultVars() const;
  void forgetResultVars();

private:
  BinaryExpr *concatChain = nullptr;
  const BinaryExpr *mixedConcat = nullptr;
  llvm::SmallVector<llvm::StringRef, 2> initializing;
  std::vector<ModuleLoadMarks> moduleLoads;
  llvm::SmallPtrSet<const Expr *, 4> prechecked;
  llvm::SmallPtrSet<const Expr *, 4> pairLiterals;
  llvm::SmallPtrSet<const CallExpr *, 4> severalVariants;
  llvm::DenseMap<const VarSymbol *, std::vector<llvm::StringRef>> argumentLists;
  FuncSymbol *iterateSymbol = nullptr;
  std::vector<Untyped> untyped;
  std::vector<VarSymbol *> resultVars;
};

}
