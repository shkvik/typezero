#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/key_pair_checker/key_pair_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class KeyPairChecker {
public:
  explicit KeyPairChecker(SemanticState &owner);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc, llvm::StringRef shown);

private:
  using ResultKeyOptionsGiven = Result<int, KeyPairCheckerKeyOptionsError>;

  static ResultKeyOptionsGiven keyOptions(int which, llvm::StringRef type,
                                          const llvm::StringMap<Expr *> &options);

  static std::optional<std::string> literalOf(const Expr *e);

  SemanticState &state;
};

}
