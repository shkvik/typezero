#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::optional<std::string> SubtleAlgorithms::stringLiteral(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  if (auto *lit = dyn_cast_or_null<StringLitExpr>(e)) {
    return ToUtf8::toUtf8(lit->value);
  }
  if (auto *tmpl = dyn_cast_or_null<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return ToUtf8::toUtf8(tmpl->quasis[0]);
  }

  return std::nullopt;
}

const std::vector<llvm::StringLiteral> &SubtleAlgorithms::algorithmsOf(llvm::StringRef op) {
  static const std::vector<llvm::StringLiteral> digest = {"SHA-1", "SHA-256", "SHA-384", "SHA-512"};
  static const std::vector<llvm::StringLiteral> generate = {
      "AES-CBC",  "AES-CTR", "AES-GCM",           "AES-KW", "ECDH", "ECDSA", "Ed25519", "HMAC",
      "RSA-OAEP", "RSA-PSS", "RSASSA-PKCS1-v1_5", "X25519"};
  static const std::vector<llvm::StringLiteral> import = {
      "AES-CBC",           "AES-CTR", "AES-GCM", "AES-KW", "ECDH",     "ECDSA",
      "Ed25519",           "HKDF",    "HMAC",    "PBKDF2", "RSA-OAEP", "RSA-PSS",
      "RSASSA-PKCS1-v1_5", "X25519"};
  static const std::vector<llvm::StringLiteral> cipher = {"AES-CBC", "AES-CTR", "AES-GCM",
                                                          "RSA-OAEP"};
  static const std::vector<llvm::StringLiteral> sign = {"ECDSA", "Ed25519", "HMAC", "RSA-PSS",
                                                        "RSASSA-PKCS1-v1_5"};
  static const std::vector<llvm::StringLiteral> derive = {"ECDH", "HKDF", "PBKDF2", "X25519"};
  static const std::vector<llvm::StringLiteral> wrap = {"AES-KW"};
  static const std::vector<llvm::StringLiteral> length = {"AES-CBC", "AES-CTR", "AES-GCM", "AES-KW",
                                                          "HKDF",    "HMAC",    "PBKDF2"};
  static const std::vector<llvm::StringLiteral> none;
  if (op == "digest") {
    return digest;
  }
  if (op == "generateKey") {
    return generate;
  }
  if (op == "importKey") {
    return import;
  }
  if (op == "encrypt" || op == "decrypt") {
    return cipher;
  }
  if (op == "sign" || op == "verify") {
    return sign;
  }
  if (op == "deriveBits") {
    return derive;
  }
  if (op == "wrapKey" || op == "unwrapKey") {
    return wrap;
  }
  if (op == "get key length") {
    return length;
  }

  return none;
}

bool SubtleAlgorithms::experimentalAlgorithm(llvm::StringRef upper) {
  static const llvm::StringLiteral names[] = {
      "SHA3-256",  "SHA3-384",  "SHA3-512",   "CSHAKE128",         "CSHAKE256",
      "ED448",     "X448",      "AES-OCB",    "CHACHA20-POLY1305", "KMAC128",
      "KMAC256",   "ARGON2D",   "ARGON2I",    "ARGON2ID",          "ML-DSA-44",
      "ML-DSA-65", "ML-DSA-87", "ML-KEM-512", "ML-KEM-768",        "ML-KEM-1024"};

  return llvm::is_contained(names, upper);
}

std::string SubtleAlgorithms::quotedList(llvm::ArrayRef<llvm::StringLiteral> names,
                                         llvm::StringRef last) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    out += (i == 0                  ? "'"
            : i + 1 == names.size() ? "' " + last.str() + " '"
                                    : "', '") +
           names[i].str();
  }

  return out + "'";
}

llvm::StringRef SubtleAlgorithms::familyOf(llvm::StringRef name) {
  if (name == "HMAC") {
    return "Hmac";
  }
  if (name.starts_with("AES-")) {
    return "Aes";
  }
  if (name == "PBKDF2" || name == "HKDF") {
    return "Kdf";
  }
  if (name == "ECDSA" || name == "ECDH") {
    return "Ec";
  }
  if (name.starts_with("RSA")) {
    return "Rsa";
  }

  return "Okp";
}

