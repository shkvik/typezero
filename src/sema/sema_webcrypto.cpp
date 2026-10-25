// Web Crypto (docs/crypto.md, step 4): what the compiler adds to crypto.subtle, which tz has
// written in tz (src/lib/webcrypto.ts):
//
//   - the intrinsics __tz_subtle_* of that code, the jobs of the pool that runtime/crypto_subtle*.c
//     does, each a call of Builtin::Crypto that gives a promise (crypto_api.h);
//   - `crypto.subtle` and `globalThis.crypto`;
//   - the calls of the program on its SubtleCrypto. An algorithm, a format or a usage written in
//     the call is checked here, as tsc would and further: an algorithm node does not know, one
//     that the operation does not take, a usage the algorithm does not allow are errors where node
//     would reject at run time. A key made by an algorithm written in the call goes to the
//     function of its family, so that the program brings only that code (and the direct layer of
//     OpenSSL for HMAC, AES and the KDFs), and generateKey and exportKey give the exact type.

#include "sema/crypto_api.h"
#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

//===----------------------------------------------------------------------===//
// The intrinsics of src/lib/webcrypto.ts
//===----------------------------------------------------------------------===//

namespace {

// The kinds of their arguments: m a hash and c a cipher, written as a literal of the direct
// layer; d bytes (a BufferSource or a secret KeyObject); o a KeyObject; b a boolean, i an int,
// f a double, s a string. Their results: B an ArrayBuffer, U one or undefined (a failed job),
// I an int, K a KeyObject or undefined.
struct SubtleIntrinsic {
  llvm::StringLiteral name;
  CryptoFn fn;
  llvm::StringLiteral params;
  char result;
};

const SubtleIntrinsic kSubtleIntrinsics[] = {
    {"__tz_subtle_digest", CryptoFn::SubtleDigest, "md", 'B'},
    {"__tz_subtle_hmac", CryptoFn::SubtleHmac, "mdd", 'B'},
    {"__tz_subtle_hmacVerify", CryptoFn::SubtleHmacVerify, "mddd", 'I'},
    {"__tz_subtle_pbkdf2", CryptoFn::SubtlePbkdf2, "mddff", 'B'},
    {"__tz_subtle_hkdf", CryptoFn::SubtleHkdf, "mdddf", 'U'},
    {"__tz_subtle_aes", CryptoFn::SubtleAes, "cdbddiid", 'U'},
    {"__tz_subtle_aesKw", CryptoFn::SubtleAesKw, "dbd", 'U'},
    {"__tz_subtle_sign", CryptoFn::SubtleSign, "oisid", 'U'},
    {"__tz_subtle_verify", CryptoFn::SubtleVerify, "oisidd", 'I'},
    {"__tz_subtle_rsaOaep", CryptoFn::SubtleRsaOaep, "osdbd", 'U'},
    {"__tz_subtle_derive", CryptoFn::SubtleDerive, "oo", 'U'},
    {"__tz_subtle_generate", CryptoFn::SubtleGenerate, "iffs", 'K'},
};

} // namespace

