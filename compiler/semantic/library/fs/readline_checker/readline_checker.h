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

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef functionName,
                          llvm::SMLoc loc);

private:
  using ResultInputIsStdin = Result<const Expr *, ReadlineCheckerStdinInputError>;
  using ResultReadlineUsed = Result<llvm::StringRef, ReadlineCheckerReadlineFunctionError>;

  ResultInputIsStdin checkStdinInput(const Expr *input);
  ResultReadlineUsed checkReadlineFunction(llvm::StringRef functionName, NodeModule module) const;

  struct ReadlineStreams {
    Expr *input;
    Expr *output;
  };

  Expr *checkArgsLoosely(CallExpr *call);
  bool hasRejectedArgs(CallExpr *call, llvm::SMLoc loc);
  std::optional<ReadlineStreams> readlineStreams(CallExpr *call, llvm::StringRef functionName);
  std::optional<ReadlineStreams> optionStreams(CallExpr *call, llvm::StringRef functionName);
  std::optional<int> outputStream(const Expr *output);

  SemanticState &state;
  FsChecker *fsChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
