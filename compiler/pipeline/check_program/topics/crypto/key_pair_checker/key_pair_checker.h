#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/crypto/key_pair_checker/key_pair_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class KeyPairChecker {
public:
  explicit KeyPairChecker(CheckState &owner);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc, llvm::StringRef shown);

private:
  using ResultKeyOptionsGiven = Result<int, KeyPairCheckerKeyOptionsError>;

  static ResultKeyOptionsGiven keyOptions(int which, llvm::StringRef type,
                                          const llvm::StringMap<Expr *> &options);

  static std::optional<std::string> literalOf(const Expr *e);

  CheckState &state;
};

}
