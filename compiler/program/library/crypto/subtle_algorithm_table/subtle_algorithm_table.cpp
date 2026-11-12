#include "compiler/program/library/crypto/subtle_algorithm_table/subtle_algorithm_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const std::vector<SubtleOperation> kAesCipherOperations = {
    SubtleOperation::GENERATE_KEY, SubtleOperation::IMPORT_KEY, SubtleOperation::ENCRYPT,
    SubtleOperation::DECRYPT, SubtleOperation::KEY_LENGTH};
const std::vector<SubtleOperation> kSignOperations = {
    SubtleOperation::GENERATE_KEY, SubtleOperation::IMPORT_KEY, SubtleOperation::SIGN,
    SubtleOperation::VERIFY};
const std::vector<SubtleOperation> kAgreeOperations = {
    SubtleOperation::GENERATE_KEY, SubtleOperation::IMPORT_KEY, SubtleOperation::DERIVE_BITS};
const std::vector<SubtleOperation> kKdfOperations = {
    SubtleOperation::IMPORT_KEY, SubtleOperation::DERIVE_BITS, SubtleOperation::KEY_LENGTH};
const std::vector<SubtleOperation> kDigestOperations = {SubtleOperation::DIGEST};

const std::vector<SubtleKeyUsage> kAesCipherUsages = {
    SubtleKeyUsage::ENCRYPT, SubtleKeyUsage::DECRYPT, SubtleKeyUsage::WRAP_KEY,
    SubtleKeyUsage::UNWRAP_KEY};
const std::vector<SubtleKeyUsage> kWrapUsages = {SubtleKeyUsage::WRAP_KEY,
                                                 SubtleKeyUsage::UNWRAP_KEY};
const std::vector<SubtleKeyUsage> kDeriveUsages = {SubtleKeyUsage::DERIVE_KEY,
                                                   SubtleKeyUsage::DERIVE_BITS};
const std::vector<SubtleKeyUsage> kSignUsages = {SubtleKeyUsage::SIGN};
const std::vector<SubtleKeyUsage> kVerifyUsages = {SubtleKeyUsage::VERIFY};

