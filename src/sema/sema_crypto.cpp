// node:crypto (docs/crypto.md): the calls of its functions, the methods and properties of its
// objects (Hash, Hmac, Cipheriv, Decipheriv, KeyObject, Sign, Verify, ECDH, X509Certificate) and
// the global crypto of Web Crypto, which become calls of runtime/crypto*.c (see crypto_api.h).
//
// The compiler picks the layer of OpenSSL (docs/crypto.md, section 3): a hash or a cipher named
// by a literal goes straight to its functions when the direct layer has it, and a name known only
// when the program runs, keys, signatures and certificates to the full one. A literal name that
// node does not know, or that node 24 turns off (md4, des-cbc), is an error here; the callback
// forms are errors with the Sync form as the hint, as with node:fs (docs/io.md).

#include "sema/crypto_api.h"
#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

using Crypto = Type::Crypto;

// The names of each hash of the direct layer that node takes (lower case): those of OpenSSL's
// provider (SHA2-256, SHA-256, its OID), of its objects, and the names of signatures that
// getHashes() lists ("RSA-SHA256").
struct HashNames {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> aliases;
};

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

// What getCiphers() lists in node 24 with OpenSSL 3.5: every cipher name node takes.
const llvm::StringLiteral kCipherNames[] = {"aes-128-cbc",
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
const llvm::StringLiteral kFunctions[] = {"createHash",
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
const llvm::StringLiteral kClasses[] = {"Hash", "Hmac",   "Cipheriv", "Decipheriv",     "KeyObject",
                                        "Sign", "Verify", "ECDH",     "X509Certificate"};
// The functions that take a callback, with their Sync form.
const std::pair<llvm::StringLiteral, llvm::StringLiteral> kCallbackForms[] = {
    {"pbkdf2", "pbkdf2Sync"},          {"scrypt", "scryptSync"},
    {"argon2", "argon2Sync"},          {"hkdf", "hkdfSync"},
    {"randomFill", "randomFillSync"},  {"generateKeyPair", "generateKeyPairSync"},
    {"generateKey", "generateKeySync"}};
const llvm::StringLiteral kExports[] = {"Certificate",
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

std::string utf8Of(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

std::u16string utf16Of(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

// Text known when compiling: a literal, or a template without values.
std::optional<std::string> literalOf(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit())
    e = conv->operand;
  if (auto *lit = dyn_cast<StringLitExpr>(e))
    return utf8Of(lit->value);
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty())
    return utf8Of(tmpl->quasis[0]);
  return std::nullopt;
}

// The hashes node 24 turns off, which fail in node with ERR_OSSL_EVP_UNSUPPORTED.
bool turnedOffHash(llvm::StringRef name) {
  static const llvm::StringLiteral names[] = {"md4",      "md2",
                                              "mdc2",     "whirlpool",
                                              "rsa-md4",  "md4withrsaencryption",
                                              "rsa-mdc2", "mdc2withrsa",
                                              "rsa-md2",  "md2withrsaencryption"};
  return llvm::is_contained(names, name);
}

bool turnedOffCipher(llvm::StringRef name) {
  for (llvm::StringRef prefix : {"bf", "blowfish", "cast", "rc2", "rc4", "rc5", "idea", "seed"})
    if (name.starts_with(prefix))
      return true;
  return name == "des" || name == "desx" || name.starts_with("desx-") ||
         (name.starts_with("des-") && !name.starts_with("des-ede"));
}

// The methods each class has, for the message about a method it has not.
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

} // namespace

//===----------------------------------------------------------------------===//
// Imports and names
//===----------------------------------------------------------------------===//

std::string cryptoExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string what = "'" + name.str() + "' of '" + path.str() + "'";
  if (llvm::is_contained(kFunctions, name) || llvm::is_contained(kClasses, name) ||
      name == "constants")
    return "";
  for (const auto &[form, sync] : kCallbackForms)
    if (form == name)
      return what + " takes a callback, which tz does not have (docs/crypto.md, section 5); "
                    "import '" +
             sync.str() + "' and use what it returns";
  if (name == "webcrypto")
    return what + " is the global crypto of Web Crypto, which needs no import: use "
                  "'crypto.subtle', 'crypto.getRandomValues' and 'crypto.randomUUID'";
  if (name == "subtle")
    return what + " is the global crypto.subtle, which needs no import: use 'crypto.subtle'";
  if (llvm::is_contained(kExports, name))
    return what + " is not supported yet";
  return "'" + path.str() + "' has no export named '" + name.str() + "'";
}

const Type *Sema::cryptoTypeNamed(llvm::StringRef name) const {
  const NodeName *n = nodeName(name);
  if (!n || n->module != NodeModule::Crypto)
    return nullptr;
  static const std::pair<llvm::StringLiteral, Crypto> classes[] = {
      {"Hash", Crypto::Hash},           {"Hmac", Crypto::Hmac},
      {"Cipheriv", Crypto::Cipheriv},   {"Decipheriv", Crypto::Decipheriv},
      {"KeyObject", Crypto::KeyObject}, {"Sign", Crypto::Sign},
      {"Verify", Crypto::Verify},       {"ECDH", Crypto::Ecdh},
      {"X509Certificate", Crypto::X509}};
  for (const auto &[className, kind] : classes)
    if (className == n->name)
      return Type::cryptoType(kind);
  return nullptr;
}

// constants.RSA_PKCS1_PSS_PADDING and the others of node:crypto, as node has them.
Expr *Sema::cryptoConstant(MemberExpr *member) {
  static const std::pair<llvm::StringLiteral, int> constants[] = {
      {"RSA_PKCS1_PADDING", 1},           {"RSA_NO_PADDING", 3},
      {"RSA_PKCS1_OAEP_PADDING", 4},      {"RSA_X931_PADDING", 5},
      {"RSA_PKCS1_PSS_PADDING", 6},       {"RSA_PSS_SALTLEN_DIGEST", -1},
      {"RSA_PSS_SALTLEN_MAX_SIGN", -2},   {"RSA_PSS_SALTLEN_AUTO", -2},
      {"POINT_CONVERSION_COMPRESSED", 2}, {"POINT_CONVERSION_UNCOMPRESSED", 4},
      {"POINT_CONVERSION_HYBRID", 6}};
  for (const auto &[name, value] : constants)
    if (name == member->name) {
      Expr *lit = ctx_.make<IntLitExpr>(member->loc(), uint64_t(value < 0 ? -value : value));
      lit->type = Type::intTy();
      if (value >= 0)
        return lit;
      auto *negated = ctx_.make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
      return checkExpr(negated);
    }
  diags_.error(member->loc(), "'constants." + member->name +
                                  "' of 'node:crypto' is not supported yet; tz has the RSA_ and "
                                  "POINT_CONVERSION_ ones");
  member->type = Type::error();
  return member;
}

//===----------------------------------------------------------------------===//
// Arguments
//===----------------------------------------------------------------------===//

Expr *Sema::cryptoCall(CallExpr *call, CryptoFn fn, std::vector<Expr *> args, const Type *type) {
  auto *index = ctx_.make<IntLitExpr>(call->loc(), uint64_t(fn));
  index->type = Type::intTy();
  args.insert(args.begin(), index);
  call->args = std::move(args);
  call->spread.clear();
  call->builtin = Builtin::Crypto;
  call->type = type;
  return call;
}

Expr *Sema::cryptoInt(llvm::SMLoc loc, int64_t value) {
  auto *lit = ctx_.make<IntLitExpr>(loc, uint64_t(value));
  lit->type = Type::intTy();
  return lit;
}

Expr *Sema::cryptoDouble(llvm::SMLoc loc, double value) {
  auto *lit = ctx_.make<FloatLitExpr>(loc, value);
  lit->type = Type::doubleTy();
  return lit;
}

Expr *Sema::cryptoBool(llvm::SMLoc loc, bool value) {
  auto *lit = ctx_.make<BoolLitExpr>(loc, value);
  lit->type = Type::boolTy();
  return lit;
}

Expr *Sema::cryptoString(llvm::SMLoc loc, llvm::StringRef text) {
  auto *lit = ctx_.make<StringLitExpr>(loc, utf16Of(text));
  lit->type = Type::string();
  return lit;
}

Expr *Sema::cryptoUndefined(llvm::SMLoc loc) {
  auto *undef = ctx_.make<UndefinedExpr>(loc);
  undef->type = Type::undefinedTy();
  return undef;
}

// Data: a string, bytes (a typed array, a Buffer, a DataView; an ArrayBuffer with `buffers`),
// or a secret KeyObject (with `keys`), or a union of those.
Expr *Sema::cryptoData(Expr *arg, llvm::StringRef what, bool strings, bool keys, bool buffers) {
  arg = checkExpr(arg, Type::string());
  auto fits = [&](const Type *t) {
    return (strings && (t->isString() || t->isStringLiteral())) || t->isByteView() ||
           (buffers && t->bytes() == Type::Bytes::ArrayBuffer) ||
           (keys && t->crypto() == Crypto::KeyObject);
  };
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  bool ok = fits(type) || (type->isUnion() && llvm::all_of(type->members(), fits));
  if (!ok) {
    std::string kinds = strings ? "a string or bytes" : "bytes";
    diags_.error(arg->loc(),
                 "the " + what.str() + " is " + kinds + " (a Buffer, a typed array or a DataView" +
                     (buffers ? ", an ArrayBuffer" : "") + (keys ? ", a KeyObject" : "") +
                     "); found '" + type->name().str() + "'" +
                     (type->isNumeric() ? "; make text of it with 'String(x)'" : ""));
    arg->type = Type::error();
  }
  if (type->isStringLiteral())
    return coerce(arg, Type::string());
  return arg;
}

// An encoding written as a literal ('hex', 'utf8'...): its number; `fallback` (as a number)
// without one. Null after an error.
Expr *Sema::cryptoEncoding(CallExpr *call, size_t index, llvm::StringRef fn, int fallback) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index]))
    return cryptoInt(call->loc(), fallback);
  Expr *arg = call->args[index];
  std::optional<std::string> text = literalOf(arg);
  if (!text) {
    checkExpr(arg);
    diags_.error(arg->loc(), "the encoding of '" + fn +
                                 "' is written as a literal, e.g. 'hex': "
                                 "the type of what it gives depends on it");
    return nullptr;
  }
  if (*text == "buffer")
    return cryptoInt(call->loc(), -1);
  Expr *number = checkBufferEncoding(arg);
  if (number->type->isError())
    return nullptr;
  return number;
}

