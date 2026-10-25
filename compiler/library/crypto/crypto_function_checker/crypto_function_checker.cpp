#include "compiler/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/library/crypto/key_pair_checker/key_pair_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

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

CryptoFunctionChecker::CryptoFunctionChecker(Checker &checker) : checker_(checker) {}

Expr *CryptoFunctionChecker::checkCryptoCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (llvm::is_contained(kClasses, fn)) {
    checker_.diags_.error(
        loc, "'" + fn + "' of 'node:crypto' is a class, not a function" +
                 (fn == "X509Certificate" ? "; make one with 'new X509Certificate(pem)'" : ""));
    return loosely();
  }
  if (fn == "constants") {
    checker_.diags_.error(
        loc, "'constants' of 'node:crypto' is used as 'constants.RSA_PKCS1_PSS_PADDING' "
             "and the like");
    return loosely();
  }
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *keyObject = Type::cryptoType(Crypto::KeyObject);
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  llvm::StringMap<Expr *> options;
  auto optionsAt = [&](size_t i, llvm::ArrayRef<llvm::StringRef> allowed) {
    return !present(i) || checker_.cryptoArgs().cryptoOptions(args[i], fn, allowed, options);
  };

  if (fn == "createHash") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], false);
    if (!optionsAt(1, {"outputLength"}) || !md) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HashNew,
                                            {md, length
                                                     ? checker_.cryptoArgs().cryptoNumber(length)
                                                     : checker_.cryptoArgs().cryptoDouble(loc, -1)},
                                            Type::cryptoType(Crypto::Hash));
  }
  if (fn == "createHmac") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = checker_.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    if (!md || key->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HmacNew,
                                            {md, key, checker_.cryptoArgs().cryptoInt(loc, 0)},
                                            Type::cryptoType(Crypto::Hmac));
  }
  if (fn == "hash") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 3)) {
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], false);
    Expr *data = checker_.cryptoArgs().cryptoData(args[1], "data", true, false, false);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 2, fn, 6); // hex
    if (!md || data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HashOnce, {md, data}, buffer);
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HashOnceText, {md, data, encoding},
                                            Type::string());
  }
  if (fn == "randomBytes") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2) ||
        checker_.cryptoArgs().rejectCryptoCallback(call, 1, fn, "'const bytes = randomBytes(n)'")) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::RandomBytes,
                                            {checker_.cryptoArgs().cryptoNumber(args[0])}, buffer);
  }
  if (fn == "randomUUID") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1) ||
        !optionsAt(0, {"disableEntropyCache"})) {
      return call;
    }
    if (Expr *cache = options.lookup("disableEntropyCache")) {
      checker_.argumentChecker().checkArg(cache, Type::boolTy());
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::RandomUuid, {}, Type::string());
  }
  if (fn == "randomInt") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 3)) {
      return call;
    }
    size_t last = args.size() - 1;
    if (args.size() > 1 &&
        checker_.cryptoArgs().rejectCryptoCallback(call, last, fn, "'const n = randomInt(max)'")) {
      return call;
    }
    if (args.size() == 3) {
      checker_.diags_.error(args[2]->loc(), "'randomInt' takes a minimum and a maximum");
      return loosely();
    }
    Expr *min = args.size() == 2 ? checker_.exprChecker().checkExpr(args[0], Type::intTy())
                                 : checker_.cryptoArgs().cryptoInt(loc, 0);
    Expr *max = checker_.exprChecker().checkExpr(args.back(), Type::intTy());
    if (min->type->isInt() && max->type->isInt()) {
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::RandomInt32, {min, max},
                                              Type::intTy());
    }
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::RandomInt,
        {checker_.cryptoArgs().cryptoNumber(min), checker_.cryptoArgs().cryptoNumber(max)},
        Type::doubleTy());
  }
  if (fn == "randomFillSync" || fn == "getRandomValues") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, fn == "randomFillSync" ? 3 : 1)) {
      return call;
    }
    Expr *target = checker_.exprChecker().checkExpr(args[0]);
    const Type *type = target->type;
    if (type->isError()) {
      return call;
    }
    if (fn == "getRandomValues") {
      static const Type::Bytes floats[] = {Type::Bytes::Float16, Type::Bytes::Float32,
                                           Type::Bytes::Float64};
      if (!type->isTypedArray() || llvm::is_contained(floats, type->bytes())) {
        checker_.diags_.error(target->loc(),
                              "'getRandomValues' fills an array of integers (a Uint8Array, "
                              "an Int32Array, a Buffer...); found '" +
                                  type->name() + "'");
        return call;
      }
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::GetRandomValues, {target}, type);
    }
    if (!type->isByteView() && type->bytes() != Type::Bytes::ArrayBuffer) {
      checker_.diags_.error(target->loc(),
                            "'randomFillSync' fills bytes (a Buffer, a typed array, a "
                            "DataView or an ArrayBuffer); found '" +
                                type->name() + "'");
      return call;
    }
    int has = (present(1) ? 1 : 0) | (present(2) ? 2 : 0);
    Expr *offset = present(1) ? checker_.cryptoArgs().cryptoNumber(args[1])
                              : checker_.cryptoArgs().cryptoDouble(loc, 0);
    Expr *size = present(2) ? checker_.cryptoArgs().cryptoNumber(args[2])
                            : checker_.cryptoArgs().cryptoDouble(loc, 0);
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::RandomFill,
        {target, offset, size, checker_.cryptoArgs().cryptoInt(loc, has)}, type);
  }
  if (fn == "timingSafeEqual") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *a = checker_.cryptoArgs().cryptoData(args[0], "first argument", false, false, true);
    Expr *b = checker_.cryptoArgs().cryptoData(args[1], "second argument", false, false, true);
    if (a->type->isError() || b->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::TimingSafeEqual, {a, b},
                                            Type::boolTy());
  }
  if (fn == "pbkdf2Sync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 5, 5)) {
      return call;
    }
    Expr *password = checker_.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = checker_.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *iterations = checker_.cryptoArgs().cryptoNumber(args[2]),
         *keylen = checker_.cryptoArgs().cryptoNumber(args[3]);
    Expr *md = checker_.cryptoArgs().cryptoMd(args[4], true);
    if (!md || password->type->isError() || salt->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::Pbkdf2,
                                            {password, salt, iterations, keylen, md}, buffer);
  }
  if (fn == "hkdfSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 5, 5)) {
      return call;
    }
    Expr *md = checker_.cryptoArgs().cryptoMd(args[0], true);
    Expr *key = checker_.cryptoArgs().cryptoData(args[1], "ikm", true, true, true);
    Expr *salt = checker_.cryptoArgs().cryptoData(args[2], "salt", true, false, true);
    Expr *info = checker_.cryptoArgs().cryptoData(args[3], "info", true, false, true);
    Expr *keylen = checker_.cryptoArgs().cryptoNumber(args[4]);
    if (!md || key->type->isError() || salt->type->isError() || info->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::Hkdf, {md, key, salt, info, keylen},
                                            Type::bytesType(Type::Bytes::ArrayBuffer));
  }
  if (fn == "scryptSync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 3, 4)) {
      return call;
    }
    Expr *password = checker_.cryptoArgs().cryptoData(args[0], "password", true, true, true);
    Expr *salt = checker_.cryptoArgs().cryptoData(args[1], "salt", true, false, true);
    Expr *keylen = checker_.cryptoArgs().cryptoNumber(args[2]);
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
        checker_.diags_.error(b->loc(), "option '" + pairs[i].first +
                                            "' cannot be used together with '" + pairs[i].second +
                                            "': they are one option");
        return call;
      }
      values[i] = a || b ? checker_.cryptoArgs().cryptoNumber(a ? a : b)
                         : checker_.cryptoArgs().cryptoDouble(loc, 0);
    }
    Expr *maxmem = options.lookup("maxmem");
    if (password->type->isError() || salt->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::Scrypt,
                                            {password, salt, keylen, values[0], values[1],
                                             values[2],
                                             maxmem ? checker_.cryptoArgs().cryptoNumber(maxmem)
                                                    : checker_.cryptoArgs().cryptoDouble(loc, 0)},
                                            buffer);
  }
  if (fn == "argon2Sync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    Expr *type, *name;
    if (literalOf(args[0])) {
      int which = checker_.cryptoArgs().cryptoChoice(args[0], "algorithm of Argon2",
                                                     {"argon2d", "argon2i", "argon2id"});
      if (which < 0) {
        return call;
      }
      type = checker_.cryptoArgs().cryptoInt(loc, which);
      name = checker_.cryptoArgs().cryptoString(loc, "");
    } else {
      type = checker_.cryptoArgs().cryptoInt(loc, 0xffffffff); // -1: the name is known only now
      name = checker_.argumentChecker().checkArg(args[0], Type::string());
    }
    if (!checker_.cryptoArgs().cryptoOptions(args[1], fn,
                                             {"message", "nonce", "parallelism", "tagLength",
                                              "memory", "passes", "secret", "associatedData"},
                                             options)) {
      return call;
    }
    for (llvm::StringRef required :
         {"message", "nonce", "parallelism", "tagLength", "memory", "passes"}) {
      if (!options.lookup(required)) {
        checker_.diags_.error(args[1]->loc(),
                              "the parameters of 'argon2Sync' need '" + required + "'");
        return call;
      }
    }
    Expr *message =
        checker_.cryptoArgs().cryptoData(options["message"], "message", true, false, true);
    Expr *nonce = checker_.cryptoArgs().cryptoData(options["nonce"], "nonce", true, false, true);
    Expr *parallelism = checker_.cryptoArgs().cryptoNumber(options["parallelism"]);
    Expr *tagLength = checker_.cryptoArgs().cryptoNumber(options["tagLength"]);
    Expr *memory = checker_.cryptoArgs().cryptoNumber(options["memory"]);
    Expr *passes = checker_.cryptoArgs().cryptoNumber(options["passes"]);
    Expr *secret =
        options.lookup("secret")
            ? checker_.cryptoArgs().cryptoData(options["secret"], "secret", true, false, true)
            : checker_.cryptoArgs().cryptoUndefined(loc);
    Expr *data = options.lookup("associatedData")
                     ? checker_.cryptoArgs().cryptoData(options["associatedData"], "associatedData",
                                                        true, false, true)
                     : checker_.cryptoArgs().cryptoUndefined(loc);
    for (Expr *e : {message, nonce, secret, data}) {
      if (e->type->isError()) {
        return call;
      }
    }
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::Argon2,
        {type, name, message, nonce, parallelism, tagLength, memory, passes, secret, data}, buffer);
  }
  if (fn == "createCipheriv" || fn == "createDecipheriv") {
    if (!checker_.argumentChecker().checkArity(call, loc, 3, 4)) {
      return call;
    }
    std::optional<std::string> name = literalOf(args[0]);
    Expr *cipher = checker_.cryptoArgs().cryptoCipher(args[0]);
    Expr *key = checker_.cryptoArgs().cryptoData(args[1], "key", true, true, true);
    Expr *iv = checker_.cryptoArgs().cryptoData(args[2], "iv", true, false, true);
    if (!optionsAt(3, {"authTagLength"}) || !cipher || key->type->isError() ||
        iv->type->isError()) {
      return call;
    }
    Expr *tag = options.lookup("authTagLength");
    if (!tag && name && llvm::StringRef(*name).lower().find("ccm") != std::string::npos) {
      checker_.diags_.error(args[0]->loc(), "'" + *name +
                                                "' needs the length of its tag: '{ "
                                                "authTagLength: 16 }'");
      return call;
    }
    bool decrypt = fn == "createDecipheriv";
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::CipherNew,
        {cipher, key, iv,
         tag ? checker_.cryptoArgs().cryptoNumber(tag)
             : checker_.cryptoArgs().cryptoDouble(loc, -1),
         checker_.cryptoArgs().cryptoBool(loc, decrypt)},
        Type::cryptoType(decrypt ? Crypto::Decipheriv : Crypto::Cipheriv));
  }
  if (fn == "getHashes" || fn == "getCiphers" || fn == "getCurves") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 0)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call,
                                            fn == "getHashes"    ? CryptoFn::GetHashes
                                            : fn == "getCiphers" ? CryptoFn::GetCiphers
                                                                 : CryptoFn::GetCurves,
                                            {}, Type::array(Type::string()));
  }
  if (fn == "createSecretKey") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *key = checker_.cryptoArgs().cryptoData(args[0], "key", true, false, true);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, fn, 0);
    if (key->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::SecretKey, {key, encoding}, keyObject);
  }
  if (fn == "createPublicKey" || fn == "createPrivateKey") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    bool priv = fn == "createPrivateKey";
    Expr *key, *encoding = checker_.cryptoArgs().cryptoInt(loc, 0),
               *type = checker_.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = checker_.cryptoArgs().cryptoUndefined(loc);
    int format = 0, has = 0;
    if (isa<ObjectLitExpr>(args[0])) {
      if (!checker_.cryptoArgs().cryptoOptions(
              args[0], fn, {"key", "format", "type", "passphrase", "encoding"}, options)) {
        return call;
      }
      if (Expr *f = options.lookup("format")) {
        if ((format = checker_.cryptoArgs().cryptoChoice(f, "format of the key",
                                                         {"pem", "der", "jwk"})) < 0) {
          return call;
        }
      }
      if (!options.lookup("key")) {
        checker_.diags_.error(args[0]->loc(), "the options of '" + fn + "' need 'key'");
        return call;
      }
      key = format == 2
                ? checker_.argumentChecker().checkArg(options["key"], Type::json())
                : checker_.cryptoArgs().cryptoData(options["key"], "key", true, !priv, true);
      if (Expr *t = options.lookup("type")) {
        int which = checker_.cryptoArgs().cryptoChoice(t, "type of the key",
                                                       {"pkcs1", "spki", "pkcs8", "sec1"});
        if (which < 0) {
          return call;
        }
        static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = checker_.cryptoArgs().cryptoString(t->loc(), types[which]);
        has |= 1;
      }
      if (Expr *p = options.lookup("passphrase")) {
        passphrase = checker_.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
        has |= 2;
      }
      if (Expr *e = options.lookup("encoding")) {
        Expr *number = literalOf(e) ? checker_.bytesChecker().checkBufferEncoding(e) : nullptr;
        if (!number || !isa<IntLitExpr>(number)) {
          checker_.diags_.error(e->loc(),
                                "the encoding of the key is written as a literal, e.g. 'hex'");
          return call;
        }
        encoding = number;
      }
    } else {
      key = checker_.cryptoArgs().cryptoData(args[0], "key", true, !priv, true);
    }
    if (key->type->isError() || passphrase->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::CreateKey,
                                            {checker_.cryptoArgs().cryptoBool(loc, priv), key,
                                             encoding, checker_.cryptoArgs().cryptoInt(loc, format),
                                             checker_.cryptoArgs().cryptoInt(loc, has), type,
                                             passphrase},
                                            keyObject);
  }
  if (fn == "generateKeyPairSync") {
    return checker_.keyPairChecker().checkGenerateKeyPair(call, loc);
  }
  if (fn == "generateKeySync") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 2)) {
      return call;
    }
    int which = checker_.cryptoArgs().cryptoChoice(args[0], "type of the key", {"hmac", "aes"});
    if (which < 0 || !checker_.cryptoArgs().cryptoOptions(args[1], fn, {"length"}, options)) {
      return call;
    }
    if (!options.lookup("length")) {
      checker_.diags_.error(args[1]->loc(),
                            "'generateKeySync' needs the length of the key in bits: '{ "
                            "length: 256 }'");
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::GenerateSecretKey,
                                            {checker_.cryptoArgs().cryptoBool(loc, which == 1),
                                             checker_.cryptoArgs().cryptoNumber(options["length"])},
                                            keyObject);
  }
  if (fn == "sign" || fn == "verify") {
    bool verify = fn == "verify";
    size_t count = verify ? 4 : 3;
    if (!checker_.argumentChecker().checkArity(call, loc, unsigned(count), unsigned(count + 1)) ||
        checker_.cryptoArgs().rejectCryptoCallback(
            call, count, fn,
            verify ? "'const ok = verify(algorithm, data, key, signature)'"
                   : "'const signature = sign(algorithm, data, key)'")) {
      return call;
    }
    Expr *algorithm = isa<UndefinedExpr>(args[0])
                          ? checker_.cryptoArgs().cryptoUndefined(loc)
                          : checker_.argumentChecker().checkArg(args[0], Type::string());
    Expr *data = checker_.cryptoArgs().cryptoData(args[1], "data", true, false, true);
    std::vector<Expr *> signArgs = {algorithm, data};
    if (!checker_.cryptoArgs().cryptoSignKey(args[2], fn, !verify, signArgs)) {
      return call;
    }
    if (verify) {
      Expr *signature = checker_.cryptoArgs().cryptoData(args[3], "signature", true, false, true);
      if (signature->type->isError()) {
        return call;
      }
      signArgs.insert(signArgs.begin() + 3, signature);
    }
    if (data->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, verify ? CryptoFn::Verify : CryptoFn::Sign,
                                            std::move(signArgs), verify ? Type::boolTy() : buffer);
  }
  if (fn == "createSign" || fn == "createVerify") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    Expr *name = args[0];
    if (literalOf(name) && !checker_.cryptoArgs().cryptoMd(name, true)) {
      return call;
    }
    bool verify = fn == "createVerify";
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::SignNew,
        {checker_.argumentChecker().checkArg(name, Type::string()),
         checker_.cryptoArgs().cryptoBool(loc, verify)},
        Type::cryptoType(verify ? Crypto::Verify : Crypto::Sign));
  }
  if (fn == "createECDH") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::EcdhNew, {checker_.argumentChecker().checkArg(args[0], Type::string())},
        Type::cryptoType(Crypto::Ecdh));
  }
  if (fn == "diffieHellman") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1) ||
        !checker_.cryptoArgs().cryptoOptions(args[0], fn, {"privateKey", "publicKey"}, options)) {
      return call;
    }
    if (!options.lookup("privateKey") || !options.lookup("publicKey")) {
      checker_.diags_.error(args[0]->loc(), "'diffieHellman' needs '{ privateKey, publicKey }'");
      return call;
    }
    Expr *priv = checker_.cryptoArgs().cryptoKeyObject(options["privateKey"], "private key");
    Expr *pub = checker_.cryptoArgs().cryptoKeyObject(options["publicKey"], "public key");
    if (priv->type->isError() || pub->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::DiffieHellman, {priv, pub}, buffer);
  }
  checker_.diags_.error(loc, cryptoExportProblem(fn, "node:crypto"));

  return loosely();
}

} // namespace tz
