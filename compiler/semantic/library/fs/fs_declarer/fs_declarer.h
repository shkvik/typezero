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
  Expr *checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkStatsDate(MemberExpr *member, ClassDecl *cls);
  bool isRuntimeClass(const ClassDecl *cls) const;

private:
  static std::optional<int> fileKind(llvm::StringRef method);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
