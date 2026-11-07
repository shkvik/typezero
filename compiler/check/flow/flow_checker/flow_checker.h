#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FlowChecker {
public:
  explicit FlowChecker(Checker &owner);

  static Checker::Flow join(const Checker::Flow &a, const Checker::Flow &b);
  void assigned(const VarSymbol *var, const Expr *value);
  void incremented(const VarSymbol *var, int64_t delta);
  void calledFunction();
  void forgetCallEffects(Checker::Flow &flow) const;
  bool changedByCalls(const VarSymbol *var) const;
  static bool staysLocal(const FunctionDecl *fn, llvm::StringRef name);
  void arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except = nullptr);
  void setDefined(const VarSymbol *var, bool defined);

private:
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);
  static bool isArrayLength(const Term &t);

  Checker &checker;
};

}
