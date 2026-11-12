#pragma once

#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class IndexProofs;
class BytesChecker;
class CryptoErrors;
class TypeRelations;
class ConstantFolder;
class ProcessChecker;
class ArgumentChecker;

struct CryptoDataKinds {
  bool isString;
  bool isKey;
  bool isArrayBuffer;
};

inline constexpr CryptoDataKinds kAnyCryptoData{
    .isString = true,
    .isKey = true,
    .isArrayBuffer = true,
};
inline constexpr CryptoDataKinds kTextOrBuffers{
    .isString = true,
    .isKey = false,
    .isArrayBuffer = true,
};
inline constexpr CryptoDataKinds kTextOrViews{
    .isString = true,
    .isKey = false,
    .isArrayBuffer = false,
};
inline constexpr CryptoDataKinds kBuffersOnly{
    .isString = false,
    .isKey = false,
    .isArrayBuffer = true,
};
inline constexpr CryptoDataKinds kKeysOrBuffers{
    .isString = false,
    .isKey = true,
    .isArrayBuffer = true,
};

enum class DigestUse {
  HASH,
  HMAC,
};

class CryptoArgs {
public:
  CryptoArgs(SemanticState &owner, SemanticParts &all);

  Expr *cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type);
  Expr *cryptoResult(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type);
  Expr *cryptoInt(llvm::SMLoc loc, int64_t value);
  Expr *cryptoDouble(llvm::SMLoc loc, double value);
  Expr *cryptoBool(llvm::SMLoc loc, bool value);
  Expr *cryptoString(llvm::SMLoc loc, llvm::StringRef text);
  Expr *cryptoUndefined(llvm::SMLoc loc);
  Expr *cryptoData(Expr *arg, llvm::StringRef what, CryptoDataKinds kinds);
  Expr *cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef functionName, int fallback);
  Expr *cryptoText(Expr *made, Expr *encoding);
  bool readCryptoOptions(Expr *arg, llvm::StringRef functionName,
                         llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  int cryptoChoice(Expr *arg, llvm::StringRef what, llvm::ArrayRef<llvm::StringRef> values);
  Expr *cryptoMd(Expr *arg, DigestUse use);
  Expr *cryptoCipher(Expr *arg);
  Expr *cryptoNumber(Expr *arg);
  std::optional<double> knownNumber(const Expr *arg) const;
  std::string numberText(double value) const;
  bool matchWhole(const Expr *arg, llvm::StringRef name, double least);
  bool matchCipherLengths(const Expr *cipher, const Expr *key, const Expr *iv);
  Expr *cryptoKey(Expr *arg, llvm::StringRef what);
  Expr *cryptoKeyObject(Expr *arg, llvm::StringRef what);
  bool appendSignKey(Expr *arg, llvm::StringRef functionName, bool priv, std::vector<Expr *> &args);
  bool rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef functionName,
                            llvm::StringRef hint);

private:
  bool isCryptoData(CryptoDataKinds kinds, const Type *candidate) const;
  bool isAnyLengthHash(llvm::StringRef name) const;
  int64_t cipherKeyBytes(llvm::StringRef name) const;
  std::optional<int64_t> bytesOf(const Expr *expr) const;
  std::optional<size_t> directMdIndex(llvm::StringRef name) const;
  using ResultKeyObjectType = Result<const Type *, CryptoArgsKeyObjectTypeError>;

  std::optional<double> plainNumber(const Expr *arg) const;
  ResultKeyObjectType checkKeyObjectType(const Type *type, llvm::StringRef what) const;
  std::optional<std::string> literalOf(const Expr *expr) const;

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BytesChecker *bytesChecker = nullptr;
  CryptoErrors *cryptoErrors = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