Expr *Sema::checkSubtleIntrinsic(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  const SubtleIntrinsic *intrinsic = llvm::find_if(
      kSubtleIntrinsics, [&](const SubtleIntrinsic &i) { return i.name == name->name; });
  if (intrinsic == std::end(kSubtleIntrinsics) ||
      call->args.size() != intrinsic->params.size() || call->hasSpread()) {
    diags_.error(name->loc(), "'" + name->name + "' is no intrinsic of tz, or is called wrongly");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  std::vector<Expr *> args;
  bool ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    Expr *checked = nullptr;
    switch (intrinsic->params[i]) {
    case 'm':
      checked = cryptoMd(arg, /*hmac=*/true);
      break;
    case 'c':
      checked = cryptoCipher(arg);
      break;
    case 'd':
      checked = cryptoData(arg, "data", /*strings=*/false, /*keys=*/true, /*buffers=*/true);
      break;
    case 'o':
      checked = cryptoKeyObject(arg, "key");
      break;
    case 'b':
      checked = checkArg(arg, Type::boolTy());
      break;
    case 'i':
      checked = checkArg(arg, Type::intTy());
      break;
    case 'f':
      checked = checkArg(arg, Type::doubleTy());
      break;
    default:
      checked = checkArg(arg, Type::string());
      break;
    }
    // The hash and the cipher are the direct layer's: written as literals.
    char kind = intrinsic->params[i];
    if (checked && (kind == 'm' || kind == 'c') && !isa<IntLitExpr>(checked)) {
      diags_.error(arg->loc(), "the hash or the cipher of '" + name->name +
                                   "' is a literal of the direct layer");
      checked = nullptr;
    }
    if (!checked || checked->type->isError()) {
      ok = false;
      checked = checked ? checked : arg;
    }
    args.push_back(checked);
  }
  if (!ok)
    return call;
  const Type *buffer = Type::bytesType(Type::Bytes::ArrayBuffer);
  const Type *value = intrinsic->result == 'B'   ? buffer
                      : intrinsic->result == 'U' ? Type::optional(buffer)
                      : intrinsic->result == 'I' ? Type::intTy()
                                                 : Type::optional(Type::cryptoType(Type::Crypto::KeyObject));
  return cryptoCall(call, intrinsic->fn, std::move(args), Type::promise(value));
}

//===----------------------------------------------------------------------===//
// crypto.subtle
//===----------------------------------------------------------------------===//

bool Sema::isSubtleClass(const Type *type, llvm::StringRef name) const {
  const ClassDecl *cls = type ? classOf(type) : nullptr;
  return cls && cls->library && cls->name == name && cls == libraryClass(name);
}

Expr *Sema::subtleObject(MemberExpr *member) {
  // The loader imports it for every file that says `crypto.subtle`.
  if (!lookup(kSubtleName).var) {
    diags_.error(member->loc(), "'crypto.subtle' is not available here");
    member->type = Type::error();
    return member;
  }
  auto *name = ctx_.make<NameExpr>(member->loc(), kSubtleName);
  return checkExpr(name);
}

//===----------------------------------------------------------------------===//
// The calls of the program on crypto.subtle
//===----------------------------------------------------------------------===//

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

// A string written as a literal.
std::optional<std::string> stringLiteral(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    if (!conv->isImplicit())
      break;
    e = conv->operand;
  }
  if (auto *lit = dyn_cast_or_null<StringLitExpr>(e))
    return utf8(lit->value);
  if (auto *tmpl = dyn_cast_or_null<TemplateExpr>(e); tmpl && tmpl->exprs.empty())
    return utf8(tmpl->quasis[0]);
  return std::nullopt;
}

// A number written as a literal.
std::optional<double> numberLiteral(const Expr *e) {
  if (auto *lit = dyn_cast_or_null<IntLitExpr>(e))
    return double(lit->value);
  if (auto *lit = dyn_cast_or_null<FloatLitExpr>(e))
    return lit->value;
  return std::nullopt;
}

// The property `name` of an object literal, or null.
const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name) {
  for (const ObjectLitExpr::Property &prop : lit->props)
    if (!prop.spread && !prop.key && prop.name == name)
      return &prop;
  return nullptr;
}

// What node 24 registers for each operation, as src/lib/webcrypto.ts has them.
const std::vector<llvm::StringLiteral> &algorithmsOf(llvm::StringRef op) {
  static const std::vector<llvm::StringLiteral> digest = {"SHA-1", "SHA-256", "SHA-384",
                                                          "SHA-512"};
  static const std::vector<llvm::StringLiteral> generate = {
      "AES-CBC", "AES-CTR",  "AES-GCM", "AES-KW",  "ECDH", "ECDSA", "Ed25519",
      "HMAC",    "RSA-OAEP", "RSA-PSS", "RSASSA-PKCS1-v1_5", "X25519"};
  static const std::vector<llvm::StringLiteral> import = {
      "AES-CBC", "AES-CTR", "AES-GCM",  "AES-KW",  "ECDH",
      "ECDSA",   "Ed25519", "HKDF",     "HMAC",    "PBKDF2",
      "RSA-OAEP", "RSA-PSS", "RSASSA-PKCS1-v1_5", "X25519"};
  static const std::vector<llvm::StringLiteral> cipher = {"AES-CBC", "AES-CTR", "AES-GCM",
                                                          "RSA-OAEP"};
  static const std::vector<llvm::StringLiteral> sign = {"ECDSA", "Ed25519", "HMAC", "RSA-PSS",
                                                        "RSASSA-PKCS1-v1_5"};
  static const std::vector<llvm::StringLiteral> derive = {"ECDH", "HKDF", "PBKDF2", "X25519"};
  static const std::vector<llvm::StringLiteral> wrap = {"AES-KW"};
  static const std::vector<llvm::StringLiteral> length = {"AES-CBC", "AES-CTR", "AES-GCM",
                                                          "AES-KW",  "HKDF",    "HMAC",
                                                          "PBKDF2"};
  static const std::vector<llvm::StringLiteral> none;
  if (op == "digest")
    return digest;
  if (op == "generateKey")
    return generate;
  if (op == "importKey")
    return import;
  if (op == "encrypt" || op == "decrypt")
    return cipher;
  if (op == "sign" || op == "verify")
    return sign;
  if (op == "deriveBits")
    return derive;
  if (op == "wrapKey" || op == "unwrapKey")
    return wrap;
  if (op == "get key length")
    return length;
  return none;
}

