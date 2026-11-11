#pragma once

#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class BuiltinModuleTypes;

class CryptoErrors {
public:
  CryptoErrors(SemanticState &owner, SemanticParts &all);

  const Type *named(llvm::StringRef name);
  const Type *ofCall(CryptoFn fn, llvm::ArrayRef<Expr *> args);

private:
  static bool flag(llvm::ArrayRef<Expr *> args, size_t i);
  static llvm::StringRef errorNameOf(CryptoFn fn, llvm::ArrayRef<Expr *> args);
  static bool isDecipher(llvm::ArrayRef<Expr *> args);

  SemanticState &state;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
};

}
