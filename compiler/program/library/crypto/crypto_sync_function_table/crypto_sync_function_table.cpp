#include "compiler/program/library/crypto/crypto_sync_function_table/crypto_sync_function_table.h"

namespace tz {

namespace {

const CryptoSyncFunctionInfo kCryptoSyncFunctions[] = {
    {
        .name = "constants",
        .function = CryptoSyncFunction::CONSTANTS,
    },
    {
        .name = "createHash",
        .function = CryptoSyncFunction::CREATE_HASH,
    },
    {
        .name = "createHmac",
        .function = CryptoSyncFunction::CREATE_HMAC,
    },
    {
        .name = "hash",
        .function = CryptoSyncFunction::HASH,
    },
    {
        .name = "randomBytes",
        .function = CryptoSyncFunction::RANDOM_BYTES,
    },
    {
        .name = "randomUUID",
        .function = CryptoSyncFunction::RANDOM_UUID,
    },
    {
        .name = "randomInt",
        .function = CryptoSyncFunction::RANDOM_INT,
    },
    {
        .name = "randomFillSync",
        .function = CryptoSyncFunction::RANDOM_FILL_SYNC,
    },
    {
        .name = "getRandomValues",
        .function = CryptoSyncFunction::GET_RANDOM_VALUES,
    },
    {
        .name = "timingSafeEqual",
        .function = CryptoSyncFunction::TIMING_SAFE_EQUAL,
    },
    {
        .name = "pbkdf2Sync",
        .function = CryptoSyncFunction::PBKDF2_SYNC,
    },
    {
        .name = "hkdfSync",
        .function = CryptoSyncFunction::HKDF_SYNC,
    },
    {
        .name = "scryptSync",
        .function = CryptoSyncFunction::SCRYPT_SYNC,
    },
    {
        .name = "argon2Sync",
        .function = CryptoSyncFunction::ARGON2_SYNC,
    },
    {
        .name = "createCipheriv",
        .function = CryptoSyncFunction::CREATE_CIPHERIV,
    },
    {
        .name = "createDecipheriv",
        .function = CryptoSyncFunction::CREATE_DECIPHERIV,
    },
    {
        .name = "getHashes",
        .function = CryptoSyncFunction::GET_HASHES,
    },
    {
        .name = "getCiphers",
        .function = CryptoSyncFunction::GET_CIPHERS,
    },
    {
        .name = "getCurves",
        .function = CryptoSyncFunction::GET_CURVES,
    },
    {
        .name = "createSecretKey",
        .function = CryptoSyncFunction::CREATE_SECRET_KEY,
    },
    {
        .name = "createPublicKey",
        .function = CryptoSyncFunction::CREATE_PUBLIC_KEY,
    },
    {
        .name = "createPrivateKey",
        .function = CryptoSyncFunction::CREATE_PRIVATE_KEY,
    },
    {
        .name = "generateKeyPairSync",
        .function = CryptoSyncFunction::GENERATE_KEY_PAIR_SYNC,
    },
    {
        .name = "generateKeySync",
        .function = CryptoSyncFunction::GENERATE_KEY_SYNC,
    },
    {
        .name = "sign",
        .function = CryptoSyncFunction::SIGN,
    },
    {
        .name = "verify",
        .function = CryptoSyncFunction::VERIFY,
    },
    {
        .name = "createSign",
        .function = CryptoSyncFunction::CREATE_SIGN,
    },
    {
        .name = "createVerify",
        .function = CryptoSyncFunction::CREATE_VERIFY,
    },
    {
        .name = "createECDH",
        .function = CryptoSyncFunction::CREATE_ECDH,
    },
    {
        .name = "diffieHellman",
        .function = CryptoSyncFunction::DIFFIE_HELLMAN,
    },
    {
        .name = "generatePrimeSync",
        .function = CryptoSyncFunction::GENERATE_PRIME_SYNC,
    },
    {
        .name = "checkPrimeSync",
        .function = CryptoSyncFunction::CHECK_PRIME_SYNC,
    },
};

}

std::optional<CryptoSyncFunction> CryptoSyncFunctionTable::find(llvm::StringRef name) {
  for (const CryptoSyncFunctionInfo &info : kCryptoSyncFunctions) {
    if (info.name == name) {
      return info.function;
    }
  }

  return std::nullopt;
}

}
