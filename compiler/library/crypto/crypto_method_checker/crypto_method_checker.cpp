#include "compiler/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

CryptoMethodChecker::CryptoMethodChecker(Checker &checker) : checker_(checker) {}

Expr *CryptoMethodChecker::checkCryptoMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  Expr *self = member->object;
  auto bytesOrText = [&](CryptoFn fn, std::vector<Expr *> fnArgs, size_t index) -> Expr * {
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, index, name, -1);
    if (!encoding) {
      return call;
    }
    auto *made = checker_.ctx_.make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{});
    checker_.cryptoArgs().cryptoCall(made, fn, std::move(fnArgs), buffer);

    return checker_.cryptoArgs().cryptoText(made, encoding);
  };
  bool hash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
              kind == Crypto::Verify;
  bool cipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;

  if (hash && name == "update") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *data = checker_.cryptoArgs().cryptoData(args[0], "data", true, false, false);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HashUpdate, {self, data, encoding},
                                            type);
  }
  if ((kind == Crypto::Hash || kind == Crypto::Hmac) && name == "digest") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 0, name, -1);
    if (!encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HashDigest, {self}, buffer);
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::HashDigestText, {self, encoding},
                                            Type::string());
  }
  if (kind == Crypto::Hash && name == "copy") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    llvm::StringMap<Expr *> options;
    if (present(0) &&
        !checker_.cryptoArgs().cryptoOptions(args[0], name, {"outputLength"}, options)) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    return checker_.cryptoArgs().cryptoCall(
        call, CryptoFn::HashCopy,
        {self, length ? checker_.cryptoArgs().cryptoNumber(length)
                      : checker_.cryptoArgs().cryptoDouble(loc, -1)},
        type);
  }
  if (kind == Crypto::Sign && name == "sign") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    std::vector<Expr *> signArgs = {self};
    if (!checker_.cryptoArgs().cryptoSignKey(args[0], name, true, signArgs)) {
      return call;
    }
    return bytesOrText(CryptoFn::SignFinal, std::move(signArgs), 1);
  }
  if (kind == Crypto::Verify && name == "verify") {
    if (!checker_.argumentChecker().checkArity(call, loc, 2, 3)) {
      return call;
    }
    std::vector<Expr *> verifyArgs = {self};
    if (!checker_.cryptoArgs().cryptoSignKey(args[0], name, false, verifyArgs)) {
      return call;
    }
    Expr *signature = checker_.cryptoArgs().cryptoData(args[1], "signature", true, false, true);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 2, name, 0);
    if (signature->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
    }
    verifyArgs.insert(verifyArgs.begin() + 2, {signature, encoding});
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::VerifyFinal, std::move(verifyArgs),
                                            Type::boolTy());
  }
  if (cipher && name == "update") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 3)) {
      return call;
    }
    Expr *data = checker_.cryptoArgs().cryptoData(args[0], "data", true, false, true);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
    }
    return bytesOrText(CryptoFn::CipherUpdate, {self, data, encoding}, 2);
  }
  if (cipher && name == "final") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    return bytesOrText(CryptoFn::CipherFinal, {self}, 0);
  }
  if (cipher && name == "setAAD") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *data = checker_.cryptoArgs().cryptoData(args[0], "AAD", false, false, true);
    llvm::StringMap<Expr *> options;
    if (present(1) &&
        !checker_.cryptoArgs().cryptoOptions(args[1], name, {"plaintextLength"}, options)) {
      return call;
    }
    if (data->type->isError()) {
      return call;
    }
    Expr *length = options.lookup("plaintextLength");
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::CipherSetAad,
                                            {self, data,
                                             length ? checker_.cryptoArgs().cryptoNumber(length)
                                                    : checker_.cryptoArgs().cryptoDouble(loc, -1)},
                                            type);
  }
  if (kind == Crypto::Cipheriv && name == "getAuthTag") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 0)) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::CipherGetAuthTag, {self}, buffer);
  }
  if (kind == Crypto::Decipheriv && name == "setAuthTag") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
      return call;
    }
    Expr *tag = checker_.cryptoArgs().cryptoData(args[0], "tag", true, false, true);
    Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
    if (tag->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::CipherSetAuthTag, {self, tag, encoding},
                                            type);
  }
  if (cipher && name == "setAutoPadding") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    Expr *padding = present(0) ? checker_.argumentChecker().checkArg(args[0], Type::boolTy())
                               : checker_.cryptoArgs().cryptoBool(loc, true);
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::CipherSetAutoPadding, {self, padding},
                                            type);
  }
  if (kind == Crypto::KeyObject && name == "export") {
    if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    if (!present(0)) {
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    llvm::StringMap<Expr *> options;
    if (!checker_.cryptoArgs().cryptoOptions(args[0], name,
                                             {"format", "type", "cipher", "passphrase"}, options)) {
      return call;
    }
    int format = 3;
    if (Expr *f = options.lookup("format")) {
      if ((format = checker_.cryptoArgs().cryptoChoice(f, "format of the key",
                                                       {"pem", "der", "jwk", "buffer"})) < 0) {
        return call;
      }
    }
    if (format == 2) {
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::KeyExportJwk, {self}, Type::json());
    }
    if (format == 3) {
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    int has = 0;
    Expr *keyType = checker_.cryptoArgs().cryptoString(loc, ""),
         *cipherName = checker_.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = checker_.cryptoArgs().cryptoUndefined(loc);
    if (Expr *t = options.lookup("type")) {
      int which = checker_.cryptoArgs().cryptoChoice(t, "type of the key",
                                                     {"pkcs1", "spki", "pkcs8", "sec1"});
      if (which < 0) {
        return call;
      }
      static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
      keyType = checker_.cryptoArgs().cryptoString(t->loc(), types[which]);
      has |= 1;
    }
    if (Expr *c = options.lookup("cipher")) {
      cipherName = checker_.argumentChecker().checkArg(c, Type::string());
      has |= 2;
    }
    if (Expr *p = options.lookup("passphrase")) {
      passphrase = checker_.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
      has |= 4;
      if (passphrase->type->isError()) {
        return call;
      }
    }
    return checker_.cryptoArgs().cryptoCall(
        call, format == 0 ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer,
        {self, checker_.cryptoArgs().cryptoInt(loc, has), keyType, cipherName, passphrase},
        format == 0 ? Type::string() : buffer);
  }
  if (kind == Crypto::KeyObject && name == "equals") {
    if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
      return call;
    }
    Expr *other = checker_.cryptoArgs().cryptoKeyObject(args[0], "other key");
    if (other->type->isError()) {
      return call;
    }
    return checker_.cryptoArgs().cryptoCall(call, CryptoFn::KeyEquals, {self, other},
                                            Type::boolTy());
  }
  if (kind == Crypto::Ecdh) {
    auto format = [&](size_t index) -> Expr * {
      if (!present(index)) {
        return checker_.cryptoArgs().cryptoInt(loc, 0);
      }
      int which = checker_.cryptoArgs().cryptoChoice(args[index], "format of the point",
                                                     {"uncompressed", "compressed", "hybrid"});

      return which < 0 ? nullptr : checker_.cryptoArgs().cryptoInt(loc, which);
    };
    if (name == "generateKeys" || name == "getPublicKey") {
      if (!checker_.argumentChecker().checkArity(call, loc, 0, 2)) {
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
      if (!checker_.argumentChecker().checkArity(call, loc, 0, 1)) {
        return call;
      }
      return bytesOrText(CryptoFn::EcdhGetPrivateKey, {self}, 0);
    }
    if (name == "setPrivateKey") {
      if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
        return call;
      }
      Expr *key = checker_.cryptoArgs().cryptoData(args[0], "private key", true, false, true);
      Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
      }
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::EcdhSetPrivateKey,
                                              {self, key, encoding}, type);
    }
    if (name == "computeSecret") {
      if (!checker_.argumentChecker().checkArity(call, loc, 1, 3)) {
        return call;
      }
      Expr *key = checker_.cryptoArgs().cryptoData(args[0], "public key", true, false, true);
      Expr *encoding = checker_.cryptoArgs().cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = checker_.cryptoArgs().cryptoInt(loc, 0);
      }
      return bytesOrText(CryptoFn::EcdhComputeSecret, {self, key, encoding}, 2);
    }
  }
  if (kind == Crypto::X509) {
    if (name == "toString" || name == "toJSON") {
      if (!checker_.argumentChecker().checkArity(call, loc, 0, 0)) {
        return call;
      }
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::X509Pem, {self}, Type::string());
    }
    if (name == "checkHost" || name == "checkEmail" || name == "checkIP") {
      if (!checker_.argumentChecker().checkArity(call, loc, 1, name == "checkIP" ? 1 : 2)) {
        return call;
      }
      Expr *value = checker_.argumentChecker().checkArg(args[0], Type::string());
      llvm::StringMap<Expr *> options;
      std::vector<llvm::StringRef> allowed = {"subject"};
      if (name == "checkHost") {
        allowed.insert(allowed.end(), {"wildcards", "partialWildcards", "multiLabelWildcards",
                                       "singleLabelSubdomains"});
      }
      if (present(1) && !checker_.cryptoArgs().cryptoOptions(args[1], name, allowed, options)) {
        return call;
      }
      int subject = 0;
      if (Expr *s = options.lookup("subject")) {
        if ((subject = checker_.cryptoArgs().cryptoChoice(s, "subject option",
                                                          {"default", "always", "never"})) < 0) {
          return call;
        }
      }
      const Type *optional = Type::optional(Type::string());
      if (name == "checkIP") {
        return checker_.cryptoArgs().cryptoCall(call, CryptoFn::X509CheckIp, {self, value},
                                                optional);
      }
      if (name == "checkEmail") {
        return checker_.cryptoArgs().cryptoCall(
            call, CryptoFn::X509CheckEmail,
            {self, value, checker_.cryptoArgs().cryptoInt(loc, subject)}, optional);
      }
      std::vector<Expr *> hostArgs = {self, value, checker_.cryptoArgs().cryptoInt(loc, subject)};
      int has = 0;
      std::vector<Expr *> flags;
      llvm::StringRef names[] = {"wildcards", "partialWildcards", "multiLabelWildcards",
                                 "singleLabelSubdomains"};
      for (int i = 0; i < 4; ++i) {
        Expr *flag = options.lookup(names[i]);
        has |= flag ? 1 << i : 0;
        flags.push_back(flag ? checker_.argumentChecker().checkArg(flag, Type::boolTy())
                             : checker_.cryptoArgs().cryptoBool(loc, false));
      }
      hostArgs.push_back(checker_.cryptoArgs().cryptoInt(loc, has));
      hostArgs.insert(hostArgs.end(), flags.begin(), flags.end());
      return checker_.cryptoArgs().cryptoCall(call, CryptoFn::X509CheckHost, std::move(hostArgs),
                                              optional);
    }
    if (name == "checkIssued" || name == "checkPrivateKey" || name == "verify") {
      if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
        return call;
      }
      Expr *other = name == "checkIssued" ? checker_.exprChecker().checkExpr(args[0])
                                          : checker_.cryptoArgs().cryptoKeyObject(args[0], "key");
      if (name == "checkIssued" && !other->type->isError() &&
          other->type->crypto() != Crypto::X509) {
        checker_.diags_.error(other->loc(), "'checkIssued' takes an X509Certificate; found '" +
                                                other->type->name() + "'");
        return call;
      }
      if (other->type->isError()) {
        return call;
      }
      CryptoFn fn = name == "checkIssued"       ? CryptoFn::X509CheckIssued
                    : name == "checkPrivateKey" ? CryptoFn::X509CheckPrivateKey
                                                : CryptoFn::X509Verify;
      return checker_.cryptoArgs().cryptoCall(call, fn, {self, other}, Type::boolTy());
    }
  }
  static const llvm::StringLiteral streamMethods[] = {
      "write",   "end",         "pipe", "unpipe", "read",           "on",
      "once",    "off",         "emit", "pause",  "resume",         "setEncoding",
      "destroy", "addListener", "cork", "uncork", "removeListener", "setDefaultEncoding"};
  if ((hash || cipher) && llvm::is_contained(streamMethods, name)) {
    const char *instead = kind == Crypto::Sign     ? "'update(data)' and 'sign(key)'"
                          : kind == Crypto::Verify ? "'update(data)' and 'verify(key, signature)'"
                          : cipher                 ? "'update(data)' and 'final()'"
                                                   : "'update(data)' and 'digest()'";
    checker_.diags_.error(loc,
                          "'" + name + "' of " + type->name() +
                              " is not supported yet: in node it is a stream too, and tz has no "
                              "streams yet (docs/streams.md); use " +
                              instead);
    return loosely();
  }
  checker_.diags_.error(loc, "property '" + name + "' does not exist on type '" + type->name() +
                                 "'; it " + "has " + methodsOf(kind));

  return loosely();
}

}
