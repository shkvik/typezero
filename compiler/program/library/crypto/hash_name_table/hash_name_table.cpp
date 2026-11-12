#include "compiler/program/library/crypto/hash_name_table/hash_name_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const HashNameInfo kHashes[] = {
    {
        .name = "md5",
        .aliases = {"ssl3-md5", "1.2.840.113549.2.5", "rsa-md5", "md5withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha1",
        .aliases = {"sha-1", "ssl3-sha1", "1.3.14.3.2.26", "rsa-sha1", "sha1withrsaencryption",
                    "rsa-sha1-2"},
        .isAnyLength = false,
    },
    {
        .name = "sha224",
        .aliases = {"sha2-224", "sha-224", "2.16.840.1.101.3.4.2.4", "rsa-sha224",
                    "sha224withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha256",
        .aliases = {"sha2-256", "sha-256", "2.16.840.1.101.3.4.2.1", "rsa-sha256",
                    "sha256withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha384",
        .aliases = {"sha2-384", "sha-384", "2.16.840.1.101.3.4.2.2", "rsa-sha384",
                    "sha384withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha512",
        .aliases = {"sha2-512", "sha-512", "2.16.840.1.101.3.4.2.3", "rsa-sha512",
                    "sha512withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha512-224",
        .aliases = {"sha2-512/224", "sha-512/224", "2.16.840.1.101.3.4.2.5", "rsa-sha512/224",
                    "sha512-224withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha512-256",
        .aliases = {"sha2-512/256", "sha-512/256", "2.16.840.1.101.3.4.2.6", "rsa-sha512/256",
                    "sha512-256withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "sha3-224",
        .aliases = {"2.16.840.1.101.3.4.2.7", "rsa-sha3-224", "id-rsassa-pkcs1-v1_5-with-sha3-224"},
        .isAnyLength = false,
    },
    {
        .name = "sha3-256",
        .aliases = {"2.16.840.1.101.3.4.2.8", "rsa-sha3-256", "id-rsassa-pkcs1-v1_5-with-sha3-256"},
        .isAnyLength = false,
    },
    {
        .name = "sha3-384",
        .aliases = {"2.16.840.1.101.3.4.2.9", "rsa-sha3-384", "id-rsassa-pkcs1-v1_5-with-sha3-384"},
        .isAnyLength = false,
    },
    {
        .name = "sha3-512",
        .aliases = {"2.16.840.1.101.3.4.2.10", "rsa-sha3-512",
                    "id-rsassa-pkcs1-v1_5-with-sha3-512"},
        .isAnyLength = false,
    },
    {
        .name = "shake128",
        .aliases = {"shake-128", "2.16.840.1.101.3.4.2.11"},
        .isAnyLength = true,
    },
    {
        .name = "shake256",
        .aliases = {"shake-256", "2.16.840.1.101.3.4.2.12"},
        .isAnyLength = true,
    },
    {
        .name = "ripemd160",
        .aliases = {"ripemd-160", "ripemd", "rmd160", "1.3.36.3.2.1", "rsa-ripemd160",
                    "ripemd160withrsa"},
        .isAnyLength = false,
    },
    {
        .name = "sm3",
        .aliases = {"1.2.156.10197.1.401", "rsa-sm3", "sm3withrsaencryption"},
        .isAnyLength = false,
    },
    {
        .name = "md5-sha1",
        .aliases = {},
        .isAnyLength = false,
    },
    {
        .name = "blake2b512",
        .aliases = {"blake2b-512", "1.3.6.1.4.1.1722.12.2.1.16"},
        .isAnyLength = false,
    },
    {
        .name = "blake2s256",
        .aliases = {"blake2s-256", "1.3.6.1.4.1.1722.12.2.2.8"},
        .isAnyLength = false,
    },
};

const llvm::StringLiteral kTurnedOffHashes[] = {
    "md4",      "md2",         "mdc2",    "whirlpool",           "rsa-md4", "md4withrsaencryption",
    "rsa-mdc2", "mdc2withrsa", "rsa-md2", "md2withrsaencryption"};

}

const HashNameInfo *HashNameTable::find(llvm::StringRef name) {
  for (const HashNameInfo &info : kHashes) {
    if (info.name == name || llvm::is_contained(info.aliases, name)) {
      return &info;
    }
  }

  return nullptr;
}

bool HashNameTable::isTurnedOff(llvm::StringRef name) {
  return llvm::is_contained(kTurnedOffHashes, name);
}

}
