#include "compiler/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

// A number written as a literal.
std::optional<double> numberLiteral(const Expr *e) {
  if (auto *lit = dyn_cast_or_null<IntLitExpr>(e)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast_or_null<FloatLitExpr>(e)) {
    return lit->value;
  }

  return std::nullopt;
}

// The property `name` of an object literal, or null.
const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name) {
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread && !prop.key && prop.name == name) {
      return &prop;
    }
  }

  return nullptr;
}

} // namespace

std::optional<std::string> stringLiteral(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  if (auto *lit = dyn_cast_or_null<StringLitExpr>(e)) {
    return utf8(lit->value);
  }
  if (auto *tmpl = dyn_cast_or_null<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return utf8(tmpl->quasis[0]);
  }

  return std::nullopt;
}

const std::vector<llvm::StringLiteral> &algorithmsOf(llvm::StringRef op) {
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

bool experimentalAlgorithm(llvm::StringRef upper) {
  static const llvm::StringLiteral names[] = {
      "SHA3-256",  "SHA3-384",  "SHA3-512",   "CSHAKE128",         "CSHAKE256",
      "ED448",     "X448",      "AES-OCB",    "CHACHA20-POLY1305", "KMAC128",
      "KMAC256",   "ARGON2D",   "ARGON2I",    "ARGON2ID",          "ML-DSA-44",
      "ML-DSA-65", "ML-DSA-87", "ML-KEM-512", "ML-KEM-768",        "ML-KEM-1024"};

  return llvm::is_contained(names, upper);
}

std::string quotedList(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef last) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    out += (i == 0                  ? "'"
            : i + 1 == names.size() ? "' " + last.str() + " '"
                                    : "', '") +
           names[i].str();
  }

  return out + "'";
}

