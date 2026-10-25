#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
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

CryptoMethodChecker::CryptoMethodChecker(Sema &sema) : sema_(sema) {}

Expr *CryptoMethodChecker::checkCryptoMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  Expr *self = member->object;
  // The bytes of `made` (a call of the receiver's method), or their text in the encoding at
  // args[index].
  auto bytesOrText = [&](CryptoFn fn, std::vector<Expr *> fnArgs, size_t index) -> Expr * {
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, index, name, -1);
    if (!encoding) {
      return call;
    }
    auto *made = sema_.ctx_.make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{});
    sema_.cryptoArgs().cryptoCall(made, fn, std::move(fnArgs), buffer);

    return sema_.cryptoArgs().cryptoText(made, encoding);
  };
  bool hash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
              kind == Crypto::Verify;
  bool cipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;

  if (hash && name == "update") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *data = sema_.cryptoArgs().cryptoData(args[0], "data", true, false, false);
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashUpdate, {self, data, encoding}, type);
  }
  if ((kind == Crypto::Hash || kind == Crypto::Hmac) && name == "digest") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 0, name, -1);
    if (!encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashDigest, {self}, buffer);
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashDigestText, {self, encoding}, Type::string());
  }
  if (kind == Crypto::Hash && name == "copy") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    llvm::StringMap<Expr *> options;
    if (present(0) && !sema_.cryptoArgs().cryptoOptions(args[0], name, {"outputLength"}, options)) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::HashCopy,
                      {self, length ? sema_.cryptoArgs().cryptoNumber(length) : sema_.cryptoArgs().cryptoDouble(loc, -1)}, type);
  }
  if (kind == Crypto::Sign && name == "sign") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    std::vector<Expr *> signArgs = {self};
    if (!sema_.cryptoArgs().cryptoSignKey(args[0], name, true, signArgs)) {
      return call;
    }
    return bytesOrText(CryptoFn::SignFinal, std::move(signArgs), 1);
  }
  if (kind == Crypto::Verify && name == "verify") {
    if (!sema_.argumentChecker().checkArity(call, loc, 2, 3)) {
      return call;
    }
    std::vector<Expr *> verifyArgs = {self};
    if (!sema_.cryptoArgs().cryptoSignKey(args[0], name, false, verifyArgs)) {
      return call;
    }
    Expr *signature = sema_.cryptoArgs().cryptoData(args[1], "signature", true, false, true);
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 2, name, 0);
    if (signature->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
    }
    verifyArgs.insert(verifyArgs.begin() + 2, {signature, encoding});
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::VerifyFinal, std::move(verifyArgs), Type::boolTy());
  }
  if (cipher && name == "update") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 3)) {
      return call;
    }
    Expr *data = sema_.cryptoArgs().cryptoData(args[0], "data", true, false, true);
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
    }
    return bytesOrText(CryptoFn::CipherUpdate, {self, data, encoding}, 2);
  }
  if (cipher && name == "final") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    return bytesOrText(CryptoFn::CipherFinal, {self}, 0);
  }
  if (cipher && name == "setAAD") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *data = sema_.cryptoArgs().cryptoData(args[0], "AAD", false, false, true);
    llvm::StringMap<Expr *> options;
    if (present(1) && !sema_.cryptoArgs().cryptoOptions(args[1], name, {"plaintextLength"}, options)) {
      return call;
    }
    if (data->type->isError()) {
      return call;
    }
    Expr *length = options.lookup("plaintextLength");
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::CipherSetAad,
                      {self, data, length ? sema_.cryptoArgs().cryptoNumber(length) : sema_.cryptoArgs().cryptoDouble(loc, -1)}, type);
  }
  if (kind == Crypto::Cipheriv && name == "getAuthTag") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 0)) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::CipherGetAuthTag, {self}, buffer);
  }
  if (kind == Crypto::Decipheriv && name == "setAuthTag") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *tag = sema_.cryptoArgs().cryptoData(args[0], "tag", true, false, true);
    Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
    if (tag->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::CipherSetAuthTag, {self, tag, encoding}, type);
  }
  if (cipher && name == "setAutoPadding") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    Expr *padding = present(0) ? sema_.argumentChecker().checkArg(args[0], Type::boolTy()) : sema_.cryptoArgs().cryptoBool(loc, true);
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::CipherSetAutoPadding, {self, padding}, type);
  }
  if (kind == Crypto::KeyObject && name == "export") {
    if (!sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    if (!present(0)) {
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    llvm::StringMap<Expr *> options;
    if (!sema_.cryptoArgs().cryptoOptions(args[0], name, {"format", "type", "cipher", "passphrase"}, options)) {
      return call;
    }
    int format = 3; // buffer
    if (Expr *f = options.lookup("format")) {
      if ((format = sema_.cryptoArgs().cryptoChoice(f, "format of the key", {"pem", "der", "jwk", "buffer"})) < 0) {
        return call;
      }
    }
    if (format == 2) {
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::KeyExportJwk, {self}, Type::json());
    }
    if (format == 3) {
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    int has = 0;
    Expr *keyType = sema_.cryptoArgs().cryptoString(loc, ""), *cipherName = sema_.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = sema_.cryptoArgs().cryptoUndefined(loc);
    if (Expr *t = options.lookup("type")) {
      int which = sema_.cryptoArgs().cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
      if (which < 0) {
        return call;
      }
      static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
      keyType = sema_.cryptoArgs().cryptoString(t->loc(), types[which]);
      has |= 1;
    }
    if (Expr *c = options.lookup("cipher")) {
      cipherName = sema_.argumentChecker().checkArg(c, Type::string());
      has |= 2;
    }
    if (Expr *p = options.lookup("passphrase")) {
      passphrase = sema_.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
      has |= 4;
      if (passphrase->type->isError()) {
        return call;
      }
    }
    return sema_.cryptoArgs().cryptoCall(call, format == 0 ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer,
                      {self, sema_.cryptoArgs().cryptoInt(loc, has), keyType, cipherName, passphrase},
                      format == 0 ? Type::string() : buffer);
  }
  if (kind == Crypto::KeyObject && name == "equals") {
    if (!sema_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    Expr *other = sema_.cryptoArgs().cryptoKeyObject(args[0], "other key");
    if (other->type->isError()) {
      return call;
    }
    return sema_.cryptoArgs().cryptoCall(call, CryptoFn::KeyEquals, {self, other}, Type::boolTy());
  }
  if (kind == Crypto::Ecdh) {
    // The format of a point: 'uncompressed' (0), 'compressed' or 'hybrid'.
    auto format = [&](size_t index) -> Expr * {
      if (!present(index)) {
        return sema_.cryptoArgs().cryptoInt(loc, 0);
      }
      int which = sema_.cryptoArgs().cryptoChoice(args[index], "format of the point",
                               {"uncompressed", "compressed", "hybrid"});

      return which < 0 ? nullptr : sema_.cryptoArgs().cryptoInt(loc, which);
    };
    if (name == "generateKeys" || name == "getPublicKey") {
      if (!sema_.argumentChecker().checkArity(call, loc, 0, 2)) {
        return call;
      }
      Expr *f = format(1);
      if (!f) {
        return call;
      }
      return bytesOrText(name == "generateKeys" ? CryptoFn::EcdhGenerateKeys
                                                : CryptoFn::EcdhGetPublicKey,
                         {self, f}, 0);
    }
    if (name == "getPrivateKey") {
      if (!sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
        return call;
      }
      return bytesOrText(CryptoFn::EcdhGetPrivateKey, {self}, 0);
    }
    if (name == "setPrivateKey") {
      if (!sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
        return call;
      }
      Expr *key = sema_.cryptoArgs().cryptoData(args[0], "private key", true, false, true);
      Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
      }
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::EcdhSetPrivateKey, {self, key, encoding}, type);
    }
    if (name == "computeSecret") {
      if (!sema_.argumentChecker().checkArity(call, loc, 1, 3)) {
        return call;
      }
      Expr *key = sema_.cryptoArgs().cryptoData(args[0], "public key", true, false, true);
      Expr *encoding = sema_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = sema_.cryptoArgs().cryptoInt(loc, 0);
      }
      return bytesOrText(CryptoFn::EcdhComputeSecret, {self, key, encoding}, 2);
    }
  }
  if (kind == Crypto::X509) {
    if (name == "toString" || name == "toJSON") {
      if (!sema_.argumentChecker().checkArity(call, loc, 0, 0)) {
        return call;
      }
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::X509Pem, {self}, Type::string());
    }
    if (name == "checkHost" || name == "checkEmail" || name == "checkIP") {
      if (!sema_.argumentChecker().checkArity(call, loc, 1, name == "checkIP" ? 1 : 2)) {
        return call;
      }
      Expr *value = sema_.argumentChecker().checkArg(args[0], Type::string());
      llvm::StringMap<Expr *> options;
      std::vector<llvm::StringRef> allowed = {"subject"};
      if (name == "checkHost") {
        allowed.insert(allowed.end(), {"wildcards", "partialWildcards", "multiLabelWildcards",
                                       "singleLabelSubdomains"});
      }
      if (present(1) && !sema_.cryptoArgs().cryptoOptions(args[1], name, allowed, options)) {
        return call;
      }
      int subject = 0;
      if (Expr *s = options.lookup("subject")) {
        if ((subject = sema_.cryptoArgs().cryptoChoice(s, "subject option", {"default", "always", "never"})) < 0) {
          return call;
        }
      }
      const Type *optional = Type::optional(Type::string());
      if (name == "checkIP") {
        return sema_.cryptoArgs().cryptoCall(call, CryptoFn::X509CheckIp, {self, value}, optional);
      }
      if (name == "checkEmail") {
        return sema_.cryptoArgs().cryptoCall(call, CryptoFn::X509CheckEmail, {self, value, sema_.cryptoArgs().cryptoInt(loc, subject)},
                          optional);
      }
      std::vector<Expr *> hostArgs = {self, value, sema_.cryptoArgs().cryptoInt(loc, subject)};
      int has = 0;
      std::vector<Expr *> flags;
      llvm::StringRef names[] = {"wildcards", "partialWildcards", "multiLabelWildcards",
                                 "singleLabelSubdomains"};
      for (int i = 0; i < 4; ++i) {
        Expr *flag = options.lookup(names[i]);
        has |= flag ? 1 << i : 0;
        flags.push_back(flag ? sema_.argumentChecker().checkArg(flag, Type::boolTy()) : sema_.cryptoArgs().cryptoBool(loc, false));
      }
      hostArgs.push_back(sema_.cryptoArgs().cryptoInt(loc, has));
      hostArgs.insert(hostArgs.end(), flags.begin(), flags.end());
      return sema_.cryptoArgs().cryptoCall(call, CryptoFn::X509CheckHost, std::move(hostArgs), optional);
    }
    if (name == "checkIssued" || name == "checkPrivateKey" || name == "verify") {
      if (!sema_.argumentChecker().checkArity(call, loc, 1, 1)) {
        return call;
      }
      Expr *other = name == "checkIssued" ? sema_.exprChecker().checkExpr(args[0]) : sema_.cryptoArgs().cryptoKeyObject(args[0], "key");
      if (name == "checkIssued" && !other->type->isError() &&
          other->type->crypto() != Crypto::X509) {
        sema_.diags_.error(other->loc(),
                     "'checkIssued' takes an X509Certificate; found '" + other->type->name() + "'");
        return call;
      }
      if (other->type->isError()) {
        return call;
      }
      CryptoFn fn = name == "checkIssued"       ? CryptoFn::X509CheckIssued
                    : name == "checkPrivateKey" ? CryptoFn::X509CheckPrivateKey
                                                : CryptoFn::X509Verify;
      return sema_.cryptoArgs().cryptoCall(call, fn, {self, other}, Type::boolTy());
    }
  }
  // In node these are streams too; tz has no streams yet (docs/streams.md).
  static const llvm::StringLiteral streamMethods[] = {
      "write",   "end",         "pipe", "unpipe", "read",           "on",
      "once",    "off",         "emit", "pause",  "resume",         "setEncoding",
      "destroy", "addListener", "cork", "uncork", "removeListener", "setDefaultEncoding"};
  if ((hash || cipher) && llvm::is_contained(streamMethods, name)) {
    const char *instead = kind == Crypto::Sign     ? "'update(data)' and 'sign(key)'"
                          : kind == Crypto::Verify ? "'update(data)' and 'verify(key, signature)'"
                          : cipher                 ? "'update(data)' and 'final()'"
                                                   : "'update(data)' and 'digest()'";
    sema_.diags_.error(loc, "'" + name + "' of " + type->name() +
                          " is not supported yet: in node it is a stream too, and tz has no "
                          "streams yet (docs/streams.md); use " +
                          instead);
    return loosely();
  }
  sema_.diags_.error(loc, "property '" + name + "' does not exist on type '" + type->name() + "'; it " +
                        "has " + methodsOf(kind));

  return loosely();
}

} // namespace tz
