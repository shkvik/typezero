#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/fs/readline_checker/readline_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ReadlineChecker {
public:
  explicit ReadlineChecker(CheckState &owner);

  Expr *checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc);

private:
  using ResultInputIsStdin = Result<const Expr *, ReadlineCheckerStdinInputError>;
  using ResultReadlineUsed = Result<llvm::StringRef, ReadlineCheckerReadlineFunctionError>;

  ResultInputIsStdin stdinInput(const Expr *input);
  static ResultReadlineUsed readlineFunction(llvm::StringRef fn, NodeModule module);

  CheckState &state;
};

}
