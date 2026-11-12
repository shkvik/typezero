#include "compiler/program/library/crypto/crypto_choice_table/crypto_choice_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const CryptoChoiceInfo kChoices[] = {
    {
        .choice = CryptoChoice::DSA_ENCODING,
        .values = {"der", "ieee-p1363"},
    },
    {
        .choice = CryptoChoice::ARGON2_ALGORITHM,
        .values = {"argon2d", "argon2i", "argon2id"},
    },
    {
        .choice = CryptoChoice::KEY_FORMAT,
        .values = {"pem", "der", "jwk"},
    },
    {
        .choice = CryptoChoice::EXPORT_FORMAT,
        .values = {"pem", "der", "jwk", "buffer"},
    },
    {
        .choice = CryptoChoice::KEY_ENCODING_TYPE,
        .values = {"pkcs1", "spki", "pkcs8", "sec1"},
    },
    {
        .choice = CryptoChoice::SECRET_KEY_TYPE,
        .values = {"hmac", "aes"},
    },
    {
        .choice = CryptoChoice::POINT_CONVERSION,
        .values = {"uncompressed", "compressed", "hybrid"},
    },
    {
        .choice = CryptoChoice::CHECK_SUBJECT,
        .values = {"default", "always", "never"},
    },
    {
        .choice = CryptoChoice::KEY_PAIR_TYPE,
        .values = {"rsa", "rsa-pss", "dsa", "ec", "ed25519", "ed448", "x25519", "x448"},
    },
};

}

llvm::ArrayRef<llvm::StringRef> CryptoChoiceTable::getValues(CryptoChoice choice) {
  for (const CryptoChoiceInfo &info : kChoices) {
    if (info.choice == choice) {
      return info.values;
    }
  }
  llvm_unreachable("crypto choice missing from the table");
}

bool CryptoChoiceTable::isBufferEncoding(llvm::StringRef text) {
  auto formats = CryptoChoiceTable::getValues(CryptoChoice::EXPORT_FORMAT);

  return text == formats[size_t(CryptoKeyFormat::BUFFER)];
}

}
