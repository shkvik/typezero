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
  Expr *cryptoData(Expr *arg, llvm::StringRef what, bool strings, bool keys, bool buffers);
  Expr *cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef fn, int fallback);
  Expr *cryptoText(Expr *made, Expr *encoding);
  bool cryptoOptions(Expr *arg, llvm::StringRef fn, llvm::ArrayRef<llvm::StringRef> allowed,
                     llvm::StringMap<Expr *> &options);
  int cryptoChoice(Expr *arg, llvm::StringRef what, llvm::ArrayRef<llvm::StringRef> values);
  Expr *cryptoMd(Expr *arg, bool hmac);
  Expr *cryptoCipher(Expr *arg);
  Expr *cryptoNumber(Expr *arg);
  std::optional<double> knownNumber(const Expr *arg) const;
  static std::string numberText(double value);
  bool checkWhole(const Expr *arg, llvm::StringRef name, double least);
  bool checkCipherLengths(const Expr *cipher, const Expr *key, const Expr *iv);
  Expr *cryptoKey(Expr *arg, llvm::StringRef what);
  Expr *cryptoKeyObject(Expr *arg, llvm::StringRef what);
  bool cryptoSignKey(Expr *arg, llvm::StringRef fn, bool priv, std::vector<Expr *> &args);
  bool rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef fn, llvm::StringRef hint);

private:
  using ResultKeyObjectType = Result<const Type *, CryptoArgsKeyObjectTypeError>;

  static ResultKeyObjectType keyObjectType(const Type *type, llvm::StringRef what);
  static std::u16string utf16Of(llvm::StringRef text);
  static std::optional<std::string> literalOf(const Expr *e);

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
