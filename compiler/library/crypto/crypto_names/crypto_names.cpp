#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const std::vector<HashNames> &hashNames() {
  static const std::vector<HashNames> table = {
      {"md5", {"ssl3-md5", "1.2.840.113549.2.5", "rsa-md5", "md5withrsaencryption"}},
      {"sha1",
       {"sha-1", "ssl3-sha1", "1.3.14.3.2.26", "rsa-sha1", "sha1withrsaencryption", "rsa-sha1-2"}},
      {"sha224",
       {"sha2-224", "sha-224", "2.16.840.1.101.3.4.2.4", "rsa-sha224", "sha224withrsaencryption"}},
      {"sha256",
       {"sha2-256", "sha-256", "2.16.840.1.101.3.4.2.1", "rsa-sha256", "sha256withrsaencryption"}},
      {"sha384",
       {"sha2-384", "sha-384", "2.16.840.1.101.3.4.2.2", "rsa-sha384", "sha384withrsaencryption"}},
      {"sha512",
       {"sha2-512", "sha-512", "2.16.840.1.101.3.4.2.3", "rsa-sha512", "sha512withrsaencryption"}},
      {"sha512-224",
       {"sha2-512/224", "sha-512/224", "2.16.840.1.101.3.4.2.5", "rsa-sha512/224",
        "sha512-224withrsaencryption"}},
      {"sha512-256",
       {"sha2-512/256", "sha-512/256", "2.16.840.1.101.3.4.2.6", "rsa-sha512/256",
        "sha512-256withrsaencryption"}},
      {"sha3-224",
       {"2.16.840.1.101.3.4.2.7", "rsa-sha3-224", "id-rsassa-pkcs1-v1_5-with-sha3-224"}},
      {"sha3-256",
       {"2.16.840.1.101.3.4.2.8", "rsa-sha3-256", "id-rsassa-pkcs1-v1_5-with-sha3-256"}},
      {"sha3-384",
       {"2.16.840.1.101.3.4.2.9", "rsa-sha3-384", "id-rsassa-pkcs1-v1_5-with-sha3-384"}},
      {"sha3-512",
       {"2.16.840.1.101.3.4.2.10", "rsa-sha3-512", "id-rsassa-pkcs1-v1_5-with-sha3-512"}},
      {"shake128", {"shake-128", "2.16.840.1.101.3.4.2.11"}},
      {"shake256", {"shake-256", "2.16.840.1.101.3.4.2.12"}},
      {"ripemd160",
       {"ripemd-160", "ripemd", "rmd160", "1.3.36.3.2.1", "rsa-ripemd160", "ripemd160withrsa"}},
      {"sm3", {"1.2.156.10197.1.401", "rsa-sm3", "sm3withrsaencryption"}},
      {"md5-sha1", {}},
      {"blake2b512", {"blake2b-512", "1.3.6.1.4.1.1722.12.2.1.16"}},
      {"blake2s256", {"blake2s-256", "1.3.6.1.4.1.1722.12.2.2.8"}},
  };

  return table;
}

bool turnedOffHash(llvm::StringRef name) {
  static const llvm::StringLiteral names[] = {"md4",      "md2",
                                              "mdc2",     "whirlpool",
                                              "rsa-md4",  "md4withrsaencryption",
                                              "rsa-mdc2", "mdc2withrsa",
                                              "rsa-md2",  "md2withrsaencryption"};

  return llvm::is_contained(names, name);
}

bool turnedOffCipher(llvm::StringRef name) {
  for (llvm::StringRef prefix : {"bf", "blowfish", "cast", "rc2", "rc4", "rc5", "idea", "seed"}) {
    if (name.starts_with(prefix)) {
      return true;
    }
  }

  return name == "des" || name == "desx" || name.starts_with("desx-") ||
         (name.starts_with("des-") && !name.starts_with("des-ede"));
}

llvm::StringRef methodsOf(Crypto kind) {
  switch (kind) {
  case Crypto::Hash:
    return "'update', 'digest' and 'copy'";
  case Crypto::Hmac:
    return "'update' and 'digest'";
  case Crypto::Cipheriv:
    return "'update', 'final', 'setAAD', 'getAuthTag' and 'setAutoPadding'";
  case Crypto::Decipheriv:
    return "'update', 'final', 'setAAD', 'setAuthTag' and 'setAutoPadding'";
  case Crypto::KeyObject:
    return "'export' and 'equals', and the properties 'type', 'asymmetricKeyType' and "
           "'symmetricKeySize'";
  case Crypto::Sign:
    return "'update' and 'sign'";
  case Crypto::Verify:
    return "'update' and 'verify'";
  case Crypto::Ecdh:
    return "'generateKeys', 'computeSecret', 'getPublicKey', 'getPrivateKey' and "
           "'setPrivateKey'";
  case Crypto::X509:
    return "'checkHost', 'checkEmail', 'checkIP', 'checkIssued', 'checkPrivateKey', 'verify', "
           "'toString' and 'toJSON', and the properties 'subject', 'issuer', 'subjectAltName', "
           "'infoAccess', 'validFrom', 'validTo', 'validFromDate', 'validToDate', "
           "'serialNumber', 'fingerprint', 'fingerprint256', 'fingerprint512', 'keyUsage', "
           "'ca', 'raw' and 'publicKey'";
  case Crypto::None:
    break;
  }

  return "";
}

std::string cryptoExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string what = "'" + name.str() + "' of '" + path.str() + "'";
  if (llvm::is_contained(kFunctions, name) || llvm::is_contained(kClasses, name) ||
      name == "constants") {
    return "";
  }
  for (const auto &[form, sync] : kCallbackForms) {
    if (form == name) {
      return what +
             " takes a callback, which tz does not have (docs/crypto.md, section 5); "
             "import '" +
             sync.str() + "' and use what it returns";
    }
  }
  if (name == "webcrypto") {
    return what + " is the global crypto of Web Crypto, which needs no import: use "
                  "'crypto.subtle', 'crypto.getRandomValues' and 'crypto.randomUUID'";
  }
  if (name == "subtle") {
    return what + " is the global crypto.subtle, which needs no import: use 'crypto.subtle'";
  }
  if (llvm::is_contained(kExports, name)) {
    return what + " is not supported yet";
  }

  return "'" + path.str() + "' has no export named '" + name.str() + "'";
}

} // namespace tz