// The algorithms node 24 has as experiments, which tz has not yet.
bool experimentalAlgorithm(llvm::StringRef upper) {
  static const llvm::StringLiteral names[] = {
      "SHA3-256",  "SHA3-384",  "SHA3-512",  "CSHAKE128", "CSHAKE256",  "ED448",
      "X448",      "AES-OCB",   "CHACHA20-POLY1305", "KMAC128", "KMAC256", "ARGON2D",
      "ARGON2I",   "ARGON2ID",  "ML-DSA-44", "ML-DSA-65", "ML-DSA-87",  "ML-KEM-512",
      "ML-KEM-768", "ML-KEM-1024"};
  return llvm::is_contained(names, upper);
}

std::string quotedList(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef last) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i)
    out += (i == 0 ? "'" : i + 1 == names.size() ? "' " + last.str() + " '" : "', '") +
           names[i].str();
  return out + "'";
}

// The family of an algorithm: what makes and uses its keys (src/lib/webcrypto.ts).
llvm::StringRef familyOf(llvm::StringRef name) {
  if (name == "HMAC")
    return "Hmac";
  if (name.starts_with("AES-"))
    return "Aes";
  if (name == "PBKDF2" || name == "HKDF")
    return "Kdf";
  if (name == "ECDSA" || name == "ECDH")
    return "Ec";
  if (name.starts_with("RSA"))
    return "Rsa";
  return "Okp";
}

// The usages a key of `name` takes: of its public key, of its private (or secret) one.
std::vector<llvm::StringLiteral> usagesOf(llvm::StringRef name, bool isPublic) {
  if (name == "HMAC")
    return {"sign", "verify"};
  if (name == "AES-KW")
    return {"wrapKey", "unwrapKey"};
  if (name.starts_with("AES-"))
    return {"encrypt", "decrypt", "wrapKey", "unwrapKey"};
  if (name == "PBKDF2" || name == "HKDF")
    return {"deriveKey", "deriveBits"};
  if (name == "ECDH" || name == "X25519")
    return isPublic ? std::vector<llvm::StringLiteral>{}
                    : std::vector<llvm::StringLiteral>{"deriveKey", "deriveBits"};
  if (name == "RSA-OAEP")
    return isPublic ? std::vector<llvm::StringLiteral>{"encrypt", "wrapKey"}
                    : std::vector<llvm::StringLiteral>{"decrypt", "unwrapKey"};
  return isPublic ? std::vector<llvm::StringLiteral>{"verify"}
                  : std::vector<llvm::StringLiteral>{"sign"};
}

// The formats importKey takes for a family.
std::vector<llvm::StringLiteral> formatsOf(llvm::StringRef family) {
  if (family == "Hmac" || family == "Aes")
    return {"raw", "jwk"};
  if (family == "Kdf")
    return {"raw"};
  if (family == "Rsa")
    return {"spki", "pkcs8", "jwk"};
  return {"spki", "pkcs8", "jwk", "raw"};
}

// The dictionary of Web IDL and its members a call must give, for an algorithm of an
// operation; empty for none.
struct Dictionary {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> required;
};