// The bytes `made` gives as text in `encoding` (a number, -1 for the bytes themselves): what
// digest('hex') and the like give.
Expr *Sema::cryptoText(Expr *made, Expr *encoding) {
  if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
    return made;
  auto *member = ctx_.make<MemberExpr>(made->loc(), made, "toString");
  return builtinCall(made->loc(), member, Builtin::BufferToString, {encoding}, Type::string());
}

// The options of `fn` written as an object literal in the call, by name; false after an error.
bool Sema::cryptoOptions(Expr *arg, llvm::StringRef fn, llvm::ArrayRef<llvm::StringRef> allowed,
                         llvm::StringMap<Expr *> &options) {
  auto *lit = dyn_cast<ObjectLitExpr>(arg);
  if (!lit) {
    checkExpr(arg);
    diags_.error(arg->loc(), "the options of '" + fn +
                                 "' are written in the call, as an object literal, e.g. '{ " +
                                 allowed[0].str() + ": ... }'");
    return false;
  }
  std::string list;
  for (size_t i = 0; i < allowed.size(); ++i)
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.key || prop.method || prop.name.empty()) {
      diags_.error(prop.loc, "write each option of '" + fn + "' by its name, e.g. '{ " +
                                 allowed[0].str() + ": ... }'");
      return false;
    }
    if (!llvm::is_contained(allowed, prop.name)) {
      diags_.error(prop.loc, "'" + fn + "' has no option '" + prop.name + "'; it takes " + list);
      return false;
    }
    options[prop.name] = prop.value;
  }
  return true;
}

// A string option that must be a literal of `values`: the index of the one written, or -1 after
// an error.
int Sema::cryptoChoice(Expr *arg, llvm::StringRef what, llvm::ArrayRef<llvm::StringRef> values) {
  std::optional<std::string> text = literalOf(arg);
  std::string list;
  for (size_t i = 0; i < values.size(); ++i)
    list += (i == 0 ? "'" : i + 1 == values.size() ? "' or '" : "', '") + values[i].str();
  list += "'";
  if (!text) {
    checkExpr(arg);
    diags_.error(arg->loc(), "the " + what + " is written as a literal: " + list);
    return -1;
  }
  for (size_t i = 0; i < values.size(); ++i)
    if (values[i] == *text)
      return int(i);
  diags_.error(arg->loc(), "unknown " + what + " '" + *text + "'; it is " + list);
  return -1;
}

// A hash named by `arg`: an int literal of kDirectMds for a literal the direct layer has, the
// literal itself for one only the full layer has, or the string for a name known only now.
Expr *Sema::cryptoMd(Expr *arg, bool hmac) {
  std::optional<std::string> text = literalOf(arg);
  if (!text)
    return checkArg(arg, Type::string());
  std::string name = llvm::StringRef(*text).lower();
  for (const HashNames &h : hashNames()) {
    if (h.name != name && !llvm::is_contained(h.aliases, name))
      continue;
    if (hmac && (h.name == "shake128" || h.name == "shake256")) {
      diags_.error(arg->loc(), "'" + *text +
                                   "' gives output of any length, which HMAC and the "
                                   "KDFs cannot use; use a hash such as 'sha256'");
      return nullptr;
    }
    for (size_t i = 0; i < std::size(kDirectMds); ++i)
      if (kDirectMds[i].name == h.name)
        return cryptoInt(arg->loc(), int64_t(i));
  }
  if (turnedOffHash(name)) {
    diags_.error(arg->loc(), "'" + *text +
                                 "' is turned off in node 24 (it throws "
                                 "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use 'sha256'");
    return nullptr;
  }
  diags_.error(arg->loc(), "unknown hash '" + *text +
                               "'; node has those getHashes() lists, e.g. "
                               "'sha256', 'sha512', 'sha3-256', 'md5'");
  return nullptr;
}

