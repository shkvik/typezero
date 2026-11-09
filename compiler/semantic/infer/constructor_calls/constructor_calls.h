#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/infer/constructor_calls/constructor_calls_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

struct ScanFrame {
  const FunctionDecl *fn;
  const CallExpr *origin;
  std::vector<const Expr *> exprs;
  size_t next;
};

struct ScanTarget {
  const FunctionDecl *fn;
  const CallExpr *site;
};

class ConstructorCalls {
public:
  explicit ConstructorCalls(SemanticState &owner);

  void checkConstructorCalls();

private:
  using ResultNotOverridden = Result<FunctionDecl *, ConstructorCallsOverriddenCallError>;

  static const NameExpr *receiverName(const CallExpr *call);
  static const ClassDecl *overridingChild(const ClassDecl *cls, const FunctionDecl *method);
  static ResultNotOverridden overriddenCall(const ClassDecl *cls, const FunctionDecl *ctor,
                                            const CallExpr *call, const CallExpr *site);

  static void enterScan(std::vector<ScanFrame> &frames, const FunctionDecl *fn,
                        const CallExpr *origin, llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);

  void scanConstructorCalls(const FunctionDecl *ctor, const ClassDecl *cls);
  std::optional<ScanTarget> takeNextTarget(ScanFrame &frame, const ClassDecl *cls,
                                           const FunctionDecl *ctor,
                                           llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);
  const CallExpr *selfCall(const ScanFrame &frame, const Expr *e) const;

  SemanticState &state;
};

}