Dictionary dictionaryOf(llvm::StringRef op, llvm::StringRef name) {
  bool aes = name.starts_with("AES-"), rsa = name.starts_with("RSA");
  bool ec = name == "ECDSA" || name == "ECDH";
  if (op == "generateKey") {
    if (aes)
      return {"AesKeyGenParams", {"length"}};
    if (ec)
      return {"EcKeyGenParams", {"namedCurve"}};
    if (name == "HMAC")
      return {"HmacKeyGenParams", {"hash"}};
    if (rsa)
      return {"RsaHashedKeyGenParams", {"hash", "modulusLength", "publicExponent"}};
  } else if (op == "importKey") {
    if (ec)
      return {"EcKeyImportParams", {"namedCurve"}};
    if (name == "HMAC")
      return {"HmacImportParams", {"hash"}};
    if (rsa)
      return {"RsaHashedImportParams", {"hash"}};
  } else if (op == "encrypt" || op == "decrypt") {
    if (name == "AES-CBC")
      return {"AesCbcParams", {"iv"}};
    if (name == "AES-CTR")
      return {"AesCtrParams", {"counter", "length"}};
    if (name == "AES-GCM")
      return {"AeadParams", {"iv"}};
    return {"RsaOaepParams", {}};
  } else if (op == "sign" || op == "verify") {
    if (name == "ECDSA")
      return {"EcdsaParams", {"hash"}};
    if (name == "RSA-PSS")
      return {"RsaPssParams", {"saltLength"}};
  } else if (op == "deriveBits") {
    if (name == "HKDF")
      return {"HkdfParams", {"hash", "info", "salt"}};
    if (name == "PBKDF2")
      return {"Pbkdf2Params", {"hash", "iterations", "salt"}};
    return {"EcdhKeyDeriveParams", {"public"}};
  } else if (op == "get key length") {
    if (aes)
      return {"AesDerivedKeyParams", {"length"}};
    if (name == "HMAC")
      return {"HmacImportParams", {"hash"}};
  }
  return {"", {}};
}

// How an operation is said in a message.
std::string verbOf(llvm::StringRef op) {
  if (op == "digest")
    return "hashes";
  if (op == "generateKey")
    return "makes keys";
  if (op == "importKey")
    return "imports keys";
  if (op == "encrypt")
    return "encrypts";
  if (op == "decrypt")
    return "decrypts";
  if (op == "sign")
    return "signs";
  if (op == "verify")
    return "verifies";
  if (op == "deriveBits")
    return "derives";
  if (op == "wrapKey")
    return "wraps keys";
  if (op == "unwrapKey")
    return "unwraps keys";
  return "derives keys";
}

// A value of a member of a dictionary, for the hint of an error.
std::string exampleOf(llvm::StringRef member) {
  if (member == "hash")
    return "\"SHA-256\"";
  if (member == "length")
    return "256";
  if (member == "namedCurve")
    return "\"P-256\"";
  if (member == "iv")
    return "new Uint8Array(16)";
  if (member == "counter")
    return "new Uint8Array(16)";
  if (member == "saltLength")
    return "32";
  if (member == "modulusLength")
    return "2048";
  if (member == "publicExponent")
    return "new Uint8Array([1, 0, 1])";
  if (member == "iterations")
    return "600000";
  if (member == "public")
    return "theirPublicKey";
  return "new Uint8Array(16)"; // salt, info
}

const llvm::StringLiteral kUsages[] = {"encrypt",    "decrypt", "sign",    "verify",
                                       "deriveKey", "deriveBits", "wrapKey", "unwrapKey"};

} // namespace

