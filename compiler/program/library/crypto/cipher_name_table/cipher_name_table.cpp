#include "compiler/program/library/crypto/cipher_name_table/cipher_name_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kCipherNames[] = {
    "aes-128-cbc",
    "aes-128-cbc-hmac-sha1",
    "aes-128-cbc-hmac-sha256",
    "aes-128-ccm",
    "aes-128-cfb",
    "aes-128-cfb1",
    "aes-128-cfb8",
    "aes-128-ctr",
    "aes-128-ecb",
    "aes-128-gcm",
    "aes-128-ocb",
    "aes-128-ofb",
    "aes-128-xts",
    "aes-192-cbc",
    "aes-192-ccm",
    "aes-192-cfb",
    "aes-192-cfb1",
    "aes-192-cfb8",
    "aes-192-ctr",
    "aes-192-ecb",
    "aes-192-gcm",
    "aes-192-ocb",
    "aes-192-ofb",
    "aes-256-cbc",
    "aes-256-cbc-hmac-sha1",
    "aes-256-cbc-hmac-sha256",
    "aes-256-ccm",
    "aes-256-cfb",
    "aes-256-cfb1",
    "aes-256-cfb8",
    "aes-256-ctr",
    "aes-256-ecb",
    "aes-256-gcm",
    "aes-256-ocb",
    "aes-256-ofb",
    "aes-256-xts",
    "aes128",
    "aes128-wrap",
    "aes128-wrap-pad",
    "aes192",
    "aes192-wrap",
    "aes192-wrap-pad",
    "aes256",
    "aes256-wrap",
    "aes256-wrap-pad",
    "aria-128-cbc",
    "aria-128-ccm",
    "aria-128-cfb",
    "aria-128-cfb1",
    "aria-128-cfb8",
    "aria-128-ctr",
    "aria-128-ecb",
    "aria-128-gcm",
    "aria-128-ofb",
    "aria-192-cbc",
    "aria-192-ccm",
    "aria-192-cfb",
    "aria-192-cfb1",
    "aria-192-cfb8",
    "aria-192-ctr",
    "aria-192-ecb",
    "aria-192-gcm",
    "aria-192-ofb",
    "aria-256-cbc",
    "aria-256-ccm",
    "aria-256-cfb",
    "aria-256-cfb1",
    "aria-256-cfb8",
    "aria-256-ctr",
    "aria-256-ecb",
    "aria-256-gcm",
    "aria-256-ofb",
    "aria128",
    "aria192",
    "aria256",
    "camellia-128-cbc",
    "camellia-128-cfb",
    "camellia-128-cfb1",
    "camellia-128-cfb8",
    "camellia-128-ctr",
    "camellia-128-ecb",
    "camellia-128-ofb",
    "camellia-192-cbc",
    "camellia-192-cfb",
    "camellia-192-cfb1",
    "camellia-192-cfb8",
    "camellia-192-ctr",
    "camellia-192-ecb",
    "camellia-192-ofb",
    "camellia-256-cbc",
    "camellia-256-cfb",
    "camellia-256-cfb1",
    "camellia-256-cfb8",
    "camellia-256-ctr",
    "camellia-256-ecb",
    "camellia-256-ofb",
    "camellia128",
    "camellia192",
    "camellia256",
    "chacha20",
    "chacha20-poly1305",
    "des-ede",
    "des-ede-cbc",
    "des-ede-cfb",
    "des-ede-ecb",
    "des-ede-ofb",
    "des-ede3",
    "des-ede3-cbc",
    "des-ede3-cfb",
    "des-ede3-cfb1",
    "des-ede3-cfb8",
    "des-ede3-ecb",
    "des-ede3-ofb",
    "des3",
    "des3-wrap",
    "id-aes128-ccm",
    "id-aes128-gcm",
    "id-aes128-wrap",
    "id-aes128-wrap-pad",
    "id-aes192-ccm",
    "id-aes192-gcm",
    "id-aes192-wrap",
    "id-aes192-wrap-pad",
    "id-aes256-ccm",
    "id-aes256-gcm",
    "id-aes256-wrap",
    "id-aes256-wrap-pad",
    "id-smime-alg-cms3deswrap",
    "sm4",
    "sm4-cbc",
    "sm4-cfb",
    "sm4-ctr",
    "sm4-ecb",
    "sm4-ofb",
};

const CipherAliasInfo kAliases[] = {
    {.alias = "aes128", .name = "aes-128-cbc"},
    {.alias = "aes192", .name = "aes-192-cbc"},
    {.alias = "aes256", .name = "aes-256-cbc"},
    {.alias = "id-aes128-gcm", .name = "aes-128-gcm"},
    {.alias = "id-aes192-gcm", .name = "aes-192-gcm"},
    {.alias = "id-aes256-gcm", .name = "aes-256-gcm"},
};

const llvm::StringLiteral kKeptPrefixes[] = {"des-ede"};

constexpr llvm::StringLiteral kCcmMode = "ccm";

const TurnedOffCipherInfo kTurnedOff[] = {
    {.text = "bf", .match = TurnedOffMatch::PREFIX},
    {.text = "blowfish", .match = TurnedOffMatch::PREFIX},
    {.text = "cast", .match = TurnedOffMatch::PREFIX},
    {.text = "rc2", .match = TurnedOffMatch::PREFIX},
    {.text = "rc4", .match = TurnedOffMatch::PREFIX},
    {.text = "rc5", .match = TurnedOffMatch::PREFIX},
    {.text = "idea", .match = TurnedOffMatch::PREFIX},
    {.text = "seed", .match = TurnedOffMatch::PREFIX},
    {.text = "des", .match = TurnedOffMatch::EXACT},
    {.text = "desx", .match = TurnedOffMatch::EXACT},
    {.text = "desx-", .match = TurnedOffMatch::PREFIX},
    {.text = "des-", .match = TurnedOffMatch::PREFIX},
};

}

bool CipherNameTable::isKnown(llvm::StringRef name) {
  return llvm::is_contained(kCipherNames, name);
}

llvm::StringRef CipherNameTable::getCanonical(llvm::StringRef name) {
  for (const CipherAliasInfo &info : kAliases) {
    if (info.alias == name) {
      return info.name;
    }
  }

  return name;
}

bool CipherNameTable::isTurnedOff(llvm::StringRef name) {
  for (llvm::StringRef kept : kKeptPrefixes) {
    if (name.starts_with(kept)) {
      return false;
    }
  }
  for (const TurnedOffCipherInfo &info : kTurnedOff) {
    bool isExact = info.match == TurnedOffMatch::EXACT && name == info.text;
    bool isPrefix = info.match == TurnedOffMatch::PREFIX && name.starts_with(info.text);
    if (isExact || isPrefix) {
      return true;
    }
  }

  return false;
}

bool CipherNameTable::needsAuthTagLength(llvm::StringRef name) {
  auto lower = name.lower();

  return llvm::StringRef(lower).contains(kCcmMode);
}

}
