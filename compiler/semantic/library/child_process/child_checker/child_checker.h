#pragma once

#include "compiler/semantic/library/child_process/child_checker/child_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FsChecker;
class CallChecker;
class ExprChecker;
class BytesChecker;
class TypeRelations;
class ArgumentChecker;
class ChildErrorTypes;

class ChildChecker {
public:
  ChildChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  static const Type *bufferType();
  static bool knownSignal(llvm::StringRef name);
  static bool hasZero(const Expr *expr);

  SemanticState &state;
  FsChecker *fsChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ChildErrorTypes *childErrorTypes = nullptr;
};

}