// A cipher named by `arg`, as cryptoMd does for hashes.
Expr *Sema::cryptoCipher(Expr *arg) {
  std::optional<std::string> text = literalOf(arg);
  if (!text)
    return checkArg(arg, Type::string());
  std::string name = llvm::StringRef(*text).lower();
  std::string canonical = name == "aes128"          ? "aes-128-cbc"
                          : name == "aes192"        ? "aes-192-cbc"
                          : name == "aes256"        ? "aes-256-cbc"
                          : name == "id-aes128-gcm" ? "aes-128-gcm"
                          : name == "id-aes192-gcm" ? "aes-192-gcm"
                          : name == "id-aes256-gcm" ? "aes-256-gcm"
                                                    : name;
  for (size_t i = 0; i < std::size(kDirectCiphers); ++i)
    if (kDirectCiphers[i].name == canonical)
      return cryptoInt(arg->loc(), int64_t(i));
  if (llvm::is_contained(kCipherNames, name))
    return cryptoString(arg->loc(), name);
  if (turnedOffCipher(name)) {
    diags_.error(arg->loc(), "'" + *text +
                                 "' is turned off in node 24 (it throws "
                                 "ERR_OSSL_EVP_UNSUPPORTED) and in tz; use "
                                 "'aes-256-gcm'");
    return nullptr;
  }
  diags_.error(arg->loc(), "unknown cipher '" + *text +
                               "'; node has those getCiphers() lists, "
                               "e.g. 'aes-256-gcm', 'aes-256-cbc', "
                               "'chacha20-poly1305'");
  return nullptr;
}

// A number argument, as a double.
Expr *Sema::cryptoNumber(Expr *arg) { return checkArg(arg, Type::doubleTy()); }

// The key of a signature or an ECDH: a KeyObject, or a PEM or DER key (a string or bytes).
Expr *Sema::cryptoKey(Expr *arg, llvm::StringRef what) {
  return cryptoData(arg, what, /*strings=*/true, /*keys=*/true, /*buffers=*/true);
}

Expr *Sema::cryptoKeyObject(Expr *arg, llvm::StringRef what) {
  arg = checkExpr(arg);
  if (!arg->type->isError() && arg->type->crypto() != Crypto::KeyObject) {
    diags_.error(arg->loc(), "the " + what + " is a KeyObject; found '" + arg->type->name() + "'");
    arg->type = Type::error();
  }
  return arg;
}

// A callback where node:crypto takes one: an error with the form without it as the hint.
bool Sema::rejectCryptoCallback(CallExpr *call, size_t index, llvm::StringRef fn,
                                llvm::StringRef hint) {
  if (index >= call->args.size())
    return false;
  Expr *arg = checkExpr(call->args[index]);
  if (!arg->type->nonOptional()->isFunction())
    return false;
  diags_.error(arg->loc(), "'" + fn +
                               "' with a callback is not supported: tz has no callback forms "
                               "of node:crypto (docs/crypto.md, section 5); call it without one "
                               "and use what it returns: " +
                               hint);
  for (Expr *&a : call->args)
    if (a != call->args[index])
      a = checkExpr(a);
  call->type = Type::error();
  return true;
}

//===----------------------------------------------------------------------===//
// The functions of node:crypto
//===----------------------------------------------------------------------===//

