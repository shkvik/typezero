#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/infer/constructor_calls/constructor_calls_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ClassLinks;

struct ScanFrame {
  const FunctionDecl *functionDecl;
  const CallExpr *origin;
  std::vector<const Expr *> exprs;
  size_t next;
};

struct ScanTarget {
  const FunctionDecl *functionDecl;
  const CallExpr *site;
};

class ConstructorCalls {
public:
  ConstructorCalls(SemanticState &owner, SemanticParts &all);

  void walkConstructorCalls();

private:
  using ResultNotOverridden = Result<FunctionDecl *, ConstructorCallsOverriddenCallError>;

  const NameExpr *receiverName(const CallExpr *call) const;
  const ClassDecl *overridingChild(const ClassDecl *classDecl, const FunctionDecl *method) const;
  ResultNotOverridden overriddenCall(const ClassDecl *classDecl, const FunctionDecl *constructor,
                                     const CallExpr *call, const CallExpr *site) const;

  void enterScan(std::vector<ScanFrame> &frames, const FunctionDecl *functionDecl,
                 const CallExpr *origin, llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const;

  void scanConstructorCalls(const FunctionDecl *constructor, const ClassDecl *classDecl);
  std::optional<ScanTarget> takeNextTarget(ScanFrame &frame, const ClassDecl *classDecl,
                                           const FunctionDecl *constructor,
                                           llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);
  const CallExpr *selfCall(const ScanFrame &frame, const Expr *expr) const;
  void walkClassConstructors(const ClassDecl *classDecl);
  bool hasOverridingChildren(const ClassDecl *classDecl) const;
  void markScanned(const ScanFrame &frame, llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const;
  std::optional<ScanTarget> followedTarget(const ScanFrame &frame, const Expr *expr,
                                           const ClassDecl *classDecl,
                                           const FunctionDecl *constructor);

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
};

}
