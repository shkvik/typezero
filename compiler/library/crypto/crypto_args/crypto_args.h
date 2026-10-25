#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The arguments of the functions of node:crypto.
class CryptoArgs {
public:
  explicit CryptoArgs(Checker &checker);

  // The parts of those calls: the call of the runtime function `fn` with
  // `args`, literals for the arguments Checker fills in, and the checks of the
  // arguments (an error leaves the type error, or gives null or false).
  Expr *cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type);
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
  Expr *cryptoKey(Expr *arg, llvm::StringRef what);
  Expr *cryptoKeyObject(Expr *arg, llvm::StringRef what);
  bool cryptoSignKey(Expr *arg, llvm::StringRef fn, bool priv, std::vector<Expr *> &args);
  bool rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef fn, llvm::StringRef hint);

private:
  Checker &checker_;
};

} // namespace tz
