#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

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

class LateClassWalks {
public:
  LateClassWalks(SemanticState &owner, ClassFacts &sharedFacts);

  void nextLateClass();
  void pushLateClassTasks();
  void walkConstructorCalls();
  void rememberEmittedLateClasses();
  void markLateOverrides(SourceFile &program);
  void walkLateClass(const WalkLateClass &task);
  void finishLateClassWalk(const FinishLateClassWalk &task);

private:
  using ResultNotOverridden = Result<FunctionDecl *, CodeClassesRegistryOverriddenCallError>;

  void markLateFailure(const LateError &error);
  void markClassOverrides(ClassDecl *classDecl);
  void noteMethodNames(const ClassDecl *classDecl);
  void walkClassConstructors(const ClassDecl *classDecl);
  void scanConstructorCalls(const FunctionDecl *constructor, const ClassDecl *classDecl);
  void markScanned(const ScanFrame &frame, llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const;
  void enterScan(std::vector<ScanFrame> &frames, const FunctionDecl *functionDecl,
                 const CallExpr *origin, llvm::SmallPtrSetImpl<const FunctionDecl *> &seen) const;


  bool isExtending(const ClassDecl *classDecl) const;
  bool hasOverridingChildren(const ClassDecl *classDecl) const;


  const NameExpr *receiverName(const CallExpr *call) const;
  const CallExpr *selfCall(const ScanFrame &frame, const Expr *expr) const;


  std::optional<ScanTarget> followedTarget(const ScanFrame &frame, const Expr *expr,
                                           const ClassDecl *classDecl,
                                           const FunctionDecl *constructor);
  std::optional<ScanTarget> takeNextTarget(ScanFrame &frame, const ClassDecl *classDecl,
                                           const FunctionDecl *constructor,
                                           llvm::SmallPtrSetImpl<const FunctionDecl *> &seen);


  const ClassDecl *overridingChild(const ClassDecl *classDecl, const FunctionDecl *method) const;
  ResultNotOverridden overriddenCall(const ClassDecl *classDecl, const FunctionDecl *constructor,
                                     const CallExpr *call, const CallExpr *site) const;

  ClassFacts &facts;
  SemanticState &state;
};

}
