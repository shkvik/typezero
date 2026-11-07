#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/result/result/result.h"
#include "compiler/whole_program/load_order/load_order_errors.h"
#include <vector>

namespace tz {

class LoadOrder {
public:
  explicit LoadOrder(Checker &owner);

  void checkLoadOrder(SourceFile &program);

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

  static void spreadEffects(LoadState &state);
  static std::string calleeName(const FunctionDecl *fn);
  static void need(const FunctionDecl *fn, LoadState &state);
  static const FunctionDecl *directFunction(const Expr *expr);
  static void functionEffects(const FunctionDecl *fn, LoadState &state);
  static void declarationOrder(SourceFile &program, LoadState &state);
  static void calledFunctions(const CallExpr *call,
                              llvm::SmallVectorImpl<const FunctionDecl *> &called);
  static const CallExpr *constructorCall(const Expr *expr,
                                         llvm::SmallVectorImpl<const FunctionDecl *> &called);
  static ResultReadAfterSet earlyRead(const NameExpr *name, const Module *module, int time,
                                      const LoadState &state);
  static ResultSetBeforeRun lateUse(const Site &site, const FunctionDecl *fn, const Use &use,
                                    const LoadState &state);

  void reportSites(const LoadState &state);
  void topLevelUses(SourceFile &program, LoadState &state);
  void visitTopLevel(const Expr *e, Module *module, int time, LoadState &state);

  Checker &checker;
};

}
