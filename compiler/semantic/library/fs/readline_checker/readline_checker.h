#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/fs/readline_checker/readline_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FsChecker;
class CallChecker;
class ExprChecker;
class ProcessChecker;
class ArgumentChecker;

class ReadlineChecker {
public:
  ReadlineChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  using ResultInputIsStdin = Result<const Expr *, ReadlineCheckerStdinInputError>;
  using ResultReadlineUsed = Result<llvm::StringRef, ReadlineCheckerReadlineFunctionError>;

  ResultInputIsStdin stdinInput(const Expr *input);
  static ResultReadlineUsed readlineFunction(llvm::StringRef fn, NodeModule module);

  SemanticState &state;
  FsChecker *fsChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