std::vector<llvm::StringLiteral> SubtleAlgorithms::usagesOf(llvm::StringRef name, bool isPublic) {
  if (name == "HMAC") {
    return {"sign", "verify"};
  }
  if (name == "AES-KW") {
    return {"wrapKey", "unwrapKey"};
  }
  if (name.starts_with("AES-")) {
    return {"encrypt", "decrypt", "wrapKey", "unwrapKey"};
  }
  if (name == "PBKDF2" || name == "HKDF") {
    return {"deriveKey", "deriveBits"};
  }
  if (name == "ECDH" || name == "X25519") {
    return isPublic ? std::vector<llvm::StringLiteral>{}
                    : std::vector<llvm::StringLiteral>{"deriveKey", "deriveBits"};
  }
  if (name == "RSA-OAEP") {
    return isPublic ? std::vector<llvm::StringLiteral>{"encrypt", "wrapKey"}
                    : std::vector<llvm::StringLiteral>{"decrypt", "unwrapKey"};
  }

  return isPublic ? std::vector<llvm::StringLiteral>{"verify"}
                  : std::vector<llvm::StringLiteral>{"sign"};
}

std::vector<llvm::StringLiteral> SubtleAlgorithms::formatsOf(llvm::StringRef family) {
  if (family == "Hmac" || family == "Aes") {
    return {"raw", "jwk"};
  }
  if (family == "Kdf") {
    return {"raw"};
  }
  if (family == "Rsa") {
    return {"spki", "pkcs8", "jwk"};
  }

  return {"spki", "pkcs8", "jwk", "raw"};
}

Dictionary SubtleAlgorithms::dictionaryOf(llvm::StringRef op, llvm::StringRef name) {
  bool aes = name.starts_with("AES-"), rsa = name.starts_with("RSA");
  bool ec = name == "ECDSA" || name == "ECDH";
  if (op == "generateKey") {
    if (aes) {
      return {"AesKeyGenParams", {"length"}};
    }
    if (ec) {
      return {"EcKeyGenParams", {"namedCurve"}};
    }
    if (name == "HMAC") {
      return {"HmacKeyGenParams", {"hash"}};
    }
    if (rsa) {
      return {"RsaHashedKeyGenParams", {"hash", "modulusLength", "publicExponent"}};
    }
  } else if (op == "importKey") {
    if (ec) {
      return {"EcKeyImportParams", {"namedCurve"}};
    }
    if (name == "HMAC") {
      return {"HmacImportParams", {"hash"}};
    }
    if (rsa) {
      return {"RsaHashedImportParams", {"hash"}};
    }
  } else if (op == "encrypt" || op == "decrypt") {
    if (name == "AES-CBC") {
      return {"AesCbcParams", {"iv"}};
    }
    if (name == "AES-CTR") {
      return {"AesCtrParams", {"counter", "length"}};
    }
    if (name == "AES-GCM") {
      return {"AeadParams", {"iv"}};
    }
    return {"RsaOaepParams", {}};
  } else if (op == "sign" || op == "verify") {
    if (name == "ECDSA") {
      return {"EcdsaParams", {"hash"}};
    }
    if (name == "RSA-PSS") {
      return {"RsaPssParams", {"saltLength"}};
    }
  } else if (op == "deriveBits") {
    if (name == "HKDF") {
      return {"HkdfParams", {"hash", "info", "salt"}};
    }
    if (name == "PBKDF2") {
      return {"Pbkdf2Params", {"hash", "iterations", "salt"}};
    }
    return {"EcdhKeyDeriveParams", {"public"}};
  } else if (op == "get key length") {
    if (aes) {
      return {"AesDerivedKeyParams", {"length"}};
    }
    if (name == "HMAC") {
      return {"HmacImportParams", {"hash"}};
    }
  }

  return {"", {}};
}

std::string SubtleAlgorithms::verbOf(llvm::StringRef op) {
  if (op == "digest") {
    return "hashes";
  }
  if (op == "generateKey") {
    return "makes keys";
  }
  if (op == "importKey") {
    return "imports keys";
  }
  if (op == "encrypt") {
    return "encrypts";
  }
  if (op == "decrypt") {
    return "decrypts";
  }
  if (op == "sign") {
    return "signs";
  }
  if (op == "verify") {
    return "verifies";
  }
  if (op == "deriveBits") {
    return "derives";
  }
  if (op == "wrapKey") {
    return "wraps keys";
  }
  if (op == "unwrapKey") {
    return "unwraps keys";
  }

  return "derives keys";
}

std::string SubtleAlgorithms::exampleOf(llvm::StringRef member) {
  if (member == "hash") {
    return "\"SHA-256\"";
  }
  if (member == "length") {
    return "256";
  }
  if (member == "namedCurve") {
    return "\"P-256\"";
  }
  if (member == "iv") {
    return "new Uint8Array(16)";
  }
  if (member == "counter") {
    return "new Uint8Array(16)";
  }
  if (member == "saltLength") {
    return "32";
  }
  if (member == "modulusLength") {
    return "2048";
  }
  if (member == "publicExponent") {
    return "new Uint8Array([1, 0, 1])";
  }
  if (member == "iterations") {
    return "600000";
  }
  if (member == "public") {
    return "theirPublicKey";
  }

  return "new Uint8Array(16)";
}

}
