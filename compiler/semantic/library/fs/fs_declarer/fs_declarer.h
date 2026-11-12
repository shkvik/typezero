#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ProcessChecker;
class ArgumentChecker;

class FsDeclarer {
public:
  FsDeclarer(SemanticState &owner, SemanticParts &all);

  void declareFsClasses();
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *classDecl);
  bool isRuntimeClass(const ClassDecl *classDecl) const;

private:
  std::optional<int> fileKind(llvm::StringRef method) const;

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
