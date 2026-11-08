#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kCryptoTypeNames[] = {"CreateHashError",
                                                "HashError",
                                                "HashCopyError",
                                                "CreateHmacError",
                                                "Pbkdf2Error",
                                                "HkdfError",
                                                "ScryptError",
                                                "Argon2Error",
                                                "CreateCipherError",
                                                "CreateDecipherError",
                                                "CipherFinalError",
                                                "DecipherFinalError",
                                                "GetAuthTagError",
                                                "CreatePrivateKeyError",
                                                "CreatePublicKeyError",
                                                "GenerateKeyPairError",
                                                "KeyExportError",
                                                "SignError",
                                                "VerifyError",
                                                "CreateSignError",
                                                "CreateVerifyError",
                                                "SignerSignError",
                                                "VerifierVerifyError",
                                                "CreateEcdhError",
                                                "EcdhGetPublicKeyError",
                                                "EcdhGetPrivateKeyError",
                                                "EcdhSetPrivateKeyError",
                                                "EcdhComputeSecretError",
                                                "DiffieHellmanError",
                                                "ParseCertificateError",
                                                "CertificateCheckPrivateKeyError",
                                                "CertificateVerifyError",
                                                "GeneratePrimeError",
                                                "CheckPrimeError"};

}

bool CryptoErrorTypes::isCryptoError(const Type *type) {
  return llvm::is_contained(kCryptoTypeNames, type->name());
}

std::vector<llvm::StringRef> CryptoErrorTypes::getTypeNames() {
  std::vector<llvm::StringRef> names;
  for (llvm::StringLiteral name : kCryptoTypeNames) {
    names.push_back(name);
  }

  return names;
}

}