Expr *Sema::checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    diags_.error(loc,
                 "'" + fn + "' of 'node:crypto' is a class, not a function" +
                     (fn == "X509Certificate" ? "; make one with 'new X509Certificate(pem)'" : ""));
    return loosely();
  }
  if (fn == "constants") {
    diags_.error(loc, "'constants' of 'node:crypto' is used as 'constants.RSA_PKCS1_PSS_PADDING' "
                      "and the like");
    return loosely();
  }
  if (rejectSpread(call))
    return call;
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) || cryptoOptions(args[i], fn, allowed, options);
  };

  if (fn == "createHash") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *md = cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md)
      return call;
    Expr *length = options.lookup("outputLength");
    return cryptoCall(call, CryptoFn::HashNew,
                      {md, length ? cryptoNumber(length) : cryptoDouble(loc, -1)},
                      Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    if (!checkArity(call, loc, 2, 2))
      return call;
    Expr *md = cryptoMd(args[0], true);
    Expr *key = cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::HmacNew, {md, key, cryptoInt(loc, 0)},
                      Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    if (!checkArity(call, loc, 2, 3))
      return call;
    Expr *md = cryptoMd(args[0], false);
    Expr *data = cryptoData(args[1], "data", true, false, false);
    Expr *encoding = cryptoEncoding(call, 2, fn, 6); // hex
    if (!md || data->type->isError() || !encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      return cryptoCall(call, CryptoFn::HashOnce, {md, data}, buffer);
    return cryptoCall(call, CryptoFn::HashOnceText, {md, data, encoding}, Type::string());
  }
  if (fn == "randomBytes") {
    if (!checkArity(call, loc, 1, 2) ||
        rejectCryptoCallback(call, 1, fn, "'const bytes = randomBytes(n)'"))
      return call;
    return cryptoCall(call, CryptoFn::RandomBytes, {cryptoNumber(args[0])}, buffer);
  }
  if (fn == "randomUUID") {
    if (!checkArity(call, loc, 0, 1) || !optionsAt(0, {"disableEntropyCache"}))
      return call;
    if (Expr *cache = options.lookup("disableEntropyCache"))
      checkArg(cache, Type::boolTy());
    return cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    if (!checkArity(call, loc, 1, 3))
      return call;
    size_t last = args.size() - 1;
    if (args.size() > 1 && rejectCryptoCallback(call, last, fn, "'const n = randomInt(max)'"))
      return call;
    if (args.size() == 3) {
      diags_.error(args[2]->loc(), "'randomInt' takes a minimum and a maximum");
      return loosely();
    }
    Expr *min = args.size() == 2 ? checkExpr(args[0], Type::intTy()) : cryptoInt(loc, 0);
    Expr *max = checkExpr(args.back(), Type::intTy());
    if (min->type->isInt() && max->type->isInt())
      return cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
    return cryptoCall(call, CryptoFn::RandomInt, {cryptoNumber(min), cryptoNumber(max)},
                      Type::doubleTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    if (!checkArity(call, loc, 1, fn == "randomFillSync" ? 3 : 1))
      return call;
    Expr *target = checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError())
      return call;
    if (fn == "getRandomValues") {
      static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                           Type::Bytes::Float64};
      if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
        diags_.error(target->loc(), "'getRandomValues' fills an array of integers (a Uint8Array, "
                                    "an Int32Array, a Buffer...); found '" +
                                        type->name() + "'");
        return call;
      }
      return cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
      diags_.error(target->loc(), "'randomFillSync' fills bytes (a Buffer, a typed array, a "
                                  "DataView or an ArrayBuffer); found '" +
                                      type->name() + "'");
      return call;
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = present(1) ? cryptoNumber(args[1]) : cryptoDouble(loc, 0);
    Expr *size = present(2) ? cryptoNumber(args[2]) : cryptoDouble(loc, 0);
    return cryptoCall(call, CryptoFn::RandomFill, {target, offset, size, cryptoInt(loc, has)},
                      type);
  }
  if (fn == "timingSafeEqual") {
    if (!checkArity(call, loc, 2, 2))
      return call;
    Expr *a = cryptoData(args[0], "first argument", false, false, true);
    Expr *b = cryptoData(args[1], "second argument", false, false, true);
    if (a->type->isError() || b->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b}, Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    if (!checkArity(call, loc, 5, 5))
      return call;
    Expr *password = cryptoData(args[0], "password", true, true, true);
    Expr *salt = cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = cryptoNumber(args[2]), *keylen = cryptoNumber(args[3]);
    Expr *md = cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::Pbkdf2, {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    if (!checkArity(call, loc, 5, 5))
      return call;
    Expr *md = cryptoMd(args[0], true);
    Expr *key = cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = cryptoData(args[2], "salt", true, false, true);
    Expr *info = cryptoData(args[3], "info", true, false, true);
    Expr *keylen = cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                      Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    if (!checkArity(call, loc, 3, 4))
      return call;
    Expr *password = cryptoData(args[0], "password", true, true, true);
    Expr *salt = cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = cryptoNumber(args[2]);
    if (!optionsAt(3, {"N", "r", "p", "maxmem", "cost", "blockSize", "parallelization"}))
      return call;
    // N, r and p have a second name each; node rejects both together.
    static const std::pair<llvm::StringLiteral, llvm::StringLiteral> pairs[] = {
        {"N", "cost"}, {"r", "blockSize"}, {"p", "parallelization"}};
    Expr *values[3];
    for (int i = 0; i < 3; ++i) {
      Expr *a = options.lookup(pairs[i].first), *b = options.lookup(pairs[i].second);
      if (a && b) {
        diags_.error(b->loc(), "option '" + pairs[i].first + "' cannot be used together with '" +
                                   pairs[i].second + "': they are one option");
        return call;
      }
      values[i] = a || b ? cryptoNumber(a ? a : b) : cryptoDouble(loc, 0);
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::Scrypt,
                      {password, salt, keylen, values[0], values[1], values[2],
                       maxmem ? cryptoNumber(maxmem) : cryptoDouble(loc, 0)},
                      buffer);
  }
  if (fn == "argon2Sync") {
    if (!checkArity(call, loc, 2, 2))
      return call;
    Expr *type, *name;
    if (literalOf(args[0])) {
      int which = cryptoChoice(args[0], "algorithm of Argon2", {"argon2d", "argon2i", "argon2id"});
      if (which < 0)
        return call;
      type = cryptoInt(loc, which);
      name = cryptoString(loc, "");
    } else {
      type = cryptoInt(loc, 0xffffffff); // -1: the name is known only now
      name = checkArg(args[0], Type::string());
    }
    if (!cryptoOptions(args[1], fn,
                       {"message", "nonce", "parallelism", "tagLength", "memory", "passes",
                        "secret", "associatedData"},
                       options))
      return call;
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"})
      if (!options.lookup(required)) {
        diags_.error(args[1]->loc(), "the parameters of 'argon2Sync' need '" + required + "'");
        return call;
      }
    Expr *message = cryptoData(options["message"], "message", true, false, true);
    Expr *nonce = cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = cryptoNumber(options["parallelism"]);
    Expr *tagLength = cryptoNumber(options["tagLength"]);
    Expr *memory = cryptoNumber(options["memory"]);
    Expr *passes = cryptoNumber(options["passes"]);
    Expr *secret = options.lookup("secret")
                       ? cryptoData(options["secret"], "secret", true, false, true)
                       : cryptoUndefined(loc);
    Expr *data = options.lookup("associatedData")
                     ? cryptoData(options["associatedData"], "associatedData", true, false, true)
                     : cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data})
      if (e->type->isError())
        return call;
    return cryptoCall(
        call, CryptoFn::Argon2,
        {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    if (!checkArity(call, loc, 3, 4))
      return call;
    std::optional<std::string> name = literalOf(args[0]);
    Expr *cipher = cryptoCipher(args[0]);
    Expr *key = cryptoData(args[1], "key", true, true, true);
    Expr *iv = cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() || iv->type->isError())
      return call;
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      diags_.error(args[0]->loc(), "'" + *name +
                                       "' needs the length of its tag: '{ "
                                       "authTagLength: 16 }'");
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return cryptoCall(call, CryptoFn::CipherNew,
                      {cipher, key, iv, tag ? cryptoNumber(tag) : cryptoDouble(loc, -1),
                       cryptoBool(loc, decrypt)},
                      Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    if (!checkArity(call, loc, 0, 0))
      return call;
    return cryptoCall(call,
                      fn == "getHashes"    ? CryptoFn::GetHashes
                      : fn == "getCiphers" ? CryptoFn::GetCiphers
                                           : CryptoFn::GetCurves,
                      {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *key = cryptoData(args[0], "key", true, false, true);
    Expr *encoding = cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      encoding = cryptoInt(loc, 0);
    return cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    if (!checkArity(call, loc, 1, 1))
      return call;
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = cryptoInt(loc, 0), *type = cryptoString(loc, "");
    Expr *passphrase = cryptoUndefined(loc);
    int format = 0, has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!cryptoOptions(args[0], fn, {"key", "format", "type", "passphrase", "encoding"}, options))
        return call;
      if (Expr *f = options.lookup("format"))
        if ((format = cryptoChoice(f, "format of the key", {"pem", "der", "jwk"})) < 0)
          return call;
      if (!options.lookup("key")) {
        diags_.error(args[0]->loc(), "the options of '" + fn + "' need 'key'");
        return call;
      }
      key = format == 2 ? checkArg(options["key"], Type::json())
                        : cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        int which = cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0)
          return call;
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = literalOf(e) ? checkBufferEncoding(e) : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          diags_.error(e->loc(), "the encoding of the key is written as a literal, e.g. 'hex'");
          return call;
        }
        encoding = number;
      }
    } else {
      key = cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::CreateKey,
                      {cryptoBool(loc, priv), key, encoding, cryptoInt(loc, format),
                       cryptoInt(loc, has), type, passphrase},
                      keyObject);
  }
  if (fn == "generateKeyPairSync")
    return checkGenerateKeyPair(call, loc);
  if (fn == "generateKeySync") {
    if (!checkArity(call, loc, 2, 2))
      return call;
    int which = cryptoChoice(args[0], "type of the key", {"hmac", "aes"});
    if (which < 0 || !cryptoOptions(args[1], fn, {"length"}, options))
      return call;
    if (!options.lookup("length")) {
      diags_.error(args[1]->loc(), "'generateKeySync' needs the length of the key in bits: '{ "
                                   "length: 256 }'");
      return call;
    }
    return cryptoCall(call, CryptoFn::GenerateSecretKey,
                      {cryptoBool(loc, which == 1), cryptoNumber(options["length"])}, keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    if (!checkArity(call, loc, unsigned(count), unsigned(count + 1)) ||
        rejectCryptoCallback(call, count, fn,
                             verify ? "'const ok = verify(algorithm, data, key, signature)'"
                                    : "'const signature = sign(algorithm, data, key)'"))
      return call;
    Expr *algorithm =
        isa<UndefinedExpr>(args[0]) ? cryptoUndefined(loc) : checkArg(args[0], Type::string());
    Expr *data = cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!cryptoSignKey(args[2], fn, !verify, signArgs))
      return call;
    if (verify) {
      Expr *signature = cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError())
        return call;
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError())
      return call;
    return cryptoCall(call, verify ? CryptoFn::Verify : CryptoFn::Sign, std::move(signArgs),
                      verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    if (!checkArity(call, loc, 1, 1))
      return call;
    Expr *name = args[0];
    if (literalOf(name) && !cryptoMd(name, true))
      return call;
    bool verify = fn == "createVerify";
    return cryptoCall(call, CryptoFn::SignNew,
                      {checkArg(name, Type::string()), cryptoBool(loc, verify)},
                      Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign));
  }
  if (fn == "createECDH") {
    if (!checkArity(call, loc, 1, 1))
      return call;
    return cryptoCall(call, CryptoFn::EcdhNew, {checkArg(args[0], Type::string())},
                      Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    if (!checkArity(call, loc, 1, 1) ||
        !cryptoOptions(args[0], fn, {"privateKey", "publicKey"}, options))
      return call;
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      diags_.error(args[0]->loc(), "'diffieHellman' needs '{ privateKey, publicKey }'");
      return call;
    }
    Expr *priv = cryptoKeyObject(options["privateKey"], "private key");
    Expr *pub = cryptoKeyObject(options["publicKey"], "public key");
    if (priv->type->isError() || pub->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
  }
  diags_.error(loc, cryptoExportProblem(fn, "node:crypto"));
  return loosely();
}

