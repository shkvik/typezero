#pragma once

#include "compiler/interprocedural/load_order/load_order_errors.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

class LoadOrder {
public:
  using ResultLoadOrderChecked = Result<void, std::vector<Diagnostic>>;

  ResultLoadOrderChecked checkLoadOrder(SourceFile &program) const;

private:
  struct Use {
    int setAt = 0;
    llvm::SMLoc loc;
    const VarSymbol *var = nullptr;
  };

  struct Effects {
    Use latest;
    std::vector<const FunctionDecl *> calls;
  };

  struct Site {
    int time = 0;
    Module *module = nullptr;
    const CallExpr *call = nullptr;
    llvm::SmallVector<const FunctionDecl *, 2> called;
  };

  struct LoadState {
    std::vector<Site> sites;
    std::vector<const FunctionDecl *> queue;
    llvm::DenseMap<const VarSymbol *, int> setAt;
    llvm::DenseMap<const VarSymbol *, Module *> homes;
    llvm::DenseMap<const FunctionDecl *, Effects> effects;
  };

  using ResultReadAfterSet = Result<const NameExpr *, LoadOrderEarlyReadError>;
  using ResultSetBeforeRun = Result<const FunctionDecl *, LoadOrderLateUseError>;

  void spreadEffects(LoadState &loadState) const;
  std::string calleeName(const FunctionDecl *fn) const;
  void need(const FunctionDecl *fn, LoadState &loadState) const;
  const FunctionDecl *directFunction(const Expr *expr) const;
  void functionEffects(const FunctionDecl *fn, LoadState &loadState) const;
  void declarationOrder(SourceFile &program, LoadState &loadState) const;
  void calledFunctions(const CallExpr *call,
                       llvm::SmallVectorImpl<const FunctionDecl *> &called) const;
  const CallExpr *constructorCall(const Expr *expr,
                                  llvm::SmallVectorImpl<const FunctionDecl *> &called) const;
  ResultReadAfterSet earlyRead(const NameExpr *name, const Module *module, int time,
                               const LoadState &loadState) const;
  ResultSetBeforeRun lateUse(const Site &site, const FunctionDecl *fn, const Use &use,
                             const LoadState &loadState) const;

  std::vector<Diagnostic> findLateUses(const LoadState &loadState) const;
  std::vector<Diagnostic> topLevelUses(SourceFile &program, LoadState &loadState) const;
  std::vector<Diagnostic> visitTopLevel(const std::vector<const Expr *> &exprs, Module *module,
                                        int time, LoadState &loadState) const;
  void rememberSite(const Expr *e, Module *module, int time, LoadState &loadState) const;
};

}