llvm::StringRef familyOf(llvm::StringRef name) {
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

std::vector<llvm::StringLiteral> usagesOf(llvm::StringRef name, bool isPublic) {
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

std::vector<llvm::StringLiteral> formatsOf(llvm::StringRef family) {
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

Dictionary dictionaryOf(llvm::StringRef op, llvm::StringRef name) {
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

std::string verbOf(llvm::StringRef op) {
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

std::string exampleOf(llvm::StringRef member) {
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

  return "new Uint8Array(16)"; // salt, info
}

std::string literalAlgorithm(Diagnostics &diags, const Expr *arg, llvm::StringRef op,
                             llvm::StringRef fallback, bool &bad) {
  const ObjectLitExpr *lit = dyn_cast_or_null<ObjectLitExpr>(arg);
  const Expr *nameExpr = arg;
  if (lit) {
    if (lit->hasSpread()) {
      return "";
    }
    const ObjectLitExpr::Property *name = property(lit, "name");
    if (!name) {
      return ""; // the type check reports it
    }
    nameExpr = name->value;
  }
  std::optional<std::string> written = stringLiteral(nameExpr);
  if (!written) {
    return "";
  }
  std::string upper = llvm::StringRef(*written).upper();
  auto find = [&](llvm::StringRef in) -> std::string {
    for (llvm::StringRef known : algorithmsOf(in)) {
      if (known.upper() == upper) {
        return known.str();
      }
    }

    return "";
  };
  std::string name = find(op);
  if (name.empty() && !fallback.empty()) {
    name = find(fallback);
  }
  llvm::SMLoc loc = nameExpr->loc();
  if (name.empty()) {
    bad = true;
    std::string list = quotedList(algorithmsOf(op), "or");
    if (experimentalAlgorithm(upper)) {
      diags.error(loc, "'" + *written +
                           "' is an algorithm node 24 has as an experiment, which tz has not "
                           "yet; Web Crypto " +
                           verbOf(op) + " with " + list);
      return "";
    }
    bool known = false;
    for (llvm::StringRef other :
         {"digest", "generateKey", "importKey", "encrypt", "sign", "deriveBits", "wrapKey"}) {
      for (llvm::StringRef a : algorithmsOf(other)) {
        known |= a.upper() == upper;
      }
    }
    if (known) {
      diags.error(loc, "'" + *written + "' is no algorithm of " + op.str() + "; it " + verbOf(op) +
                           " with " + list);
    } else {
      diags.error(loc, "unknown algorithm '" + *written + "' of " + op.str() + "; Web Crypto " +
                           verbOf(op) + " with " + list);
    }
    return "";
  }
  // The members its dictionary needs, and the values written that node would reject.
  Dictionary dict = dictionaryOf(op, name);
  if (!lit) {
    if (!dict.required.empty()) {
      bad = true;
      diags.error(loc, "'" + name + "' of " + op.str() + " takes its parameters (" +
                           dict.name.str() + "): write an object, e.g. { name: \"" + name + "\", " +
                           dict.required[0].str() + ": " + exampleOf(dict.required[0]) + " }");
    }
    return bad ? "" : name;
  }
  for (llvm::StringRef member : dict.required) {
    if (!property(lit, member)) {
      bad = true;
      diags.error(lit->loc(), "the algorithm '" + name + "' of " + op.str() + " needs '" +
                                  member.str() + "' (" + dict.name.str() + "); add it, e.g. '" +
                                  member.str() + ": " + exampleOf(member) + "'");
      return "";
    }
  }
  if (const ObjectLitExpr::Property *hash = property(lit, "hash")) {
    const Expr *value = hash->value;
    if (auto *inner = dyn_cast<ObjectLitExpr>(value)) {
      if (const ObjectLitExpr::Property *n = property(inner, "name")) {
        value = n->value;
      }
    }
    if (std::optional<std::string> h = stringLiteral(value)) {
      llvm::StringRef hn(*h);
      bool sha = llvm::StringRef(hn.lower()).starts_with("sha");
      bool knownHash = false;
      for (llvm::StringRef a : algorithmsOf("digest")) {
        knownHash |= a.upper() == hn.upper();
      }
      if (!sha || !knownHash) {
        bad = true;
        diags.error(value->loc(),
                    sha || experimentalAlgorithm(hn.upper())
                        ? "unknown hash '" + *h + "' of " + dict.name.str() +
                              "; Web Crypto hashes with " + quotedList(algorithmsOf("digest"), "or")
                        : "'" + *h + "' is no SHA hash, which " + dict.name.str() +
                              " takes; use 'SHA-256', 'SHA-384', 'SHA-512' or 'SHA-1'");
        return "";
      }
    }
  }
  if (const ObjectLitExpr::Property *curve = property(lit, "namedCurve")) {
    if (std::optional<std::string> c = stringLiteral(curve->value);
        c && *c != "P-256" && *c != "P-384" && *c != "P-521") {
      bad = true;
      diags.error(curve->value->loc(),
                  "unknown namedCurve '" + *c + "'; Web Crypto has 'P-256', 'P-384' and 'P-521'");
      return "";
    }
  }
  if (name.rfind("AES-", 0) == 0 && (op == "generateKey" || op == "get key length")) {
    if (const ObjectLitExpr::Property *length = property(lit, "length")) {
      if (std::optional<double> n = numberLiteral(length->value);
          n && *n != 128 && *n != 192 && *n != 256) {
        bad = true;
        diags.error(length->value->loc(), "an AES key is 128, 192 or 256 bits long");
        return "";
      }
    }
  }
  if (name == "AES-GCM") {
    if (const ObjectLitExpr::Property *tag = property(lit, "tagLength")) {
      if (std::optional<double> n = numberLiteral(tag->value); n && *n != 32 && *n != 64 &&
                                                               *n != 96 && *n != 104 && *n != 112 &&
                                                               *n != 120 && *n != 128) {
        bad = true;
        diags.error(tag->value->loc(), "the tag of AES-GCM is 32, 64, 96, 104, 112, 120 or 128 "
                                       "bits long");
        return "";
      }
    }
  }

  return name;
}

} // namespace tz
