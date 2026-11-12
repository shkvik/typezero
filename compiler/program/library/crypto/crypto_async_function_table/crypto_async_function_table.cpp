#include "compiler/program/library/crypto/crypto_async_function_table/crypto_async_function_table.h"

namespace tz {

namespace {

const CryptoAsyncFunctionInfo kAsyncFunctions[] = {
    {.name = "pbkdf2", .syncName = "pbkdf2Sync", .module = NodeModule::Crypto},
    {.name = "scrypt", .syncName = "scryptSync", .module = NodeModule::Crypto},
    {.name = "argon2", .syncName = "argon2Sync", .module = NodeModule::Crypto},
    {.name = "hkdf", .syncName = "hkdfSync", .module = NodeModule::Crypto},
    {.name = "randomFill", .syncName = "randomFillSync", .module = NodeModule::Crypto},
    {.name = "generateKeyPair", .syncName = "generateKeyPairSync", .module = NodeModule::Crypto},
    {.name = "generateKey", .syncName = "generateKeySync", .module = NodeModule::Crypto},
    {.name = "generatePrime", .syncName = "generatePrimeSync", .module = NodeModule::Crypto},
    {.name = "checkPrime", .syncName = "checkPrimeSync", .module = NodeModule::Crypto},
    {.name = "randomBytesAsync", .syncName = "randomBytes", .module = NodeModule::TzCrypto},
    {.name = "randomIntAsync", .syncName = "randomInt", .module = NodeModule::TzCrypto},
    {.name = "signAsync", .syncName = "sign", .module = NodeModule::TzCrypto},
    {.name = "verifyAsync", .syncName = "verify", .module = NodeModule::TzCrypto},
    {.name = "diffieHellmanAsync", .syncName = "diffieHellman", .module = NodeModule::TzCrypto},
};

}

const CryptoAsyncFunctionInfo *CryptoAsyncFunctionTable::find(llvm::StringRef name) {
  for (const CryptoAsyncFunctionInfo &info : kAsyncFunctions) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
