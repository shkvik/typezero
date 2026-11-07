#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/whole_program/load_order/load_order_errors.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

class LoadOrder {
public:
  explicit LoadOrder(CheckState &owner);

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

  static void spreadEffects(LoadState &loadState);
  static std::string calleeName(const FunctionDecl *fn);
  static void need(const FunctionDecl *fn, LoadState &loadState);
  static const FunctionDecl *directFunction(const Expr *expr);
  static void functionEffects(const FunctionDecl *fn, LoadState &loadState);
  static void declarationOrder(SourceFile &program, LoadState &loadState);
  static void calledFunctions(const CallExpr *call,
                              llvm::SmallVectorImpl<const FunctionDecl *> &called);
  static const CallExpr *constructorCall(const Expr *expr,
                                         llvm::SmallVectorImpl<const FunctionDecl *> &called);
  static ResultReadAfterSet earlyRead(const NameExpr *name, const Module *module, int time,
                                      const LoadState &loadState);
  static ResultSetBeforeRun lateUse(const Site &site, const FunctionDecl *fn, const Use &use,
                                    const LoadState &loadState);

  void reportSites(const LoadState &loadState);
  void topLevelUses(SourceFile &program, LoadState &loadState);
  void visitTopLevel(const Expr *e, Module *module, int time, LoadState &loadState);

  CheckState &state;
};

}