// The key of a signature at `arg`: a key, or the options with it ({ key, padding, saltLength,
// dsaEncoding }), appended to `args` as the runtime takes them: the key, padding, saltLength,
// the encoding of DSA (1 for 'ieee-p1363') and which were given.
bool Sema::cryptoSignKey(Expr *arg, llvm::StringRef fn, bool priv, std::vector<Expr *> &args) {
  llvm::SMLoc loc = arg->loc();
  Expr *padding = cryptoInt(loc, 0), *salt = cryptoInt(loc, 0);
  int p1363 = 0, has = 0;
  Expr *key;
  if (isa<ObjectLitExpr>(arg)) {
    llvm::StringMap<Expr *> options;
    if (!cryptoOptions(arg, fn, {"key", "padding", "saltLength", "dsaEncoding"}, options))
      return false;
    if (!options.lookup("key")) {
      diags_.error(arg->loc(), "the options of '" + fn + "' need 'key'");
      return false;
    }
    key = cryptoKey(options["key"], "key");
    if (Expr *p = options.lookup("padding")) {
      padding = checkArg(p, Type::intTy());
      has |= 1;
    }
    if (Expr *s = options.lookup("saltLength")) {
      salt = checkArg(s, Type::intTy());
      has |= 2;
    }
    if (Expr *d = options.lookup("dsaEncoding")) {
      int which = cryptoChoice(d, "encoding of the signature", {"der", "ieee-p1363"});
      if (which < 0)
        return false;
      p1363 = which;
    }
  } else {
    key = cryptoKey(arg, priv ? "private key" : "key");
  }
  if (key->type->isError())
    return false;
  args.insert(args.end(), {key, padding, salt, cryptoInt(loc, p1363), cryptoInt(loc, has)});
  return true;
}