// An algorithm written in the call: its canonical name, or "" when the call does not write one
// (or after an error, `bad`). `op` is what node normalizes it for; `fallback` the operation
// node tries after it (encrypt for wrapKey).
static std::string literalAlgorithm(Diagnostics &diags, const Expr *arg, llvm::StringRef op,
                                    llvm::StringRef fallback, bool &bad) {
  const ObjectLitExpr *lit = dyn_cast_or_null<ObjectLitExpr>(arg);
  const Expr *nameExpr = arg;
  if (lit) {
    if (lit->hasSpread())
      return "";
    const ObjectLitExpr::Property *name = property(lit, "name");
    if (!name)
      return ""; // the type check reports it
    nameExpr = name->value;
  }
  std::optional<std::string> written = stringLiteral(nameExpr);
  if (!written)
    return "";
  std::string upper = llvm::StringRef(*written).upper();
  auto find = [&](llvm::StringRef in) -> std::string {
    for (llvm::StringRef known : algorithmsOf(in))
      if (known.upper() == upper)
        return known.str();
    return "";
  };
  std::string name = find(op);
  if (name.empty() && !fallback.empty())
    name = find(fallback);
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
    for (llvm::StringRef other : {"digest", "generateKey", "importKey", "encrypt", "sign",
                                  "deriveBits", "wrapKey"})
      for (llvm::StringRef a : algorithmsOf(other))
        known |= a.upper() == upper;
    if (known)
      diags.error(loc, "'" + *written + "' is no algorithm of " + op.str() + "; it " +
                           verbOf(op) + " with " + list);
    else
      diags.error(loc, "unknown algorithm '" + *written + "' of " + op.str() + "; Web Crypto " +
                           verbOf(op) + " with " + list);
    return "";
  }
  // The members its dictionary needs, and the values written that node would reject.
  Dictionary dict = dictionaryOf(op, name);
  if (!lit) {
    if (!dict.required.empty()) {
      bad = true;
      diags.error(loc, "'" + name + "' of " + op.str() + " takes its parameters (" +
                           dict.name.str() + "): write an object, e.g. { name: \"" + name +
                           "\", " + dict.required[0].str() + ": " +
                           exampleOf(dict.required[0]) + " }");
    }
    return bad ? "" : name;
  }
  for (llvm::StringRef member : dict.required)
    if (!property(lit, member)) {
      bad = true;
      diags.error(lit->loc(), "the algorithm '" + name + "' of " + op.str() + " needs '" +
                                  member.str() + "' (" + dict.name.str() + "); add it, e.g. '" +
                                  member.str() + ": " + exampleOf(member) + "'");
      return "";
    }
  if (const ObjectLitExpr::Property *hash = property(lit, "hash")) {
    const Expr *value = hash->value;
    if (auto *inner = dyn_cast<ObjectLitExpr>(value))
      if (const ObjectLitExpr::Property *n = property(inner, "name"))
        value = n->value;
    if (std::optional<std::string> h = stringLiteral(value)) {
      llvm::StringRef hn(*h);
      bool sha = llvm::StringRef(hn.lower()).starts_with("sha");
      bool knownHash = false;
      for (llvm::StringRef a : algorithmsOf("digest"))
        knownHash |= a.upper() == hn.upper();
      if (!sha || !knownHash) {
        bad = true;
        diags.error(value->loc(),
                    sha || experimentalAlgorithm(hn.upper())
                        ? "unknown hash '" + *h + "' of " + dict.name.str() +
                              "; Web Crypto hashes with " +
                              quotedList(algorithmsOf("digest"), "or")
                        : "'" + *h + "' is no SHA hash, which " + dict.name.str() +
                              " takes; use 'SHA-256', 'SHA-384', 'SHA-512' or 'SHA-1'");
        return "";
      }
    }
  }
  if (const ObjectLitExpr::Property *curve = property(lit, "namedCurve"))
    if (std::optional<std::string> c = stringLiteral(curve->value);
        c && *c != "P-256" && *c != "P-384" && *c != "P-521") {
      bad = true;
      diags.error(curve->value->loc(), "unknown namedCurve '" + *c +
                                           "'; Web Crypto has 'P-256', 'P-384' and 'P-521'");
      return "";
    }
  if (name.rfind("AES-", 0) == 0 && (op == "generateKey" || op == "get key length"))
    if (const ObjectLitExpr::Property *length = property(lit, "length"))
      if (std::optional<double> n = numberLiteral(length->value);
          n && *n != 128 && *n != 192 && *n != 256) {
        bad = true;
        diags.error(length->value->loc(), "an AES key is 128, 192 or 256 bits long");
        return "";
      }
  if (name == "AES-GCM")
    if (const ObjectLitExpr::Property *tag = property(lit, "tagLength"))
      if (std::optional<double> n = numberLiteral(tag->value);
          n && *n != 32 && *n != 64 && *n != 96 && *n != 104 && *n != 112 && *n != 120 &&
          *n != 128) {
        bad = true;
        diags.error(tag->value->loc(), "the tag of AES-GCM is 32, 64, 96, 104, 112, 120 or 128 "
                                       "bits long");
        return "";
      }
  return name;
}

