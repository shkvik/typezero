#include "compiler/program/library/crypto/crypto_option_table/crypto_option_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const CryptoOptionInfo kRows[] = {
    {
        .name = "key",
        .value = CryptoOption::KEY,
    },
    {
        .name = "padding",
        .value = CryptoOption::PADDING,
    },
    {
        .name = "saltLength",
        .value = CryptoOption::SALT_LENGTH,
    },
    {
        .name = "dsaEncoding",
        .value = CryptoOption::DSA_ENCODING,
    },
    {
        .name = "outputLength",
        .value = CryptoOption::OUTPUT_LENGTH,
    },
    {
        .name = "disableEntropyCache",
        .value = CryptoOption::DISABLE_ENTROPY_CACHE,
    },
    {
        .name = "N",
        .value = CryptoOption::SCRYPT_N,
    },
    {
        .name = "r",
        .value = CryptoOption::SCRYPT_R,
    },
    {
        .name = "p",
        .value = CryptoOption::SCRYPT_P,
    },
    {
        .name = "maxmem",
        .value = CryptoOption::MAX_MEMORY,
    },
    {
        .name = "cost",
        .value = CryptoOption::COST,
    },
    {
        .name = "blockSize",
        .value = CryptoOption::BLOCK_SIZE,
    },
    {
        .name = "parallelization",
        .value = CryptoOption::PARALLELIZATION,
    },
    {
        .name = "message",
        .value = CryptoOption::MESSAGE,
    },
    {
        .name = "nonce",
        .value = CryptoOption::NONCE,
    },
    {
        .name = "parallelism",
        .value = CryptoOption::PARALLELISM,
    },
    {
        .name = "tagLength",
        .value = CryptoOption::TAG_LENGTH,
    },
    {
        .name = "memory",
        .value = CryptoOption::MEMORY,
    },
    {
        .name = "passes",
        .value = CryptoOption::PASSES,
    },
    {
        .name = "secret",
        .value = CryptoOption::SECRET,
    },
    {
        .name = "associatedData",
        .value = CryptoOption::ASSOCIATED_DATA,
    },
    {
        .name = "authTagLength",
        .value = CryptoOption::AUTH_TAG_LENGTH,
    },
    {
        .name = "format",
        .value = CryptoOption::FORMAT,
    },
    {
        .name = "type",
        .value = CryptoOption::TYPE,
    },
    {
        .name = "passphrase",
        .value = CryptoOption::PASSPHRASE,
    },
    {
        .name = "encoding",
        .value = CryptoOption::ENCODING,
    },
    {
        .name = "length",
        .value = CryptoOption::LENGTH,
    },
    {
        .name = "privateKey",
        .value = CryptoOption::PRIVATE_KEY,
    },
    {
        .name = "publicKey",
        .value = CryptoOption::PUBLIC_KEY,
    },
    {
        .name = "add",
        .value = CryptoOption::ADDEND,
    },
    {
        .name = "rem",
        .value = CryptoOption::REMAINDER,
    },
    {
        .name = "safe",
        .value = CryptoOption::SAFE,
    },
    {
        .name = "bigint",
        .value = CryptoOption::BIGINT,
    },
    {
        .name = "checks",
        .value = CryptoOption::CHECKS,
    },
    {
        .name = "plaintextLength",
        .value = CryptoOption::PLAINTEXT_LENGTH,
    },
    {
        .name = "cipher",
        .value = CryptoOption::CIPHER,
    },
    {
        .name = "subject",
        .value = CryptoOption::SUBJECT,
    },
    {
        .name = "wildcards",
        .value = CryptoOption::WILDCARDS,
    },
    {
        .name = "partialWildcards",
        .value = CryptoOption::PARTIAL_WILDCARDS,
    },
    {
        .name = "multiLabelWildcards",
        .value = CryptoOption::MULTI_LABEL_WILDCARDS,
    },
    {
        .name = "singleLabelSubdomains",
        .value = CryptoOption::SINGLE_LABEL_SUBDOMAINS,
    },
    {
        .name = "modulusLength",
        .value = CryptoOption::MODULUS_LENGTH,
    },
    {
        .name = "publicExponent",
        .value = CryptoOption::PUBLIC_EXPONENT,
    },
    {
        .name = "namedCurve",
        .value = CryptoOption::NAMED_CURVE,
    },
    {
        .name = "publicKeyEncoding",
        .value = CryptoOption::PUBLIC_KEY_ENCODING,
    },
    {
        .name = "privateKeyEncoding",
        .value = CryptoOption::PRIVATE_KEY_ENCODING,
    },
    {
        .name = "divisorLength",
        .value = CryptoOption::DIVISOR_LENGTH,
    },
    {
        .name = "hashAlgorithm",
        .value = CryptoOption::HASH_ALGORITHM,
    },
    {
        .name = "mgf1HashAlgorithm",
        .value = CryptoOption::MGF1_HASH_ALGORITHM,
    },
};

}

std::optional<CryptoOption> CryptoOptionTable::find(llvm::StringRef name) {
  for (const CryptoOptionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef CryptoOptionTable::getName(CryptoOption value) {
  for (const CryptoOptionInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("crypto option missing from the table");
}

}