// generateKeyPairSync(type, options): `{ publicKey, privateKey }`, each a KeyObject, or the key
// written as its encoding asks: a string (PEM), a Buffer (DER) or a Json (JWK).
Expr *Sema::checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc) {
  std::vector<Expr *> &args = call->args;
  if (!checkArity(call, loc, 1, 2))
    return call;
  static const llvm::StringRef types[] = {"rsa",     "rsa-pss", "dsa",    "ec",
                                          "ed25519", "ed448",   "x25519", "x448"};
  int which = -1;
  Expr *name = cryptoString(loc, "");
  if (literalOf(args[0])) {
    if ((which = cryptoChoice(args[0], "type of the key pair", types)) < 0)
      return call;
  } else {
    name = checkArg(args[0], Type::string());
  }
  llvm::StringMap<Expr *> options;
  if (args.size() > 1 && !cryptoOptions(args[1], "generateKeyPairSync",
                                        {"modulusLength", "publicExponent", "namedCurve",
                                         "publicKeyEncoding", "privateKeyEncoding", "divisorLength",
                                         "hashAlgorithm", "mgf1HashAlgorithm", "saltLength"},
                                        options))
    return call;
  for (llvm::StringRef unsupported : {"hashAlgorithm", "mgf1HashAlgorithm", "saltLength"})
    if (Expr *e = options.lookup(unsupported)) {
      diags_.error(e->loc(), "the option '" + unsupported +
                                 "' of 'rsa-pss' keys is not "
                                 "supported yet");
      return call;
    }
  if (which >= 0 && which <= 2 && !options.lookup("modulusLength")) {
    diags_.error(loc, "'" + types[which] + "' keys need '{ modulusLength: 2048 }'");
    return call;
  }
  if (which == 3 && !options.lookup("namedCurve")) {
    diags_.error(loc, "'ec' keys need '{ namedCurve: \"P-256\" }'");
    return call;
  }
  int has = 0;
  Expr *modulus = cryptoDouble(loc, 0), *exponent = cryptoDouble(loc, 65537);
  Expr *curve = cryptoString(loc, "");
  if (Expr *m = options.lookup("modulusLength"))
    modulus = cryptoNumber(m);
  if (Expr *e = options.lookup("publicExponent")) {
    exponent = cryptoNumber(e);
    has |= 2;
  }
  if (Expr *d = options.lookup("divisorLength"))
    cryptoNumber(d); // OpenSSL chooses it, as node does without it
  if (Expr *c = options.lookup("namedCurve")) {
    curve = checkArg(c, Type::string());
    has |= 1;
  }
  auto *generate = ctx_.make<CallExpr>(loc, call->callee, std::vector<Expr *>{});
  cryptoCall(generate, CryptoFn::GenerateKeyPair,
             {cryptoInt(loc, which < 0 ? 0xffffffff : which), name, modulus, exponent,
              cryptoInt(loc, has), curve},
             Type::cryptoType(Crypto::KeyObject));
  // The encodings: the format (-1 for a KeyObject, 0 'pem', 1 'der', 2 'jwk'), which options
  // were given (1 type, 2 cipher, 4 passphrase), the type, the cipher and the passphrase of
  // each key, after the call that makes the private one.
  std::vector<Expr *> parts = {generate};
  const Type *fields[2];
  for (int k = 0; k < 2; ++k) {
    llvm::StringRef option = k == 0 ? "publicKeyEncoding" : "privateKeyEncoding";
    Expr *encoding = options.lookup(option);
    int format = -1, flags = 0;
    Expr *type = cryptoString(loc, ""), *cipher = cryptoString(loc, "");
    Expr *passphrase = cryptoUndefined(loc);
    if (encoding) {
      llvm::StringMap<Expr *> e;
      llvm::SmallVector<llvm::StringRef, 4> allowed = {"type", "format"};
      if (k == 1)
        allowed.append({"cipher", "passphrase"});
      if (!cryptoOptions(encoding, option, allowed, e))
        return call;
      if (!e.lookup("format")) {
        diags_.error(encoding->loc(), "'" + option + "' needs 'format': 'pem', 'der' or 'jwk'");
        return call;
      }
      if ((format = cryptoChoice(e["format"], "format of the key", {"pem", "der", "jwk"})) < 0)
        return call;
      if (Expr *t = e.lookup("type")) {
        int typeIndex = cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
        if (typeIndex < 0)
          return call;
        static const char *const names[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = cryptoString(t->loc(), names[typeIndex]);
        flags |= 1;
      } else if (format != 2) {
        diags_.error(encoding->loc(),
                     "'" + option + "' needs 'type': " +
                         (k == 0 ? "'spki' or 'pkcs1'" : "'pkcs8', 'pkcs1' or 'sec1'"));
        return call;
      }
      if (Expr *c = e.lookup("cipher")) {
        cipher = checkArg(c, Type::string());
        flags |= 2;
        if (!e.lookup("passphrase")) {
          diags_.error(encoding->loc(), "a cipher needs a 'passphrase'");
          return call;
        }
      }
      if (Expr *p = e.lookup("passphrase")) {
        passphrase = cryptoData(p, "passphrase", true, false, true);
        flags |= 4;
        if (passphrase->type->isError())
          return call;
      }
    }
    fields[k] = format < 0    ? Type::cryptoType(Crypto::KeyObject)
                : format == 0 ? Type::string()
                : format == 1 ? Type::bytesType(Type::Bytes::Buffer)
                              : Type::json();
    parts.insert(parts.end(), {cryptoInt(loc, format < 0 ? 0xffffffff : format),
                               cryptoInt(loc, flags), type, cipher, passphrase});
  }
  call->args = std::move(parts);
  call->spread.clear();
  call->builtin = Builtin::CryptoKeyPair;
  call->type =
      Type::object({{"publicKey", fields[0], false}, {"privateKey", fields[1], false}}, "KeyPair");
  return call;
}

//===----------------------------------------------------------------------===//
// Methods and properties
//===----------------------------------------------------------------------===//

Expr *Sema::checkCryptoMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (rejectSpread(call))
    return call;
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  Expr *self = member->object;
  // The bytes of `made` (a call of the receiver's method), or their text in the encoding at
  // args[index].
  auto bytesOrText = [&](CryptoFn fn, std::vector<Expr *> fnArgs, size_t index) -> Expr * {
    Expr *encoding = cryptoEncoding(call, index, name, -1);
    if (!encoding)
      return call;
    auto *made = ctx_.make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{});
    cryptoCall(made, fn, std::move(fnArgs), buffer);
    return cryptoText(made, encoding);
  };
  bool hash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
              kind == Crypto::Verify;
  bool cipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;

  if (hash && name == "update") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *data = cryptoData(args[0], "data", true, false, false);
    Expr *encoding = cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      encoding = cryptoInt(loc, 0);
    return cryptoCall(call, CryptoFn::HashUpdate, {self, data, encoding}, type);
  }
  if ((kind == Crypto::Hash || kind == Crypto::Hmac) && name == "digest") {
    if (!checkArity(call, loc, 0, 1))
      return call;
    Expr *encoding = cryptoEncoding(call, 0, name, -1);
    if (!encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      return cryptoCall(call, CryptoFn::HashDigest, {self}, buffer);
    return cryptoCall(call, CryptoFn::HashDigestText, {self, encoding}, Type::string());
  }
  if (kind == Crypto::Hash && name == "copy") {
    if (!checkArity(call, loc, 0, 1))
      return call;
    llvm::StringMap<Expr *> options;
    if (present(0) && !cryptoOptions(args[0], name, {"outputLength"}, options))
      return call;
    Expr *length = options.lookup("outputLength");
    return cryptoCall(call, CryptoFn::HashCopy,
                      {self, length ? cryptoNumber(length) : cryptoDouble(loc, -1)}, type);
  }
  if (kind == Crypto::Sign && name == "sign") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    std::vector<Expr *> signArgs = {self};
    if (!cryptoSignKey(args[0], name, true, signArgs))
      return call;
    return bytesOrText(CryptoFn::SignFinal, std::move(signArgs), 1);
  }
  if (kind == Crypto::Verify && name == "verify") {
    if (!checkArity(call, loc, 2, 3))
      return call;
    std::vector<Expr *> verifyArgs = {self};
    if (!cryptoSignKey(args[0], name, false, verifyArgs))
      return call;
    Expr *signature = cryptoData(args[1], "signature", true, false, true);
    Expr *encoding = cryptoEncoding(call, 2, name, 0);
    if (signature->type->isError() || !encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      encoding = cryptoInt(loc, 0);
    verifyArgs.insert(verifyArgs.begin() + 2, {signature, encoding});
    return cryptoCall(call, CryptoFn::VerifyFinal, std::move(verifyArgs), Type::boolTy());
  }
  if (cipher && name == "update") {
    if (!checkArity(call, loc, 1, 3))
      return call;
    Expr *data = cryptoData(args[0], "data", true, false, true);
    Expr *encoding = cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      encoding = cryptoInt(loc, 0);
    return bytesOrText(CryptoFn::CipherUpdate, {self, data, encoding}, 2);
  }
  if (cipher && name == "final") {
    if (!checkArity(call, loc, 0, 1))
      return call;
    return bytesOrText(CryptoFn::CipherFinal, {self}, 0);
  }
  if (cipher && name == "setAAD") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *data = cryptoData(args[0], "AAD", false, false, true);
    llvm::StringMap<Expr *> options;
    if (present(1) && !cryptoOptions(args[1], name, {"plaintextLength"}, options))
      return call;
    if (data->type->isError())
      return call;
    Expr *length = options.lookup("plaintextLength");
    return cryptoCall(call, CryptoFn::CipherSetAad,
                      {self, data, length ? cryptoNumber(length) : cryptoDouble(loc, -1)}, type);
  }
  if (kind == Crypto::Cipheriv && name == "getAuthTag") {
    if (!checkArity(call, loc, 0, 0))
      return call;
    return cryptoCall(call, CryptoFn::CipherGetAuthTag, {self}, buffer);
  }
  if (kind == Crypto::Decipheriv && name == "setAuthTag") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *tag = cryptoData(args[0], "tag", true, false, true);
    Expr *encoding = cryptoEncoding(call, 1, name, 0);
    if (tag->type->isError() || !encoding)
      return call;
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
      encoding = cryptoInt(loc, 0);
    return cryptoCall(call, CryptoFn::CipherSetAuthTag, {self, tag, encoding}, type);
  }
  if (cipher && name == "setAutoPadding") {
    if (!checkArity(call, loc, 0, 1))
      return call;
    Expr *padding = present(0) ? checkArg(args[0], Type::boolTy()) : cryptoBool(loc, true);
    return cryptoCall(call, CryptoFn::CipherSetAutoPadding, {self, padding}, type);
  }
  if (kind == Crypto::KeyObject && name == "export") {
    if (!checkArity(call, loc, 0, 1))
      return call;
    if (!present(0))
      return cryptoCall(call, CryptoFn::KeyExportSecret, {self}, buffer);
    llvm::StringMap<Expr *> options;
    if (!cryptoOptions(args[0], name, {"format", "type", "cipher", "passphrase"}, options))
      return call;
    int format = 3; // buffer
    if (Expr *f = options.lookup("format"))
      if ((format = cryptoChoice(f, "format of the key", {"pem", "der", "jwk", "buffer"})) < 0)
        return call;
    if (format == 2)
      return cryptoCall(call, CryptoFn::KeyExportJwk, {self}, Type::json());
    if (format == 3)
      return cryptoCall(call, CryptoFn::KeyExportSecret, {self}, buffer);
    int has = 0;
    Expr *keyType = cryptoString(loc, ""), *cipherName = cryptoString(loc, "");
    Expr *passphrase = cryptoUndefined(loc);
    if (Expr *t = options.lookup("type")) {
      int which = cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
      if (which < 0)
        return call;
      static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
      keyType = cryptoString(t->loc(), types[which]);
      has |= 1;
    }
    if (Expr *c = options.lookup("cipher")) {
      cipherName = checkArg(c, Type::string());
      has |= 2;
    }
    if (Expr *p = options.lookup("passphrase")) {
      passphrase = cryptoData(p, "passphrase", true, false, true);
      has |= 4;
      if (passphrase->type->isError())
        return call;
    }
    return cryptoCall(call, format == 0 ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer,
                      {self, cryptoInt(loc, has), keyType, cipherName, passphrase},
                      format == 0 ? Type::string() : buffer);
  }
  if (kind == Crypto::KeyObject && name == "equals") {
    if (!checkArity(call, loc, 1, 1))
      return call;
    Expr *other = cryptoKeyObject(args[0], "other key");
    if (other->type->isError())
      return call;
    return cryptoCall(call, CryptoFn::KeyEquals, {self, other}, Type::boolTy());
  }
  if (kind == Crypto::Ecdh) {
    // The format of a point: 'uncompressed' (0), 'compressed' or 'hybrid'.
    auto format = [&](size_t index) -> Expr * {
      if (!present(index))
        return cryptoInt(loc, 0);
      int which = cryptoChoice(args[index], "format of the point",
                               {"uncompressed", "compressed", "hybrid"});
      return which < 0 ? nullptr : cryptoInt(loc, which);
    };
    if (name == "generateKeys" || name == "getPublicKey") {
      if (!checkArity(call, loc, 0, 2))
        return call;
      Expr *f = format(1);
      if (!f)
        return call;
      return bytesOrText(name == "generateKeys" ? CryptoFn::EcdhGenerateKeys
                                                : CryptoFn::EcdhGetPublicKey,
                         {self, f}, 0);
    }
    if (name == "getPrivateKey") {
      if (!checkArity(call, loc, 0, 1))
        return call;
      return bytesOrText(CryptoFn::EcdhGetPrivateKey, {self}, 0);
    }
    if (name == "setPrivateKey") {
      if (!checkArity(call, loc, 1, 2))
        return call;
      Expr *key = cryptoData(args[0], "private key", true, false, true);
      Expr *encoding = cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding)
        return call;
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
        encoding = cryptoInt(loc, 0);
      return cryptoCall(call, CryptoFn::EcdhSetPrivateKey, {self, key, encoding}, type);
    }
    if (name == "computeSecret") {
      if (!checkArity(call, loc, 1, 3))
        return call;
      Expr *key = cryptoData(args[0], "public key", true, false, true);
      Expr *encoding = cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding)
        return call;
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0)
        encoding = cryptoInt(loc, 0);
      return bytesOrText(CryptoFn::EcdhComputeSecret, {self, key, encoding}, 2);
    }
  }
  if (kind == Crypto::X509) {
    if (name == "toString" || name == "toJSON") {
      if (!checkArity(call, loc, 0, 0))
        return call;
      return cryptoCall(call, CryptoFn::X509Pem, {self}, Type::string());
    }
    if (name == "checkHost" || name == "checkEmail" || name == "checkIP") {
      if (!checkArity(call, loc, 1, name == "checkIP" ? 1 : 2))
        return call;
      Expr *value = checkArg(args[0], Type::string());
      llvm::StringMap<Expr *> options;
      std::vector<llvm::StringRef> allowed = {"subject"};
      if (name == "checkHost")
        allowed.insert(allowed.end(), {"wildcards", "partialWildcards", "multiLabelWildcards",
                                       "singleLabelSubdomains"});
      if (present(1) && !cryptoOptions(args[1], name, allowed, options))
        return call;
      int subject = 0;
      if (Expr *s = options.lookup("subject"))
        if ((subject = cryptoChoice(s, "subject option", {"default", "always", "never"})) < 0)
          return call;
      const Type *optional = Type::optional(Type::string());
      if (name == "checkIP")
        return cryptoCall(call, CryptoFn::X509CheckIp, {self, value}, optional);
      if (name == "checkEmail")
        return cryptoCall(call, CryptoFn::X509CheckEmail, {self, value, cryptoInt(loc, subject)},
                          optional);
      std::vector<Expr *> hostArgs = {self, value, cryptoInt(loc, subject)};
      int has = 0;
      std::vector<Expr *> flags;
      llvm::StringRef names[] = {"wildcards", "partialWildcards", "multiLabelWildcards",
                                 "singleLabelSubdomains"};
      for (int i = 0; i < 4; ++i) {
        Expr *flag = options.lookup(names[i]);
        has |= flag ? 1 << i : 0;
        flags.push_back(flag ? checkArg(flag, Type::boolTy()) : cryptoBool(loc, false));
      }
      hostArgs.push_back(cryptoInt(loc, has));
      hostArgs.insert(hostArgs.end(), flags.begin(), flags.end());
      return cryptoCall(call, CryptoFn::X509CheckHost, std::move(hostArgs), optional);
    }
    if (name == "checkIssued" || name == "checkPrivateKey" || name == "verify") {
      if (!checkArity(call, loc, 1, 1))
        return call;
      Expr *other = name == "checkIssued" ? checkExpr(args[0]) : cryptoKeyObject(args[0], "key");
      if (name == "checkIssued" && !other->type->isError() &&
          other->type->crypto() != Crypto::X509) {
        diags_.error(other->loc(),
                     "'checkIssued' takes an X509Certificate; found '" + other->type->name() + "'");
        return call;
      }
      if (other->type->isError())
        return call;
      CryptoFn fn = name == "checkIssued"       ? CryptoFn::X509CheckIssued
                    : name == "checkPrivateKey" ? CryptoFn::X509CheckPrivateKey
                                                : CryptoFn::X509Verify;
      return cryptoCall(call, fn, {self, other}, Type::boolTy());
    }
  }
  // In node these are streams too; tz has no streams yet (docs/streams.md).
  static const llvm::StringLiteral streamMethods[] = {
      "write",  "end",    "pipe",   "unpipe", "read",           "on",     "once",
      "off",    "emit",   "pause",  "resume", "setEncoding",    "destroy", "addListener",
      "cork",   "uncork", "removeListener",   "setDefaultEncoding"};
  if ((hash || cipher) && llvm::is_contained(streamMethods, name)) {
    const char *instead = kind == Crypto::Sign     ? "'update(data)' and 'sign(key)'"
                          : kind == Crypto::Verify ? "'update(data)' and 'verify(key, signature)'"
                          : cipher                 ? "'update(data)' and 'final()'"
                                                   : "'update(data)' and 'digest()'";
    diags_.error(loc, "'" + name + "' of " + type->name() +
                          " is not supported yet: in node it is a stream too, and tz has no "
                          "streams yet (docs/streams.md); use " +
                          instead);
    return loosely();
  }
  diags_.error(loc, "property '" + name + "' does not exist on type '" + type->name() + "'; it " +
                        "has " + methodsOf(kind));
  return loosely();
}

