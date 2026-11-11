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

const CryptoCallErrorInfo kCryptoCallErrors[] = {
    {
        .fn = CryptoFn::HashNew,
        .name = "CreateHashError",
    },
    {
        .fn = CryptoFn::HashOnce,
        .name = "HashError",
    },
    {
        .fn = CryptoFn::HashOnceText,
        .name = "HashError",
    },
    {
        .fn = CryptoFn::HashCopy,
        .name = "HashCopyError",
    },
    {
        .fn = CryptoFn::HmacNew,
        .name = "CreateHmacError",
    },
    {
        .fn = CryptoFn::Pbkdf2,
        .name = "Pbkdf2Error",
    },
    {
        .fn = CryptoFn::Pbkdf2Async,
        .name = "Pbkdf2Error",
    },
    {
        .fn = CryptoFn::Hkdf,
        .name = "HkdfError",
    },
    {
        .fn = CryptoFn::HkdfAsync,
        .name = "HkdfError",
    },
    {
        .fn = CryptoFn::Scrypt,
        .name = "ScryptError",
    },
    {
        .fn = CryptoFn::ScryptAsync,
        .name = "ScryptError",
    },
    {
        .fn = CryptoFn::Argon2,
        .name = "Argon2Error",
    },
    {
        .fn = CryptoFn::Argon2Async,
        .name = "Argon2Error",
    },
    {
        .fn = CryptoFn::GeneratePrime,
        .name = "GeneratePrimeError",
    },
    {
        .fn = CryptoFn::GeneratePrimeAsync,
        .name = "GeneratePrimeError",
    },
    {
        .fn = CryptoFn::CheckPrime,
        .name = "CheckPrimeError",
    },
    {
        .fn = CryptoFn::CheckPrimeAsync,
        .name = "CheckPrimeError",
    },
    {
        .fn = CryptoFn::CipherGetAuthTag,
        .name = "GetAuthTagError",
    },
    {
        .fn = CryptoFn::GenerateKeyPair,
        .name = "GenerateKeyPairError",
    },
    {
        .fn = CryptoFn::KeyExportSecret,
        .name = "KeyExportError",
    },
    {
        .fn = CryptoFn::KeyExportPem,
        .name = "KeyExportError",
    },
    {
        .fn = CryptoFn::KeyExportDer,
        .name = "KeyExportError",
    },
    {
        .fn = CryptoFn::KeyExportJwk,
        .name = "KeyExportError",
    },
    {
        .fn = CryptoFn::Sign,
        .name = "SignError",
    },
    {
        .fn = CryptoFn::SignAsync,
        .name = "SignError",
    },
    {
        .fn = CryptoFn::Verify,
        .name = "VerifyError",
    },
    {
        .fn = CryptoFn::VerifyAsync,
        .name = "VerifyError",
    },
    {
        .fn = CryptoFn::SignFinal,
        .name = "SignerSignError",
    },
    {
        .fn = CryptoFn::VerifyFinal,
        .name = "VerifierVerifyError",
    },
    {
        .fn = CryptoFn::EcdhNew,
        .name = "CreateEcdhError",
    },
    {
        .fn = CryptoFn::EcdhGetPublicKey,
        .name = "EcdhGetPublicKeyError",
    },
    {
        .fn = CryptoFn::EcdhGetPrivateKey,
        .name = "EcdhGetPrivateKeyError",
    },
    {
        .fn = CryptoFn::EcdhSetPrivateKey,
        .name = "EcdhSetPrivateKeyError",
    },
    {
        .fn = CryptoFn::EcdhComputeSecret,
        .name = "EcdhComputeSecretError",
    },
    {
        .fn = CryptoFn::DiffieHellman,
        .name = "DiffieHellmanError",
    },
    {
        .fn = CryptoFn::DiffieHellmanAsync,
        .name = "DiffieHellmanError",
    },
    {
        .fn = CryptoFn::X509New,
        .name = "ParseCertificateError",
    },
    {
        .fn = CryptoFn::X509CheckPrivateKey,
        .name = "CertificateCheckPrivateKeyError",
    },
    {
        .fn = CryptoFn::X509Verify,
        .name = "CertificateVerifyError",
    },
};
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

std::optional<llvm::StringRef> CryptoErrorTypes::callErrorName(CryptoFn fn) {
  for (const CryptoCallErrorInfo &info : kCryptoCallErrors) {
    if (info.fn == fn) {
      return llvm::StringRef(info.name);
    }
  }

  return std::nullopt;
}

}
