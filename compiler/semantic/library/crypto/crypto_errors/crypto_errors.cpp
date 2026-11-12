#include "compiler/semantic/library/crypto/crypto_errors/crypto_errors.h"
#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

using llvm::dyn_cast;

namespace tz {

CryptoErrors::CryptoErrors(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.builtinModuleTypes;
}

const Type *CryptoErrors::named(llvm::StringRef name) {
  return this->builtinModuleTypes->named(name);
}

const Type *CryptoErrors::ofCall(CryptoFn fn, llvm::ArrayRef<Expr *> args) {
  auto name = this->errorNameOf(fn, args);

  return this->named(name);
}

llvm::StringRef CryptoErrors::errorNameOf(CryptoFn fn, llvm::ArrayRef<Expr *> args) const {
  if (fn == CryptoFn::CipherNew) {
    return this->flag(args, 4) ? "CreateDecipherError" : "CreateCipherError";
  }
  if (fn == CryptoFn::CipherFinal) {
    return this->isDecipher(args) ? "DecipherFinalError" : "CipherFinalError";
  }
  if (fn == CryptoFn::CreateKey) {
    return this->flag(args, 0) ? "CreatePrivateKeyError" : "CreatePublicKeyError";
  }
  if (fn == CryptoFn::SignNew) {
    return this->flag(args, 1) ? "CreateVerifyError" : "CreateSignError";
  }
  auto plain = CryptoErrorTypes::callErrorName(fn);
  if (!plain.has_value()) {
    return "CreateHashError";
  }

  return *plain;
}

bool CryptoErrors::isDecipher(llvm::ArrayRef<Expr *> args) const {
  const Type *self = args.empty() ? nullptr : args[0]->type;

  return self != nullptr && self->crypto() == Type::Crypto::Decipheriv;
}

bool CryptoErrors::flag(llvm::ArrayRef<Expr *> args, size_t i) const {
  const BoolLitExpr *literal = i < args.size() ? dyn_cast<BoolLitExpr>(args[i]) : nullptr;

  return literal != nullptr && literal->value;
}

}
