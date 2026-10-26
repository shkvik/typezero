#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

class CallRanges {
public:
  explicit CallRanges(OverflowCheck &check);

  bool mergeSummaries(bool narrowing);
  void analyzeAll();
  void analyzeFunction(const FunctionDecl *fn);
  Val paramValue(const FunctionDecl *fn, size_t index);
  Val evalCall(const CallExpr *call, State &st);
  Val evalCallback(const CallExpr *call, State &st);
  Val evalNew(const NewExpr *ne, State &st);
  Val runCallback(const FunctionDecl *fn, llvm::ArrayRef<Val> args, size_t first, State &st);
  void passArgs(const FunctionDecl *fn, const CallExpr *call, llvm::ArrayRef<Val> args);
  Val returned(const FunctionDecl *fn, const Type *type);
  void calledUserCode(State &st);

private:
  OverflowCheck &check_;
};

}
