#include "compiler/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::string utf8Of(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

// Text known when compiling: a literal, or a template without values.
std::optional<std::string> literalOf(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return utf8Of(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return utf8Of(tmpl->quasis[0]);
  }

  return std::nullopt;
}

} // namespace

CryptoFunctionChecker::CryptoFunctionChecker(Sema &sema) : sema_(sema) {}

Expr *CryptoFunctionChecker::checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    sema_.diags_.error(loc,
                 "'" + fn + "' of 'node:crypto' is a class, not a function" +
                     (fn == "X509Certificate" ? "; make one with 'new X509Certificate(pem)'" : ""));
    return loosely();
  }
  if (fn == "constants") {
    sema_.diags_.error(loc, "'constants' of 'node:crypto' is used as 'constants.RSA_PKCS1_PSS_PADDING' "
                      "and the like");
    return loosely();
  }
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) || sema_.cryptoArgs().cryptoOptions(args[i], fn, allowed, options);
  };

  if (fn == "createHash") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *md = sema_.cryptoArgs().cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashNew,
                      {md, length ? sema_.cryptoArgs().cryptoNumber(length) : sema_.cryptoArgs().cryptoDouble(loc, -1)},
                      Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *md = sema_.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = sema_.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HmacNew, {md, key, sema_.cryptoArgs().cryptoInt(loc, 0)},
                      Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 3)) {
      return call;
    }
    Expr *md = sema_.cryptoArgs().cryptoMd(args[0], false);
    Expr *data = sema_.cryptoArgs().cryptoData(args[1], "data", true, false, false);
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 2, fn, 6); // hex
    if (!md || data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashOnce, {md, data}, buffer);
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashOnceText, {md, data, encoding}, Type::string());
  }
  if (fn == "randomBytes") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2) ||
        sema_.cryptoArgs().rejectCryptoCallback(call, 1, fn, "'const bytes = randomBytes(n)'")) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::RandomBytes, {sema_.cryptoArgs().cryptoNumber(args[0])}, buffer);
  }
  if (fn == "randomUUID") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 1) || !optionsAt(0, {"disableEntropyCache"})) {
      return call;
    }
    if (Expr *cache = options.lookup("disableEntropyCache")) {
      sema_.argumentChecker().checkArg(cache, Type::boolTy());
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 3)) {
      return call;
    }
    size_t last = args.size() - 1;
    if (args.size() > 1 && sema_.cryptoArgs().rejectCryptoCallback(call, last, fn, "'const n = randomInt(max)'")) {
      return call;
    }
    if (args.size() == 3) {
      sema_.diags_.error(args[2]->loc(), "'randomInt' takes a minimum and a maximum");
      return loosely();
    }
    Expr *min = args.size() == 2 ? sema_.exprChecker().checkExpr(args[0], Type::intTy()) : sema_.cryptoArgs().cryptoInt(loc, 0);
    Expr *max = sema_.exprChecker().checkExpr(args.back(), Type::intTy());
    if (min->type->isInt() && max->type->isInt()) {
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::RandomInt32, {min, max}, Type::intTy());
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::RandomInt, {sema_.cryptoArgs().cryptoNumber(min), sema_.cryptoArgs().cryptoNumber(max)},
                      Type::doubleTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, fn == "randomFillSync" ? 3 : 1)) {
      return call;
    }
    Expr *target = sema_.exprChecker().checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError()) {
      return call;
    }
    if (fn == "getRandomValues") {
      static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                           Type::Bytes::Float64};
      if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
        sema_.diags_.error(target->loc(), "'getRandomValues' fills an array of integers (a Uint8Array, "
                                    "an Int32Array, a Buffer...); found '" +
                                        type->name() + "'");
        return call;
      }
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
      sema_.diags_.error(target->loc(), "'randomFillSync' fills bytes (a Buffer, a typed array, a "
                                  "DataView or an ArrayBuffer); found '" +
                                      type->name() + "'");
      return call;
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = present(1) ? sema_.cryptoArgs().cryptoNumber(args[1]) : sema_.cryptoArgs().cryptoDouble(loc, 0);
    Expr *size = present(2) ? sema_.cryptoArgs().cryptoNumber(args[2]) : sema_.cryptoArgs().cryptoDouble(loc, 0);
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::RandomFill, {target, offset, size, sema_.cryptoArgs().cryptoInt(loc, has)},
                      type);
  }
  if (fn == "timingSafeEqual") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *a = sema_.cryptoArgs().cryptoData(args[0], "first argument", false, false, true);
    Expr *b = sema_.cryptoArgs().cryptoData(args[1], "second argument", false, false, true);
    if (a->type->isError() || b->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b}, Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 5, 5)) {
      return call;
    }
    Expr *password = sema_.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = sema_.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = sema_.cryptoArgs().cryptoNumber(args[2]), *keylen = sema_.cryptoArgs().cryptoNumber(args[3]);
    Expr *md = sema_.cryptoArgs().cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::Pbkdf2, {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 5, 5)) {
      return call;
    }
    Expr *md = sema_.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = sema_.cryptoArgs().cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = sema_.cryptoArgs().cryptoData(args[2], "salt", true, false, true);
    Expr *info = sema_.cryptoArgs().cryptoData(args[3], "info", true, false, true);
    Expr *keylen = sema_.cryptoArgs().cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                      Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 3, 4)) {
      return call;
    }
    Expr *password = sema_.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = sema_.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = sema_.cryptoArgs().cryptoNumber(args[2]);
    if (!optionsAt(3, {"N", "r", "p", "maxmem", "cost", "blockSize", "parallelization"})) {
      return call;
    }
    // N, r and p have a second name each; node rejects both together.
    static const std::pair<llvm::StringLiteral, llvm::StringLiteral> pairs[] = {
        {"N", "cost"}, {"r", "blockSize"}, {"p", "parallelization"}};
    Expr *values[3];
    for (int i = 0; i < 3; ++i) {
      Expr *a = options.lookup(pairs[i].first), *b = options.lookup(pairs[i].second);
      if (a && b) {
        sema_.diags_.error(b->loc(), "option '" + pairs[i].first + "' cannot be used together with '" +
                                   pairs[i].second + "': they are one option");
        return call;
      }
      values[i] = a || b ? sema_.cryptoArgs().cryptoNumber(a ? a : b) : sema_.cryptoArgs().cryptoDouble(loc, 0);
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::Scrypt,
                      {password, salt, keylen, values[0], values[1], values[2],
                       maxmem ? sema_.cryptoArgs().cryptoNumber(maxmem) : sema_.cryptoArgs().cryptoDouble(loc, 0)},
                      buffer);
  }
  if (fn == "argon2Sync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *type, *name;
    if (literalOf(args[0])) {
      int which = sema_.cryptoArgs().cryptoChoice(args[0], "algorithm of Argon2", {"argon2d", "argon2i", "argon2id"});
      if (which < 0) {
        return call;
      }
      type = sema_.cryptoArgs().cryptoInt(loc, which);
      name = sema_.cryptoArgs().cryptoString(loc, "");
    } else {
      type = sema_.cryptoArgs().cryptoInt(loc, 0xffffffff); // -1: the name is known only now
      name = sema_.argumentChecker().checkArg(args[0], Type::string());
    }
    if (!sema_.cryptoArgs().cryptoOptions(args[1], fn,
                       {"message", "nonce", "parallelism", "tagLength", "memory", "passes",
                        "secret", "associatedData"},
                       options)) {
      return call;
    }
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
      if (!options.lookup(required)) {
        sema_.diags_.error(args[1]->loc(), "the parameters of 'argon2Sync' need '" + required + "'");
        return call;
      }
    }
    Expr *message = sema_.cryptoArgs().cryptoData(options["message"], "message", true, false, true);
    Expr *nonce = sema_.cryptoArgs().cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = sema_.cryptoArgs().cryptoNumber(options["parallelism"]);
    Expr *tagLength = sema_.cryptoArgs().cryptoNumber(options["tagLength"]);
    Expr *memory = sema_.cryptoArgs().cryptoNumber(options["memory"]);
    Expr *passes = sema_.cryptoArgs().cryptoNumber(options["passes"]);
    Expr *secret = options.lookup("secret")
                       ? sema_.cryptoArgs().cryptoData(options["secret"], "secret", true, false, true)
                       : sema_.cryptoArgs().cryptoUndefined(loc);
    Expr *data = options.lookup("associatedData")
                     ? sema_.cryptoArgs().cryptoData(options["associatedData"], "associatedData", true, false, true)
                     : sema_.cryptoArgs().cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data}) {
      if (e->type->isError()) {
        return call;
      }
    }
    return sema_.cryptoArgs().cryptoCall(
        call, CryptoFn::Argon2,
        {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    if (!sema_.argumentChecker().checkArity(call, loc, 3, 4)) {
      return call;
    }
    std::optional<std::string> name = literalOf(args[0]);
    Expr *cipher = sema_.cryptoArgs().cryptoCipher(args[0]);
    Expr *key = sema_.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    Expr *iv = sema_.cryptoArgs().cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() ||
        iv->type->isError()) {
      return call;
    }
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      sema_.diags_.error(args[0]->loc(), "'" + *name +
                                       "' needs the length of its tag: '{ "
                                       "authTagLength: 16 }'");
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::CipherNew,
                      {cipher, key, iv, tag ? sema_.cryptoArgs().cryptoNumber(tag) : sema_.cryptoArgs().cryptoDouble(loc, -1),
                       sema_.cryptoArgs().cryptoBool(loc, decrypt)},
                      Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 0)) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call,
                      fn == "getHashes"    ? CryptoFn::GetHashes
                      : fn == "getCiphers" ? CryptoFn::GetCiphers
                                           : CryptoFn::GetCurves,
                      {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *key = sema_.cryptoArgs().cryptoData(args[0], "key", true, false, true);
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = sema_.cryptoArgs().cryptoInt(loc, 0), *type = sema_.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = sema_.cryptoArgs().cryptoUndefined(loc);
    int format = 0, has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!sema_.cryptoArgs().cryptoOptions(args[0], fn, {"key", "format", "type", "passphrase", "encoding"},
                         options)) {
        return call;
      }
      if (Expr *f = options.lookup("format")) {
        if ((format = sema_.cryptoArgs().cryptoChoice(f, "format of the key", {"pem", "der", "jwk"})) < 0) {
          return call;
        }
      }
      if (!options.lookup("key")) {
        sema_.diags_.error(args[0]->loc(), "the options of '" + fn + "' need 'key'");
        return call;
      }
      key = format == 2 ? sema_.argumentChecker().checkArg(options["key"], Type::json())
                        : sema_.cryptoArgs().cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        int which = sema_.cryptoArgs().cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0) {
          return call;
        }
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = sema_.cryptoArgs().cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = sema_.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = literalOf(e) ? sema_.bytesChecker().checkBufferEncoding(e) : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          sema_.diags_.error(e->loc(), "the encoding of the key is written as a literal, e.g. 'hex'");
          return call;
        }
        encoding = number;
      }
    } else {
      key = sema_.cryptoArgs().cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::CreateKey,
                      {sema_.cryptoArgs().cryptoBool(loc, priv), key, encoding, sema_.cryptoArgs().cryptoInt(loc, format),
                       sema_.cryptoArgs().cryptoInt(loc, has), type, passphrase},
                      keyObject);
  }
  if (fn == "generateKeyPairSync") {
    return sema_.keyPairChecker().checkGenerateKeyPair(call, loc);
  }
  if (fn == "generateKeySync") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    int which = sema_.cryptoArgs().cryptoChoice(args[0], "type of the key", {"hmac", "aes"});
    if (which < 0 || !sema_.cryptoArgs().cryptoOptions(args[1], fn, {"length"}, options)) {
      return call;
    }
    if (!options.lookup("length")) {
      sema_.diags_.error(args[1]->loc(), "'generateKeySync' needs the length of the key in bits: '{ "
                                   "length: 256 }'");
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::GenerateSecretKey,
                      {sema_.cryptoArgs().cryptoBool(loc, which == 1), sema_.cryptoArgs().cryptoNumber(options["length"])}, keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    if (!sema_.argumentChecker().checkArity(call, loc, unsigned(count), unsigned(count + 1)) ||
        sema_.cryptoArgs().rejectCryptoCallback(call, count, fn,
                             verify ? "'const ok = verify(algorithm, data, key, signature)'"
                                    : "'const signature = sign(algorithm, data, key)'")) {
      return call;
    }
    Expr *algorithm =
        isa<UndefinedExpr>(args[0]) ? sema_.cryptoArgs().cryptoUndefined(loc) : sema_.argumentChecker().checkArg(args[0], Type::string());
    Expr *data = sema_.cryptoArgs().cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!sema_.cryptoArgs().cryptoSignKey(args[2], fn, !verify, signArgs)) {
      return call;
    }
    if (verify) {
      Expr *signature = sema_.cryptoArgs().cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError()) {
        return call;
      }
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, verify ? CryptoFn::Verify : CryptoFn::Sign, std::move(signArgs),
                      verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    Expr *name = args[0];
    if (literalOf(name) && !sema_.cryptoArgs().cryptoMd(name, true)) {
      return call;
    }
    bool verify = fn == "createVerify";
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::SignNew,
                      {sema_.argumentChecker().checkArg(name, Type::string()), sema_.cryptoArgs().cryptoBool(loc, verify)},
                      Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign));
  }
  if (fn == "createECDH") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::EcdhNew, {sema_.argumentChecker().checkArg(args[0], Type::string())},
                      Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 1) ||
        !sema_.cryptoArgs().cryptoOptions(args[0], fn, {"privateKey", "publicKey"}, options)) {
      return call;
    }
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      sema_.diags_.error(args[0]->loc(), "'diffieHellman' needs '{ privateKey, publicKey }'");
      return call;
    }
    Expr *priv = sema_.cryptoArgs().cryptoKeyObject(options["privateKey"], "private key");
    Expr *pub = sema_.cryptoArgs().cryptoKeyObject(options["publicKey"], "public key");
    if (priv->type->isError() || pub->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
  }
  sema_.diags_.error(loc, cryptoExportProblem(fn, "node:crypto"));

  return loosely();
}

} // namespace tz
