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
  bool flag(llvm::ArrayRef<Expr *> args, size_t i) const;
  llvm::StringRef errorNameOf(CryptoFn fn, llvm::ArrayRef<Expr *> args) const;
  bool isDecipher(llvm::ArrayRef<Expr *> args) const;

  SemanticState &state;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
};

}