const SubtleAlgorithmInfo kAlgorithms[] = {
    {
        .name = "AES-CBC",
        .algorithm = SubtleAlgorithm::AES_CBC,
        .family = SubtleFamily::AES,
        .operations = kAesCipherOperations,
        .publicUsages = kAesCipherUsages,
        .privateUsages = kAesCipherUsages,
    },
    {
        .name = "AES-CTR",
        .algorithm = SubtleAlgorithm::AES_CTR,
        .family = SubtleFamily::AES,
        .operations = kAesCipherOperations,
        .publicUsages = kAesCipherUsages,
        .privateUsages = kAesCipherUsages,
    },
    {
        .name = "AES-GCM",
        .algorithm = SubtleAlgorithm::AES_GCM,
        .family = SubtleFamily::AES,
        .operations = kAesCipherOperations,
        .publicUsages = kAesCipherUsages,
        .privateUsages = kAesCipherUsages,
    },
    {
        .name = "AES-KW",
        .algorithm = SubtleAlgorithm::AES_KW,
        .family = SubtleFamily::AES,
        .operations = {SubtleOperation::GENERATE_KEY, SubtleOperation::IMPORT_KEY,
                       SubtleOperation::WRAP_KEY, SubtleOperation::UNWRAP_KEY,
                       SubtleOperation::KEY_LENGTH},
        .publicUsages = kWrapUsages,
        .privateUsages = kWrapUsages,
    },
    {
        .name = "ECDH",
        .algorithm = SubtleAlgorithm::ECDH,
        .family = SubtleFamily::EC,
        .operations = kAgreeOperations,
        .publicUsages = {},
        .privateUsages = kDeriveUsages,
    },
    {
        .name = "ECDSA",
        .algorithm = SubtleAlgorithm::ECDSA,
        .family = SubtleFamily::EC,
        .operations = kSignOperations,
        .publicUsages = kVerifyUsages,
        .privateUsages = kSignUsages,
    },
    {
        .name = "Ed25519",
        .algorithm = SubtleAlgorithm::ED25519,
        .family = SubtleFamily::OKP,
        .operations = kSignOperations,
        .publicUsages = kVerifyUsages,
        .privateUsages = kSignUsages,
    },
    {
        .name = "HKDF",
        .algorithm = SubtleAlgorithm::HKDF,
        .family = SubtleFamily::KDF,
        .operations = kKdfOperations,
        .publicUsages = kDeriveUsages,
        .privateUsages = kDeriveUsages,
    },
    {
        .name = "HMAC",
        .algorithm = SubtleAlgorithm::HMAC,
        .family = SubtleFamily::HMAC,
        .operations = {SubtleOperation::GENERATE_KEY, SubtleOperation::IMPORT_KEY,
                       SubtleOperation::SIGN, SubtleOperation::VERIFY, SubtleOperation::KEY_LENGTH},
        .publicUsages = {SubtleKeyUsage::SIGN, SubtleKeyUsage::VERIFY},
        .privateUsages = {SubtleKeyUsage::SIGN, SubtleKeyUsage::VERIFY},
    },
    {
        .name = "PBKDF2",
        .algorithm = SubtleAlgorithm::PBKDF2,
        .family = SubtleFamily::KDF,
        .operations = kKdfOperations,
        .publicUsages = kDeriveUsages,
        .privateUsages = kDeriveUsages,
    },
    {
        .name = "RSA-OAEP",
        .algorithm = SubtleAlgorithm::RSA_OAEP,
        .family = SubtleFamily::RSA,
        .operations = {SubtleOperation::GENERATE_KEY, SubtleOperation::IMPORT_KEY,
                       SubtleOperation::ENCRYPT, SubtleOperation::DECRYPT},
        .publicUsages = {SubtleKeyUsage::ENCRYPT, SubtleKeyUsage::WRAP_KEY},
        .privateUsages = {SubtleKeyUsage::DECRYPT, SubtleKeyUsage::UNWRAP_KEY},
    },
    {
        .name = "RSA-PSS",
        .algorithm = SubtleAlgorithm::RSA_PSS,
        .family = SubtleFamily::RSA,
        .operations = kSignOperations,
        .publicUsages = kVerifyUsages,
        .privateUsages = kSignUsages,
    },
    {
        .name = "RSASSA-PKCS1-v1_5",
        .algorithm = SubtleAlgorithm::RSASSA_PKCS1_V1_5,
        .family = SubtleFamily::RSA,
        .operations = kSignOperations,
        .publicUsages = kVerifyUsages,
        .privateUsages = kSignUsages,
    },
    {
        .name = "SHA-1",
        .algorithm = SubtleAlgorithm::SHA_1,
        .family = SubtleFamily::DIGEST,
        .operations = kDigestOperations,
        .publicUsages = {},
        .privateUsages = {},
    },
    {
        .name = "SHA-256",
        .algorithm = SubtleAlgorithm::SHA_256,
        .family = SubtleFamily::DIGEST,
        .operations = kDigestOperations,
        .publicUsages = {},
        .privateUsages = {},
    },
    {
        .name = "SHA-384",
        .algorithm = SubtleAlgorithm::SHA_384,
        .family = SubtleFamily::DIGEST,
        .operations = kDigestOperations,
        .publicUsages = {},
        .privateUsages = {},
    },
    {
        .name = "SHA-512",
        .algorithm = SubtleAlgorithm::SHA_512,
        .family = SubtleFamily::DIGEST,
        .operations = kDigestOperations,
        .publicUsages = {},
        .privateUsages = {},
    },
    {
        .name = "X25519",
        .algorithm = SubtleAlgorithm::X25519,
        .family = SubtleFamily::OKP,
        .operations = kAgreeOperations,
        .publicUsages = {},
        .privateUsages = kDeriveUsages,
    },
};

const llvm::StringLiteral kExperimentalNames[] = {
    "SHA3-256",  "SHA3-384",  "SHA3-512",   "CSHAKE128",         "CSHAKE256",
    "ED448",     "X448",      "AES-OCB",    "CHACHA20-POLY1305", "KMAC128",
    "KMAC256",   "ARGON2D",   "ARGON2I",    "ARGON2ID",          "ML-DSA-44",
    "ML-DSA-65", "ML-DSA-87", "ML-KEM-512", "ML-KEM-768",        "ML-KEM-1024"};

constexpr llvm::StringLiteral kShaPrefix = "sha";

}

const SubtleAlgorithmInfo *SubtleAlgorithmTable::findUpper(llvm::StringRef upper) {
  for (const SubtleAlgorithmInfo &info : kAlgorithms) {
    if (info.name.upper() == upper) {
      return &info;
    }
  }

  return nullptr;
}

const SubtleAlgorithmInfo *SubtleAlgorithmTable::findIn(SubtleOperation operation,
                                                        llvm::StringRef upper) {
  for (const SubtleAlgorithmInfo &info : kAlgorithms) {
    if (llvm::is_contained(info.operations, operation) && info.name.upper() == upper) {
      return &info;
    }
  }

  return nullptr;
}

std::vector<llvm::StringRef> SubtleAlgorithmTable::getNames(SubtleOperation operation) {
  auto names = std::vector<llvm::StringRef>();
  for (const SubtleAlgorithmInfo &info : kAlgorithms) {
    if (llvm::is_contained(info.operations, operation)) {
      names.push_back(info.name);
    }
  }

  return names;
}

bool SubtleAlgorithmTable::isExperimental(llvm::StringRef upper) {
  return llvm::is_contained(kExperimentalNames, upper);
}

bool SubtleAlgorithmTable::hasShaPrefix(llvm::StringRef name) {
  auto lower = name.lower();

  return llvm::StringRef(lower).starts_with(kShaPrefix);
}

}
