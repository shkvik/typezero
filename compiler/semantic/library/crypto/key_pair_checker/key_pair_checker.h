#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/key_pair_checker/key_pair_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CryptoArgs;
class CryptoErrors;
class ArgumentChecker;

class KeyPairChecker {
public:
  KeyPairChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc, llvm::StringRef shown);

private:
  using ResultKeyOptionsGiven = Result<int, KeyPairCheckerKeyOptionsError>;

  static ResultKeyOptionsGiven keyOptions(int which, llvm::StringRef type,
                                          const llvm::StringMap<Expr *> &options);

  static std::optional<std::string> literalOf(const Expr *e);

  SemanticState &state;
  CryptoArgs *cryptoArgs = nullptr;
  CryptoErrors *cryptoErrors = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
