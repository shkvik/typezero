#include "compiler/pipeline/check_program/topics/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_args/crypto_args.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::isa;

namespace tz {

CryptoMethodChecker::CryptoMethodChecker(CheckState &owner)
    : state(owner) {}

Expr *CryptoMethodChecker::checkCryptoMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  Expr *self = member->object;
  auto bytesOrText = [&](CryptoFn fn, std::vector<Expr *> fnArgs, size_t index,
                         bool fallible) -> Expr * {
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, index, name, -1);
    if (!encoding) {
      return call;
    }
    auto *made =
        this->state.getAst().make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{});
    if (fallible) {
      CryptoArgs(this->state).cryptoResult(made, fn, std::move(fnArgs), buffer);
    } else {
      CryptoArgs(this->state).cryptoCall(made, fn, std::move(fnArgs), buffer);
    }

    return CryptoArgs(this->state).cryptoText(made, encoding);
  };
  bool hash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
              kind == Crypto::Verify;
  bool cipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;

  if (hash && name == "update") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *data = CryptoArgs(this->state).cryptoData(args[0], "data", true, false, false);
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::HashUpdate, {self, data, encoding}, type);
  }
  if ((kind == Crypto::Hash || kind == Crypto::Hmac) && name == "digest") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 0, name, -1);
    if (!encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return CryptoArgs(this->state).cryptoCall(call, CryptoFn::HashDigest, {self}, buffer);
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::HashDigestText, {self, encoding}, Type::string());
  }
  if (kind == Crypto::Hash && name == "copy") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    llvm::StringMap<Expr *> options;
    if (present(0) &&
        !CryptoArgs(this->state).cryptoOptions(args[0], name, {"outputLength"}, options)) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    if (length) {
      return CryptoArgs(this->state)
          .cryptoResult(call, CryptoFn::HashCopy,
                        {self, CryptoArgs(this->state).cryptoNumber(length)}, type);
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::HashCopy, {self, CryptoArgs(this->state).cryptoDouble(loc, -1)},
                    type);
  }
  if (kind == Crypto::Sign && name == "sign") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::vector<Expr *> signArgs = {self};
    if (!CryptoArgs(this->state).cryptoSignKey(args[0], name, true, signArgs)) {
      return call;
    }
    return bytesOrText(CryptoFn::SignFinal, std::move(signArgs), 1, true);
  }
  if (kind == Crypto::Verify && name == "verify") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::vector<Expr *> verifyArgs = {self};
    if (!CryptoArgs(this->state).cryptoSignKey(args[0], name, false, verifyArgs)) {
      return call;
    }
    Expr *signature = CryptoArgs(this->state).cryptoData(args[1], "signature", true, false, true);
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 2, name, 0);
    if (signature->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
    }
    verifyArgs.insert(verifyArgs.begin() + 2, {signature, encoding});
    return CryptoArgs(this->state)
        .cryptoResult(call, CryptoFn::VerifyFinal, std::move(verifyArgs), Type::boolTy());
  }
  if (cipher && name == "update") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *data = CryptoArgs(this->state).cryptoData(args[0], "data", true, false, true);
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
    }
    return bytesOrText(CryptoFn::CipherUpdate, {self, data, encoding}, 2, false);
  }
  if (cipher && name == "final") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return bytesOrText(CryptoFn::CipherFinal, {self}, 0, true);
  }
  if (cipher && name == "setAAD") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *data = CryptoArgs(this->state).cryptoData(args[0], "AAD", false, false, true);
    llvm::StringMap<Expr *> options;
    if (present(1) &&
        !CryptoArgs(this->state).cryptoOptions(args[1], name, {"plaintextLength"}, options)) {
      return call;
    }
    if (data->type->isError()) {
      return call;
    }
    Expr *length = options.lookup("plaintextLength");
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::CipherSetAad,
                    {self, data,
                     length ? CryptoArgs(this->state).cryptoNumber(length)
                            : CryptoArgs(this->state).cryptoDouble(loc, -1)},
                    type);
  }
  if (kind == Crypto::Cipheriv && name == "getAuthTag") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return CryptoArgs(this->state).cryptoResult(call, CryptoFn::CipherGetAuthTag, {self}, buffer);
  }
  if (kind == Crypto::Decipheriv && name == "setAuthTag") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *tag = CryptoArgs(this->state).cryptoData(args[0], "tag", true, false, true);
    Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 1, name, 0);
    if (tag->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::CipherSetAuthTag, {self, tag, encoding}, type);
  }
  if (cipher && name == "setAutoPadding") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *padding = present(0) ? ArgumentChecker(this->state).checkArg(args[0], Type::boolTy())
                               : CryptoArgs(this->state).cryptoBool(loc, true);
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::CipherSetAutoPadding, {self, padding}, type);
  }
  if (kind == Crypto::KeyObject && name == "export") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    if (!present(0)) {
      return CryptoArgs(this->state).cryptoResult(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    llvm::StringMap<Expr *> options;
    if (!CryptoArgs(this->state)
             .cryptoOptions(args[0], name, {"format", "type", "cipher", "passphrase"}, options)) {
      return call;
    }
    int format = 3;
    if (Expr *f = options.lookup("format")) {
      if ((format = CryptoArgs(this->state)
                        .cryptoChoice(f, "format of the key", {"pem", "der", "jwk", "buffer"})) <
          0) {
        return call;
      }
    }
    if (format == 2) {
      return CryptoArgs(this->state)
          .cryptoResult(call, CryptoFn::KeyExportJwk, {self}, Type::json());
    }
    if (format == 3) {
      return CryptoArgs(this->state).cryptoResult(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    int has = 0;
    Expr *keyType = CryptoArgs(this->state).cryptoString(loc, ""),
         *cipherName = CryptoArgs(this->state).cryptoString(loc, "");
    Expr *passphrase = CryptoArgs(this->state).cryptoUndefined(loc);
    if (Expr *t = options.lookup("type")) {
      int which = CryptoArgs(this->state)
                      .cryptoChoice(t, "type of the key", {"pkcs1", "spki", "pkcs8", "sec1"});
      if (which < 0) {
        return call;
      }
      static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
      keyType = CryptoArgs(this->state).cryptoString(t->loc(), types[which]);
      has |= 1;
    }
    if (Expr *c = options.lookup("cipher")) {
      cipherName = ArgumentChecker(this->state).checkArg(c, Type::string());
      has |= 2;
    }
    if (Expr *p = options.lookup("passphrase")) {
      passphrase = CryptoArgs(this->state).cryptoData(p, "passphrase", true, false, true);
      has |= 4;
      if (passphrase->type->isError()) {
        return call;
      }
    }
    return CryptoArgs(this->state)
        .cryptoResult(
            call, format == 0 ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer,
            {self, CryptoArgs(this->state).cryptoInt(loc, has), keyType, cipherName, passphrase},
            format == 0 ? Type::string() : buffer);
  }
  if (kind == Crypto::KeyObject && name == "equals") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *other = CryptoArgs(this->state).cryptoKeyObject(args[0], "other key");
    if (other->type->isError()) {
      return call;
    }
    return CryptoArgs(this->state)
        .cryptoCall(call, CryptoFn::KeyEquals, {self, other}, Type::boolTy());
  }
  if (kind == Crypto::Ecdh) {
    auto format = [&](size_t index) -> Expr * {
      if (!present(index)) {
        return CryptoArgs(this->state).cryptoInt(loc, 0);
      }
      int which = CryptoArgs(this->state)
                      .cryptoChoice(args[index], "format of the point",
                                    {"uncompressed", "compressed", "hybrid"});

      return which < 0 ? nullptr : CryptoArgs(this->state).cryptoInt(loc, which);
    };
    if (name == "generateKeys" || name == "getPublicKey") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 0, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *f = format(1);
      if (!f) {
        return call;
      }
      return bytesOrText(name == "generateKeys" ? CryptoFn::EcdhGenerateKeys
                                                : CryptoFn::EcdhGetPublicKey,
                         {self, f}, 0, name == "getPublicKey");
    }
    if (name == "getPrivateKey") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 0, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      return bytesOrText(CryptoFn::EcdhGetPrivateKey, {self}, 0, true);
    }
    if (name == "setPrivateKey") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *key = CryptoArgs(this->state).cryptoData(args[0], "private key", true, false, true);
      Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
      }
      return CryptoArgs(this->state)
          .cryptoResult(call, CryptoFn::EcdhSetPrivateKey, {self, key, encoding}, Type::voidTy());
    }
    if (name == "computeSecret") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 3);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *key = CryptoArgs(this->state).cryptoData(args[0], "public key", true, false, true);
      Expr *encoding = CryptoArgs(this->state).cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = CryptoArgs(this->state).cryptoInt(loc, 0);
      }
      return bytesOrText(CryptoFn::EcdhComputeSecret, {self, key, encoding}, 2, true);
    }
  }
  if (kind == Crypto::X509) {
    if (name == "toString" || name == "toJSON") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 0, 0);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      return CryptoArgs(this->state).cryptoCall(call, CryptoFn::X509Pem, {self}, Type::string());
    }
    if (name == "checkHost" || name == "checkEmail" || name == "checkIP") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, name == "checkIP" ? 1 : 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *value = ArgumentChecker(this->state).checkArg(args[0], Type::string());
      llvm::StringMap<Expr *> options;
      std::vector<llvm::StringRef> allowed = {"subject"};
      if (name == "checkHost") {
        allowed.insert(allowed.end(), {"wildcards", "partialWildcards", "multiLabelWildcards",
                                       "singleLabelSubdomains"});
      }
      if (present(1) && !CryptoArgs(this->state).cryptoOptions(args[1], name, allowed, options)) {
        return call;
      }
      int subject = 0;
      if (Expr *s = options.lookup("subject")) {
        if ((subject = CryptoArgs(this->state)
                           .cryptoChoice(s, "subject option", {"default", "always", "never"})) <
            0) {
          return call;
        }
      }
      const Type *optional = Type::optional(Type::string());
      if (name == "checkIP") {
        return CryptoArgs(this->state)
            .cryptoCall(call, CryptoFn::X509CheckIp, {self, value}, optional);
      }
      if (name == "checkEmail") {
        return CryptoArgs(this->state)
            .cryptoCall(call, CryptoFn::X509CheckEmail,
                        {self, value, CryptoArgs(this->state).cryptoInt(loc, subject)}, optional);
      }
      std::vector<Expr *> hostArgs = {self, value, CryptoArgs(this->state).cryptoInt(loc, subject)};
      int has = 0;
      std::vector<Expr *> flags;
      llvm::StringRef names[] = {"wildcards", "partialWildcards", "multiLabelWildcards",
                                 "singleLabelSubdomains"};
      for (int i = 0; i < 4; ++i) {
        Expr *flag = options.lookup(names[i]);
        has |= flag ? 1 << i : 0;
        flags.push_back(flag ? ArgumentChecker(this->state).checkArg(flag, Type::boolTy())
                             : CryptoArgs(this->state).cryptoBool(loc, false));
      }
      hostArgs.push_back(CryptoArgs(this->state).cryptoInt(loc, has));
      hostArgs.insert(hostArgs.end(), flags.begin(), flags.end());
      return CryptoArgs(this->state)
          .cryptoCall(call, CryptoFn::X509CheckHost, std::move(hostArgs), optional);
    }
    if (name == "checkIssued" || name == "checkPrivateKey" || name == "verify") {
      ArgumentChecker::ResultArityMatches arity =
          ArgumentChecker(this->state).checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *other = name == "checkIssued" ? ExprChecker(this->state).checkExpr(args[0])
                                          : CryptoArgs(this->state).cryptoKeyObject(args[0], "key");
      if (name == "checkIssued" && !other->type->isError() &&
          other->type->crypto() != Crypto::X509) {
        this->state.getDiagnostics().error(other->loc(),
                                           "'checkIssued' takes an X509Certificate; found '" +
                                               other->type->name() + "'");
        return call;
      }
      if (other->type->isError()) {
        return call;
      }
      if (name == "checkIssued") {
        return CryptoArgs(this->state)
            .cryptoCall(call, CryptoFn::X509CheckIssued, {self, other}, Type::boolTy());
      }
      return CryptoArgs(this->state)
          .cryptoResult(call,
                        name == "checkPrivateKey" ? CryptoFn::X509CheckPrivateKey
                                                  : CryptoFn::X509Verify,
                        {self, other}, Type::boolTy());
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
    this->state.getDiagnostics().error(
        loc, "'" + name + "' of " + type->name() +
                 " is not supported yet: in node it is a stream too, and tz has no "
                 "streams yet (docs/streams.md); use " +
                 instead);
    return loosely();
  }
  this->state.getDiagnostics().error(loc, "property '" + name + "' does not exist on type '" +
                                              type->name() + "'; it " + "has " + methodsOf(kind));

  return loosely();
}

}