Expr *Sema::checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef method = member->name;
  std::vector<Expr *> &args = call->args;
  if (call->hasSpread())
    return nullptr;
  auto arg = [&](size_t i) -> Expr * { return i < args.size() ? args[i] : nullptr; };
  // An algorithm: as written, or the literal a constant holds (`const algorithm = { name:
  // "HMAC", hash: "SHA-256" }`, as hono writes it).
  auto algorithm = [&](size_t i) -> const Expr * {
    Expr *e = arg(i);
    if (auto *name = dyn_cast_or_null<NameExpr>(e))
      if (VarSymbol *var = lookup(name->name).var)
        if (const Expr *lit = constLiterals_.lookup(var))
          return lit;
    return e;
  };
  bool bad = false;

  // The format, written as a literal.
  auto format = [&](size_t i) -> std::string {
    std::optional<std::string> f = stringLiteral(arg(i));
    if (!f)
      return "";
    static const llvm::StringLiteral formats[] = {"raw", "pkcs8", "spki", "jwk"};
    if (!llvm::is_contained(formats, *f)) {
      bad = true;
      diags_.error(arg(i)->loc(), "unknown key format '" + *f +
                                      "'; it is 'raw', 'pkcs8', 'spki' or 'jwk'");
      return "";
    }
    return *f;
  };

  // The usages, written as an array literal of literals, against those the key takes: of a
  // public key or a private one, or of either (`isPublic` 0, 1, or -1 for both).
  auto usages = [&](size_t i, llvm::StringRef name, int isPublic, bool creates) {
    auto *lit = dyn_cast_or_null<ArrayLitExpr>(arg(i));
    if (!lit)
      return;
    std::vector<llvm::StringLiteral> allowed;
    if (!name.empty()) {
      if (isPublic != 0)
        allowed = usagesOf(name, true);
      if (isPublic != 1)
        for (llvm::StringLiteral u : usagesOf(name, false))
          if (!llvm::is_contained(allowed, u))
            allowed.push_back(u);
    }
    for (const ArrayLitExpr::Element &e : lit->elements) {
      std::optional<std::string> u = e.spread ? std::nullopt : stringLiteral(e.expr);
      if (!u)
        return;
      if (!llvm::is_contained(kUsages, *u)) {
        bad = true;
        diags_.error(e.expr->loc(), "usage '" + *u + "' does not exist; the usages are " +
                                        quotedList(kUsages, "and"));
        return;
      }
      if (!name.empty() && !llvm::is_contained(allowed, *u)) {
        bad = true;
        // "an HMAC key", "a PBKDF2 key"
        std::string an = llvm::StringRef("EHRAX").contains(name.front()) ? "an " : "a ";
        std::string what = isPublic == 1 ? "a public " + name.str() + " key"
                           : isPublic == 0 && name != "HMAC" && familyOf(name) != "Aes" &&
                                   familyOf(name) != "Kdf"
                               ? "a private " + name.str() + " key"
                               : an + name.str() + " key";
        diags_.error(e.expr->loc(),
                     "'" + *u + "' is no usage of " + what + "; " +
                         (allowed.empty() ? std::string("it takes none: pass []")
                                          : "it takes " + quotedList(allowed, "and")));
        return;
      }
    }
    // A secret or private key that can do nothing: node rejects it.
    if (!creates || name.empty() || isPublic == 1)
      return;
    std::vector<llvm::StringLiteral> own = usagesOf(name, false);
    bool any = false;
    for (const ArrayLitExpr::Element &e : lit->elements)
      any |= llvm::is_contained(own, *stringLiteral(e.expr));
    if (any)
      return;
    bad = true;
    if (lit->elements.empty() || isPublic == 0)
      diags_.error(lit->loc(), "a key without usages can do nothing, and node rejects it "
                               "('Usages cannot be empty'); give its usages, e.g. " +
                                   quotedList(own, "and"));
    else
      diags_.error(lit->loc(), "the private key of the pair would have no usages, which node "
                               "rejects ('Usages cannot be empty'); add " +
                                   quotedList(own, "or"));
  };

  // The key data, once checked, against the format: of what the program gave, before it became
  // the `BufferSource | JsonWebKey` of the parameter.
  std::string keyFormat;
  auto keyDataFits = [&]() {
    Expr *data = arg(1);
    if (!data || keyFormat.empty() || !data->type || data->type->isError())
      return true;
    while (auto *conv = dyn_cast<CastExpr>(data)) {
      if (!conv->isImplicit())
        break;
      data = conv->operand;
    }
    const Type *type = data->type;
    if (!type || type->isError())
      return true;
    bool bytes = type->isBytes() ||
                 (type->isUnion() && llvm::all_of(type->members(),
                                                  [](const Type *t) { return t->isBytes(); }));
    bool jwk = !type->isBytes() && type->isObject() && !classOf(type) && !type->isArray() &&
               !type->isTuple();
    if (keyFormat == "jwk" && bytes) {
      diags_.error(data->loc(), "the key data of 'jwk' is a JsonWebKey; found '" +
                                    type->name().str() + "'");
      return false;
    }
    if (keyFormat != "jwk" && jwk) {
      diags_.error(data->loc(), "the key data of '" + keyFormat +
                                    "' is bytes (a BufferSource); found '" + type->name().str() +
                                    "'");
      return false;
    }
    return true;
  };

  std::string target; // the function of tz:webcrypto the call goes to, if not the method
  std::vector<bool> checkedArgs(args.size(), false);
  // What the program writes as a JSON value: a name or a member of one of type Json, or
  // JSON.parse(text).
  std::function<bool(const Expr *)> isJsonValue = [&](const Expr *e) -> bool {
    if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
      VarSymbol *var = lookup(name->name).var;
      return var && var->type && var->type->nonOptional()->isJson();
    }
    if (auto *m = dyn_cast_or_null<MemberExpr>(e))
      return isJsonValue(m->object);
    if (auto *index = dyn_cast_or_null<IndexExpr>(e))
      return isJsonValue(index->object);
    if (auto *c = dyn_cast_or_null<CallExpr>(e))
      if (auto *m = dyn_cast<MemberExpr>(c->callee))
        return m->name == "parse" && m->typeArgs.empty() && isGlobalName(m->object, "JSON");
    return false;
  };
  if (method == "digest") {
    literalAlgorithm(diags_, algorithm(0), "digest", "", bad);
  } else if (method == "generateKey") {
    std::string name = literalAlgorithm(diags_, algorithm(0), "generateKey", "", bad);
    // A pair: the usages of both keys, of which the private one needs some.
    llvm::StringRef family = familyOf(name);
    usages(2, name, family == "Hmac" || family == "Aes" ? 0 : -1, true);
    if (!name.empty())
      target = "subtleGenerate" + familyOf(name).str();
  } else if (method == "importKey") {
    std::string f = format(0);
    std::string name = literalAlgorithm(diags_, algorithm(2), "importKey", "", bad);
    if (!f.empty() && !name.empty()) {
      std::vector<llvm::StringLiteral> formats = formatsOf(familyOf(name));
      if (!llvm::is_contained(formats, f)) {
        bad = true;
        diags_.error(arg(0)->loc(), "'" + name + "' keys are not imported as '" + f +
                                        "' (node: 'Unable to import " + name + " using " + f +
                                        " format'); it takes " + quotedList(formats, "or"));
      }
    }
    int isPublic = f == "spki" || f == "raw" ? 1 : f == "pkcs8" ? 0 : -1;
    if (familyOf(name) == "Hmac" || familyOf(name) == "Aes" || familyOf(name) == "Kdf")
      isPublic = 0;
    usages(4, name, isPublic, isPublic == 0);
    keyFormat = f;
    if (!name.empty())
      target = "subtleImport" + familyOf(name).str();
    // A JWK as JSON, as JSON.parse gives it, is read as a JsonWebKey, which in node it is.
    if ((f.empty() || f == "jwk") && isJsonValue(arg(1)) && isa<NameExpr>(member->object) &&
        libFunctions_.lookup("subtleJwkOf")) {
      Expr *json = checkExpr(arg(1));
      auto *callee = ctx_.make<NameExpr>(json->loc(), "subtleJwkOf");
      auto *wrap = ctx_.make<CallExpr>(json->loc(), callee, std::vector<Expr *>{json});
      args[1] = callLibraryFunction(wrap, "subtleJwkOf", {}, {true}, json->loc());
      checkedArgs[1] = true;
      if (target.empty())
        target = "subtleImportAny";
    }
  } else if (method == "exportKey") {
    std::string f = format(0);
    if (!f.empty())
      target = f == "jwk" ? "subtleExportJwk" : "subtleExportBytes";
  } else if (method == "sign" || method == "verify" || method == "encrypt" ||
             method == "decrypt" || method == "deriveBits") {
    literalAlgorithm(diags_, algorithm(0), method, "", bad);
  } else if (method == "deriveKey") {
    literalAlgorithm(diags_, algorithm(0), "deriveBits", "", bad);
    std::string name = literalAlgorithm(diags_, algorithm(2), "importKey", "", bad);
    if (!name.empty()) {
      llvm::StringRef family = familyOf(name);
      if (family != "Hmac" && family != "Aes" && family != "Kdf") {
        bad = true;
        diags_.error(arg(2)->loc(), "deriveKey makes secret keys: AES, HMAC, HKDF or PBKDF2, "
                                    "not '" + name + "'");
      } else {
        if (family != "Kdf")
          literalAlgorithm(diags_, algorithm(2), "get key length", "", bad);
        usages(4, name, 0, true);
        target = "subtleDeriveKey" + family.str();
      }
    }
  } else if (method == "wrapKey") {
    format(0);
    literalAlgorithm(diags_, algorithm(3), "wrapKey", "encrypt", bad);
  } else if (method == "unwrapKey") {
    std::string f = format(0);
    literalAlgorithm(diags_, algorithm(3), "unwrapKey", "decrypt", bad);
    std::string name = literalAlgorithm(diags_, algorithm(4), "importKey", "", bad);
    if (!f.empty() && !name.empty()) {
      std::vector<llvm::StringLiteral> formats = formatsOf(familyOf(name));
      if (!llvm::is_contained(formats, f)) {
        bad = true;
        diags_.error(arg(0)->loc(), "'" + name + "' keys are not imported as '" + f +
                                        "'; it takes " + quotedList(formats, "or"));
      }
    }
    int isPublic = f == "spki" || f == "raw" ? 1 : f == "pkcs8" ? 0 : -1;
    if (familyOf(name) == "Hmac" || familyOf(name) == "Aes" || familyOf(name) == "Kdf")
      isPublic = 0;
    usages(6, name, isPublic, isPublic == 0);
    if (!name.empty())
      target = "subtleUnwrap" + familyOf(name).str();
  } else {
    return nullptr; // checkMethodCall reports it
  }

  if (bad) {
    // The arguments are still checked, as the method takes them (an empty array of usages
    // is one of KeyUsage).
    call->type = Type::error();
    std::vector<FunctionDecl *> methods =
        methodsNamed(libraryClass("SubtleCrypto"), method, /*isStatic=*/false);
    FuncSymbol *sym = methods.size() == 1 ? methods[0]->sym : nullptr;
    if (sym)
      resolveParams(sym);
    for (size_t i = 0; i < args.size(); ++i)
      if (!checkedArgs[i])
        args[i] = sym && i < sym->params.size() ? checkArg(args[i], sym->params[i])
                                               : checkExpr(args[i]);
    return call;
  }
  // The function of the family takes the arguments of the method; the receiver, crypto.subtle
  // or a variable, has nothing to evaluate.
  Expr *checked;
  if (!target.empty() && libFunctions_.lookup(target) && isa<NameExpr>(member->object))
    checked = callLibraryFunction(call, target, {}, checkedArgs, member->loc());
  else
    checked = checkMethodCall(call, member, expected);
  if (!keyDataFits())
    checked->type = Type::error();
  return checked;
}

} // namespace tz
