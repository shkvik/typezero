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

  ResultKeyOptionsGiven checkKeyOptions(int which, llvm::StringRef type,
                                        const llvm::StringMap<Expr *> &options) const;

  std::optional<std::string> literalOf(const Expr *expr) const;

  struct KeyPairKind {
    int which;
    Expr *name;
  };

  struct KeyPairNumbers {
    Expr *modulus;
    Expr *exponent;
    Expr *curve;
    int has;
  };

  struct KeyEncoding {
    int format;
    int flags;
    Expr *type;
    Expr *cipher;
    Expr *passphrase;
  };

  llvm::ArrayRef<llvm::StringRef> pairTypeNames() const;
  std::optional<KeyPairKind> keyPairKind(CallExpr *call, llvm::SMLoc loc);
  bool readKeyPairOptions(CallExpr *call, llvm::StringRef shown, llvm::StringMap<Expr *> &options);
  std::optional<KeyPairNumbers> keyPairNumbers(llvm::SMLoc loc, int which,
                                               const llvm::StringMap<Expr *> &options);
  Expr *generateKeyCall(CallExpr *call, llvm::SMLoc loc, const KeyPairKind &kind,
                        const KeyPairNumbers &numbers);
  const Type *appendedEncoding(llvm::SMLoc loc, const llvm::StringMap<Expr *> &options,
                               bool isPrivate, std::vector<Expr *> &parts);
  bool readEncoding(Expr *encoding, llvm::StringRef option, bool isPrivate, KeyEncoding &chosen);
  bool readEncodingType(const Expr *encoding, llvm::StringRef option, bool isPrivate,
                        const llvm::StringMap<Expr *> &e, KeyEncoding &chosen);
  bool readEncodingCipher(const Expr *encoding, const llvm::StringMap<Expr *> &e,
                          KeyEncoding &chosen);
  const Type *encodedKeyType(int format) const;

  SemanticState &state;
  CryptoArgs *cryptoArgs = nullptr;
  CryptoErrors *cryptoErrors = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
