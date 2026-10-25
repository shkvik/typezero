#pragma once

#include "compiler/check/sema/sema.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

// The names node:crypto knows: hashes, ciphers, functions, classes and exports.

// The names of each hash of the direct layer that node takes (lower case): those of OpenSSL's
// provider (SHA2-256, SHA-256, its OID), of its objects, and the names of signatures that
// getHashes() lists ("RSA-SHA256").
struct HashNames {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> aliases;
};

const std::vector<HashNames> &hashNames();

// What getCiphers() lists in node 24 with OpenSSL 3.5: every cipher name node takes.
inline const llvm::StringLiteral kCipherNames[] = {"aes-128-cbc",
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
                                            "sm4-ofb"};

// The names of node:crypto in node 24, and what tz has of them.
inline const llvm::StringLiteral kFunctions[] = {"createHash",
                                          "createHmac",
                                          "hash",
                                          "randomBytes",
                                          "randomUUID",
                                          "randomInt",
                                          "randomFillSync",
                                          "getRandomValues",
                                          "timingSafeEqual",
                                          "pbkdf2Sync",
                                          "scryptSync",
                                          "argon2Sync",
                                          "hkdfSync",
                                          "createCipheriv",
                                          "createDecipheriv",
                                          "getHashes",
                                          "getCiphers",
                                          "getCurves",
                                          "createSecretKey",
                                          "createPublicKey",
                                          "createPrivateKey",
                                          "generateKeyPairSync",
                                          "generateKeySync",
                                          "sign",
                                          "verify",
                                          "createSign",
                                          "createVerify",
                                          "createECDH",
                                          "diffieHellman"};

inline const llvm::StringLiteral kClasses[] = {"Hash", "Hmac",   "Cipheriv", "Decipheriv",     "KeyObject",
                                        "Sign", "Verify", "ECDH",     "X509Certificate"};

// The functions that take a callback, with their Sync form.
inline const std::pair<llvm::StringLiteral, llvm::StringLiteral> kCallbackForms[] = {
    {"pbkdf2", "pbkdf2Sync"},          {"scrypt", "scryptSync"},
    {"argon2", "argon2Sync"},          {"hkdf", "hkdfSync"},
    {"randomFill", "randomFillSync"},  {"generateKeyPair", "generateKeyPairSync"},
    {"generateKey", "generateKeySync"}};

inline const llvm::StringLiteral kExports[] = {"Certificate",
                                        "DiffieHellman",
                                        "DiffieHellmanGroup",
                                        "checkPrime",
                                        "checkPrimeSync",
                                        "createDiffieHellman",
                                        "createDiffieHellmanGroup",
                                        "decapsulate",
                                        "encapsulate",
                                        "generatePrime",
                                        "generatePrimeSync",
                                        "getCipherInfo",
                                        "getDiffieHellman",
                                        "getFips",
                                        "privateDecrypt",
                                        "privateEncrypt",
                                        "publicDecrypt",
                                        "publicEncrypt",
                                        "secureHeapUsed",
                                        "setEngine",
                                        "setFips"};

// The hashes node 24 turns off, which fail in node with ERR_OSSL_EVP_UNSUPPORTED.
bool turnedOffHash(llvm::StringRef name);

bool turnedOffCipher(llvm::StringRef name);

// The methods each class has, for the message about a method it has not.
llvm::StringRef methodsOf(Crypto kind);

std::string cryptoExportProblem(llvm::StringRef name, llvm::StringRef path);

} // namespace tz
