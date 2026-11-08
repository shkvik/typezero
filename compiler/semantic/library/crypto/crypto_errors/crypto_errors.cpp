#include "compiler/semantic/library/crypto/crypto_errors/crypto_errors.h"
#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"

using llvm::dyn_cast;

namespace tz {

CryptoErrors::CryptoErrors(SemanticState &owner)
    : state(owner) {}

void CryptoErrors::declare() {
  BuiltinModuleTypes types(this->state);
  if (types.declared("CreateHashError")) {
    return;
  }
  types.declare(BuiltinText::CRYPTO_ERRORS);
  for (llvm::StringRef name : CryptoErrorTypes::getTypeNames()) {
    types.exportFrom(NodeModule::Crypto, name);
  }
}

const Type *CryptoErrors::named(llvm::StringRef name) {
  return BuiltinModuleTypes(this->state).named(name);
}

const Type *CryptoErrors::ofCall(CryptoFn fn, llvm::ArrayRef<Expr *> args) {
  switch (fn) {
    case CryptoFn::HashNew:
      return this->named("CreateHashError");
    case CryptoFn::HashOnce:
    case CryptoFn::HashOnceText:
      return this->named("HashError");
    case CryptoFn::HashCopy:
      return this->named("HashCopyError");
    case CryptoFn::HmacNew:
      return this->named("CreateHmacError");
    case CryptoFn::Pbkdf2:
    case CryptoFn::Pbkdf2Async:
      return this->named("Pbkdf2Error");
    case CryptoFn::Hkdf:
    case CryptoFn::HkdfAsync:
      return this->named("HkdfError");
    case CryptoFn::Scrypt:
    case CryptoFn::ScryptAsync:
      return this->named("ScryptError");
    case CryptoFn::Argon2:
    case CryptoFn::Argon2Async:
      return this->named("Argon2Error");
    case CryptoFn::GeneratePrime:
    case CryptoFn::GeneratePrimeAsync:
      return this->named("GeneratePrimeError");
    case CryptoFn::CheckPrime:
    case CryptoFn::CheckPrimeAsync:
      return this->named("CheckPrimeError");
    case CryptoFn::CipherNew:
      return this->named(CryptoErrors::flag(args, 4) ? "CreateDecipherError" : "CreateCipherError");
    case CryptoFn::CipherFinal: {
      const Type *self = args.empty() ? nullptr : args[0]->type;
      bool decipher = self && self->crypto() == Type::Crypto::Decipheriv;
      return this->named(decipher ? "DecipherFinalError" : "CipherFinalError");
    }
    case CryptoFn::CipherGetAuthTag:
      return this->named("GetAuthTagError");
    case CryptoFn::CreateKey:
      return this->named(CryptoErrors::flag(args, 0) ? "CreatePrivateKeyError"
                                                     : "CreatePublicKeyError");
    case CryptoFn::GenerateKeyPair:
      return this->named("GenerateKeyPairError");
    case CryptoFn::KeyExportSecret:
    case CryptoFn::KeyExportPem:
    case CryptoFn::KeyExportDer:
    case CryptoFn::KeyExportJwk:
      return this->named("KeyExportError");
    case CryptoFn::Sign:
    case CryptoFn::SignAsync:
      return this->named("SignError");
    case CryptoFn::Verify:
    case CryptoFn::VerifyAsync:
      return this->named("VerifyError");
    case CryptoFn::SignNew:
      return this->named(CryptoErrors::flag(args, 1) ? "CreateVerifyError" : "CreateSignError");
    case CryptoFn::SignFinal:
      return this->named("SignerSignError");
    case CryptoFn::VerifyFinal:
      return this->named("VerifierVerifyError");
    case CryptoFn::EcdhNew:
      return this->named("CreateEcdhError");
    case CryptoFn::EcdhGetPublicKey:
      return this->named("EcdhGetPublicKeyError");
    case CryptoFn::EcdhGetPrivateKey:
      return this->named("EcdhGetPrivateKeyError");
    case CryptoFn::EcdhSetPrivateKey:
      return this->named("EcdhSetPrivateKeyError");
    case CryptoFn::EcdhComputeSecret:
      return this->named("EcdhComputeSecretError");
    case CryptoFn::DiffieHellman:
    case CryptoFn::DiffieHellmanAsync:
      return this->named("DiffieHellmanError");
    case CryptoFn::X509New:
      return this->named("ParseCertificateError");
    case CryptoFn::X509CheckPrivateKey:
      return this->named("CertificateCheckPrivateKeyError");
    case CryptoFn::X509Verify:
      return this->named("CertificateVerifyError");
    default:
      return this->named("CreateHashError");
  }
}

bool CryptoErrors::flag(llvm::ArrayRef<Expr *> args, size_t i) {
  const BoolLitExpr *literal = i < args.size() ? dyn_cast<BoolLitExpr>(args[i]) : nullptr;

  return literal != nullptr && literal->value;
}

}