Expr *Sema::checkCryptoMember(MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  auto read = [&](CryptoFn fn, const Type *result, std::vector<Expr *> args = {}) {
    args.insert(args.begin(), member->object);
    auto *call = ctx_.make<CallExpr>(loc, member, std::vector<Expr *>{});
    return cryptoCall(call, fn, std::move(args), result);
  };
  if (kind == Crypto::KeyObject) {
    if (name == "type")
      return read(CryptoFn::KeyType, Type::string());
    if (name == "asymmetricKeyType")
      return read(CryptoFn::KeyAsymmetricType, Type::optional(Type::string()));
    if (name == "symmetricKeySize")
      return read(CryptoFn::KeySymmetricSize, Type::optional(Type::intTy()));
    if (name == "asymmetricKeyDetails") {
      diags_.error(loc, "'asymmetricKeyDetails' is not supported yet: node gives the exponent of "
                        "RSA as a bigint, which tz has not");
      member->type = Type::error();
      return member;
    }
  }
  if (kind == Crypto::X509) {
    struct Property {
      llvm::StringLiteral name;
      CryptoFn fn;
      int bits;
    };
    static const Property properties[] = {
        {"subject", CryptoFn::X509Subject, 0},
        {"issuer", CryptoFn::X509Issuer, 0},
        {"validFrom", CryptoFn::X509ValidFrom, 0},
        {"validTo", CryptoFn::X509ValidTo, 0},
        {"validFromDate", CryptoFn::X509ValidFromDate, 0},
        {"validToDate", CryptoFn::X509ValidToDate, 0},
        {"serialNumber", CryptoFn::X509SerialNumber, 0},
        {"fingerprint", CryptoFn::X509Fingerprint, 1},
        {"fingerprint256", CryptoFn::X509Fingerprint, 256},
        {"fingerprint512", CryptoFn::X509Fingerprint, 512},
        {"subjectAltName", CryptoFn::X509SubjectAltName, 0},
        {"infoAccess", CryptoFn::X509InfoAccess, 0},
        {"keyUsage", CryptoFn::X509KeyUsage, 0},
        {"ca", CryptoFn::X509Ca, 0},
        {"raw", CryptoFn::X509Raw, 0},
        {"publicKey", CryptoFn::X509PublicKey, 0},
    };
    for (const Property &p : properties) {
      if (p.name != name)
        continue;
      const Type *result =
          p.fn == CryptoFn::X509ValidFromDate || p.fn == CryptoFn::X509ValidToDate ? Type::date()
          : p.fn == CryptoFn::X509SubjectAltName || p.fn == CryptoFn::X509InfoAccess
              ? Type::optional(Type::string())
          : p.fn == CryptoFn::X509KeyUsage  ? Type::optional(Type::array(Type::string()))
          : p.fn == CryptoFn::X509Ca        ? Type::boolTy()
          : p.fn == CryptoFn::X509Raw       ? Type::bytesType(Type::Bytes::Buffer)
          : p.fn == CryptoFn::X509PublicKey ? Type::cryptoType(Crypto::KeyObject)
                                            : Type::string();
      if (p.bits)
        return read(p.fn, result, {cryptoInt(loc, p.bits)});
      return read(p.fn, result);
    }
    if (name == "issuerCertificate") {
      diags_.error(loc, "'issuerCertificate' comes only with TLS (docs/net.md)");
      member->type = Type::error();
      return member;
    }
  }
  diags_.error(loc, "property '" + name + "' of " + type->name() + " can only be called; it has " +
                        methodsOf(kind));
  member->type = Type::error();
  return member;
}

