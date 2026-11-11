#include "compiler/program/library/crypto/subtle_dictionary_table/subtle_dictionary_table.h"

namespace tz {

namespace {

const SubtleDictionaryInfo kSubtleDictionaries[] = {
    {
        .operation = "generateKey",
        .algorithm = "AES-",
        .prefix = true,
        .dictionary = {"AesKeyGenParams", {"length"}},
    },
    {
        .operation = "generateKey",
        .algorithm = "ECDSA",
        .prefix = false,
        .dictionary = {"EcKeyGenParams", {"namedCurve"}},
    },
    {
        .operation = "generateKey",
        .algorithm = "ECDH",
        .prefix = false,
        .dictionary = {"EcKeyGenParams", {"namedCurve"}},
    },
    {
        .operation = "generateKey",
        .algorithm = "HMAC",
        .prefix = false,
        .dictionary = {"HmacKeyGenParams", {"hash"}},
    },
    {
        .operation = "generateKey",
        .algorithm = "RSA",
        .prefix = true,
        .dictionary = {"RsaHashedKeyGenParams", {"hash", "modulusLength", "publicExponent"}},
    },
    {
        .operation = "importKey",
        .algorithm = "ECDSA",
        .prefix = false,
        .dictionary = {"EcKeyImportParams", {"namedCurve"}},
    },
    {
        .operation = "importKey",
        .algorithm = "ECDH",
        .prefix = false,
        .dictionary = {"EcKeyImportParams", {"namedCurve"}},
    },
    {
        .operation = "importKey",
        .algorithm = "HMAC",
        .prefix = false,
        .dictionary = {"HmacImportParams", {"hash"}},
    },
    {
        .operation = "importKey",
        .algorithm = "RSA",
        .prefix = true,
        .dictionary = {"RsaHashedImportParams", {"hash"}},
    },
    {
        .operation = "encrypt",
        .algorithm = "AES-CBC",
        .prefix = false,
        .dictionary = {"AesCbcParams", {"iv"}},
    },
    {
        .operation = "encrypt",
        .algorithm = "AES-CTR",
        .prefix = false,
        .dictionary = {"AesCtrParams", {"counter", "length"}},
    },
    {
        .operation = "encrypt",
        .algorithm = "AES-GCM",
        .prefix = false,
        .dictionary = {"AeadParams", {"iv"}},
    },
    {
        .operation = "encrypt",
        .algorithm = "",
        .prefix = true,
        .dictionary = {"RsaOaepParams", {}},
    },
    {
        .operation = "decrypt",
        .algorithm = "AES-CBC",
        .prefix = false,
        .dictionary = {"AesCbcParams", {"iv"}},
    },
    {
        .operation = "decrypt",
        .algorithm = "AES-CTR",
        .prefix = false,
        .dictionary = {"AesCtrParams", {"counter", "length"}},
    },
    {
        .operation = "decrypt",
        .algorithm = "AES-GCM",
        .prefix = false,
        .dictionary = {"AeadParams", {"iv"}},
    },
    {
        .operation = "decrypt",
        .algorithm = "",
        .prefix = true,
        .dictionary = {"RsaOaepParams", {}},
    },
    {
        .operation = "sign",
        .algorithm = "ECDSA",
        .prefix = false,
        .dictionary = {"EcdsaParams", {"hash"}},
    },
    {
        .operation = "sign",
        .algorithm = "RSA-PSS",
        .prefix = false,
        .dictionary = {"RsaPssParams", {"saltLength"}},
    },
    {
        .operation = "verify",
        .algorithm = "ECDSA",
        .prefix = false,
        .dictionary = {"EcdsaParams", {"hash"}},
    },
    {
        .operation = "verify",
        .algorithm = "RSA-PSS",
        .prefix = false,
        .dictionary = {"RsaPssParams", {"saltLength"}},
    },
    {
        .operation = "deriveBits",
        .algorithm = "HKDF",
        .prefix = false,
        .dictionary = {"HkdfParams", {"hash", "info", "salt"}},
    },
    {
        .operation = "deriveBits",
        .algorithm = "PBKDF2",
        .prefix = false,
        .dictionary = {"Pbkdf2Params", {"hash", "iterations", "salt"}},
    },
    {
        .operation = "deriveBits",
        .algorithm = "",
        .prefix = true,
        .dictionary = {"EcdhKeyDeriveParams", {"public"}},
    },
    {
        .operation = "get key length",
        .algorithm = "AES-",
        .prefix = true,
        .dictionary = {"AesDerivedKeyParams", {"length"}},
    },
    {
        .operation = "get key length",
        .algorithm = "HMAC",
        .prefix = false,
        .dictionary = {"HmacImportParams", {"hash"}},
    },
};

}

const SubtleDictionaryInfo *SubtleDictionaryTable::find(llvm::StringRef operation,
                                                        llvm::StringRef algorithm) {
  for (const SubtleDictionaryInfo &info : kSubtleDictionaries) {
    if (info.operation != operation) {
      continue;
    }
    bool matches =
        info.prefix ? algorithm.starts_with(info.algorithm) : algorithm == info.algorithm;
    if (matches) {
      return &info;
    }
  }

  return nullptr;
}

}
