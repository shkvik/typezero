#include "compiler/program/library/crypto/crypto_class_table/crypto_class_table.h"

namespace tz {

namespace {

const CryptoClassInfo kClasses[] = {
    {.name = "Hash", .crypto = Type::Crypto::Hash},
    {.name = "Hmac", .crypto = Type::Crypto::Hmac},
    {.name = "Cipheriv", .crypto = Type::Crypto::Cipheriv},
    {.name = "Decipheriv", .crypto = Type::Crypto::Decipheriv},
    {.name = "KeyObject", .crypto = Type::Crypto::KeyObject},
    {.name = "Sign", .crypto = Type::Crypto::Sign},
    {.name = "Verify", .crypto = Type::Crypto::Verify},
    {.name = "ECDH", .crypto = Type::Crypto::Ecdh},
    {.name = "X509Certificate", .crypto = Type::Crypto::X509},
};

}

std::optional<Type::Crypto> CryptoClassTable::find(llvm::StringRef name) {
  for (const CryptoClassInfo &info : kClasses) {
    if (info.name == name) {
      return info.crypto;
    }
  }

  return std::nullopt;
}

}