// new X509Certificate(buffer), the one class of node:crypto made with new.
Expr *Sema::checkCryptoNew(NewExpr *expr, llvm::StringRef cls) {
  expr->type = Type::error();
  if (cls != "X509Certificate") {
    diags_.error(expr->loc(), "'new " + cls + "' is not how node makes one: " +
                                  (cls == "KeyObject" ? "use 'createSecretKey', 'createPublicKey' "
                                                        "or 'createPrivateKey'"
                                   : cls == "ECDH"    ? "use 'createECDH(curve)'"
                                   : cls == "Sign" || cls == "Verify"
                                       ? "use 'create" + cls.str() + "(algorithm)'"
                                   : cls == "Hash" || cls == "Hmac"
                                       ? "use 'create" + cls.str() + "(algorithm)'"
                                       : "use 'create" + cls.str() + "(algorithm, key, iv)'"));
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    return expr;
  }
  if (expr->args.size() != 1) {
    diags_.error(expr->loc(), "'new X509Certificate' takes the certificate, PEM or DER");
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    return expr;
  }
  Expr *data = cryptoData(expr->args[0], "certificate", true, false, true);
  if (data->type->isError())
    return expr;
  auto *callee = ctx_.make<NameExpr>(expr->loc(), "X509Certificate");
  auto *call = ctx_.make<CallExpr>(expr->loc(), callee, std::vector<Expr *>{});
  return cryptoCall(call, CryptoFn::X509New, {data}, Type::cryptoType(Crypto::X509));
}

//===----------------------------------------------------------------------===//
// Web Crypto: the global crypto
//===----------------------------------------------------------------------===//

Expr *Sema::checkWebCryptoCall(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "randomUUID" || name == "getRandomValues")
    return checkCryptoCall(call, name, member->loc());
  call->type = Type::error();
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  if (name == "subtle")
    diags_.error(member->loc(), "'crypto.subtle' is an object, not a function");
  else
    diags_.error(member->loc(), "'crypto." + name +
                                    "' does not exist; the global crypto has "
                                    "'randomUUID', 'getRandomValues' and "
                                    "'subtle'");
  return call;
}

Expr *Sema::checkWebCryptoMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->name == "subtle")
    return subtleObject(member);
  if (member->name == "randomUUID" || member->name == "getRandomValues")
    diags_.error(member->loc(), "'crypto." + member->name + "' can only be called");
  else
    diags_.error(member->loc(), "'crypto." + member->name +
                                    "' does not exist; the global crypto "
                                    "has 'randomUUID', 'getRandomValues' "
                                    "and 'subtle'");
  return member;
}

} // namespace tz
