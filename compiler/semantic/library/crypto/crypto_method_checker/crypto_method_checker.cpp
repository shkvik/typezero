#include "compiler/semantic/library/crypto/crypto_method_checker/crypto_method_checker.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/semantic/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker_errors.h"
#include "compiler/semantic/library/crypto/crypto_names/crypto_names.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::isa;

namespace tz {

CryptoMethodChecker::CryptoMethodChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->cryptoArgs = &all.getCryptoArgs();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->argumentChecker = &all.getArgumentChecker();
}

CryptoMethodChecker::ResultIssuerChecked CryptoMethodChecker::issuer(const Expr *other) {
  if (!other->type->isError() && other->type->crypto() != Crypto::X509) {
    std::string message = ErrorTextCryptoMethod::issuer(other->type->name());

    return err(CryptoMethodCheckerIssuerError{
        .code = CryptoMethodCheckerIssuerErrorCode::NOT_A_CERTIFICATE,
        .message = message,
    });
  }

  return ok(other);
}

Expr *CryptoMethodChecker::checkCryptoMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }

    return call;
  };
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  std::vector<Expr *> &args = call->args;
  auto present = [&](size_t i) { return i < args.size() && !isa<UndefinedExpr>(args[i]); };
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  Expr *self = member->object;
  auto bytesOrText = [&](CryptoFn fn, std::vector<Expr *> fnArgs, size_t index,
                         bool fallible) -> Expr * {
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, index, name, -1);
    if (!encoding) {
      return call;
    }
    auto *made =
        this->state.getAst().make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{});
    if (fallible) {
      this->cryptoArgs->cryptoResult(made, fn, std::move(fnArgs), buffer);
    } else {
      this->cryptoArgs->cryptoCall(made, fn, std::move(fnArgs), buffer);
    }

    return this->cryptoArgs->cryptoText(made, encoding);
  };
  bool hash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
              kind == Crypto::Verify;
  bool cipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;

  if (hash && name == "update") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *data = this->cryptoArgs->cryptoData(args[0], "data", true, false, false);
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = this->cryptoArgs->cryptoInt(loc, 0);
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::HashUpdate, {self, data, encoding}, type);
  }
  if ((kind == Crypto::Hash || kind == Crypto::Hmac) && name == "digest") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 0, name, -1);
    if (!encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      return this->cryptoArgs->cryptoCall(call, CryptoFn::HashDigest, {self}, buffer);
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::HashDigestText, {self, encoding},
                                        Type::string());
  }
  if (kind == Crypto::Hash && name == "copy") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    llvm::StringMap<Expr *> options;
    if (present(0) && !this->cryptoArgs->cryptoOptions(args[0], name, {"outputLength"}, options)) {
      return call;
    }
    Expr *length = options.lookup("outputLength");
    if (length) {
      return this->cryptoArgs->cryptoResult(call, CryptoFn::HashCopy,
                                            {self, this->cryptoArgs->cryptoNumber(length)}, type);
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::HashCopy,
                                        {self, this->cryptoArgs->cryptoDouble(loc, -1)}, type);
  }
  if (kind == Crypto::Sign && name == "sign") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::vector<Expr *> signArgs = {self};
    if (!this->cryptoArgs->cryptoSignKey(args[0], name, true, signArgs)) {
      return call;
    }
    return bytesOrText(CryptoFn::SignFinal, std::move(signArgs), 1, true);
  }
  if (kind == Crypto::Verify && name == "verify") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 2, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    std::vector<Expr *> verifyArgs = {self};
    if (!this->cryptoArgs->cryptoSignKey(args[0], name, false, verifyArgs)) {
      return call;
    }
    Expr *signature = this->cryptoArgs->cryptoData(args[1], "signature", true, false, true);
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 2, name, 0);
    if (signature->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = this->cryptoArgs->cryptoInt(loc, 0);
    }
    verifyArgs.insert(verifyArgs.begin() + 2, {signature, encoding});
    return this->cryptoArgs->cryptoResult(call, CryptoFn::VerifyFinal, std::move(verifyArgs),
                                          Type::boolTy());
  }
  if (cipher && name == "update") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *data = this->cryptoArgs->cryptoData(args[0], "data", true, false, true);
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 1, name, 0);
    if (data->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = this->cryptoArgs->cryptoInt(loc, 0);
    }
    return bytesOrText(CryptoFn::CipherUpdate, {self, data, encoding}, 2, false);
  }
  if (cipher && name == "final") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return bytesOrText(CryptoFn::CipherFinal, {self}, 0, true);
  }
  if (cipher && name == "setAAD") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *data = this->cryptoArgs->cryptoData(args[0], "AAD", false, false, true);
    llvm::StringMap<Expr *> options;
    if (present(1) &&
        !this->cryptoArgs->cryptoOptions(args[1], name, {"plaintextLength"}, options)) {
      return call;
    }
    if (data->type->isError()) {
      return call;
    }
    Expr *length = options.lookup("plaintextLength");
    return this->cryptoArgs->cryptoCall(
        call, CryptoFn::CipherSetAad,
        {self, data,
         length ? this->cryptoArgs->cryptoNumber(length) : this->cryptoArgs->cryptoDouble(loc, -1)},
        type);
  }
  if (kind == Crypto::Cipheriv && name == "getAuthTag") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    return this->cryptoArgs->cryptoResult(call, CryptoFn::CipherGetAuthTag, {self}, buffer);
  }
  if (kind == Crypto::Decipheriv && name == "setAuthTag") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *tag = this->cryptoArgs->cryptoData(args[0], "tag", true, false, true);
    Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 1, name, 0);
    if (tag->type->isError() || !encoding) {
      return call;
    }
    if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
      encoding = this->cryptoArgs->cryptoInt(loc, 0);
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::CipherSetAuthTag, {self, tag, encoding},
                                        type);
  }
  if (cipher && name == "setAutoPadding") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *padding = present(0) ? this->argumentChecker->checkArg(args[0], Type::boolTy())
                               : this->cryptoArgs->cryptoBool(loc, true);
    return this->cryptoArgs->cryptoCall(call, CryptoFn::CipherSetAutoPadding, {self, padding},
                                        type);
  }
  if (kind == Crypto::KeyObject && name == "export") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    if (!present(0)) {
      return this->cryptoArgs->cryptoResult(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    llvm::StringMap<Expr *> options;
    if (!this->cryptoArgs->cryptoOptions(args[0], name, {"format", "type", "cipher", "passphrase"},
                                         options)) {
      return call;
    }
    int format = 3;
    if (Expr *f = options.lookup("format")) {
      std::string keyFormat = ErrorTextCryptoFnLabel::keyFormat();
      format = this->cryptoArgs->cryptoChoice(f, keyFormat, {"pem", "der", "jwk", "buffer"});
      if (format < 0) {
        return call;
      }
    }
    if (format == 2) {
      return this->cryptoArgs->cryptoResult(call, CryptoFn::KeyExportJwk, {self}, Type::json());
    }
    if (format == 3) {
      return this->cryptoArgs->cryptoResult(call, CryptoFn::KeyExportSecret, {self}, buffer);
    }
    int has = 0;
    Expr *keyType = this->cryptoArgs->cryptoString(loc, ""),
         *cipherName = this->cryptoArgs->cryptoString(loc, "");
    Expr *passphrase = this->cryptoArgs->cryptoUndefined(loc);
    if (Expr *t = options.lookup("type")) {
      std::string typeWhat = ErrorTextCryptoFnLabel::keyType();
      int which = this->cryptoArgs->cryptoChoice(t, typeWhat, {"pkcs1", "spki", "pkcs8", "sec1"});
      if (which < 0) {
        return call;
      }
      static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
      keyType = this->cryptoArgs->cryptoString(t->loc(), types[which]);
      has |= 1;
    }
    if (Expr *c = options.lookup("cipher")) {
      cipherName = this->argumentChecker->checkArg(c, Type::string());
      has |= 2;
    }
    if (Expr *p = options.lookup("passphrase")) {
      passphrase = this->cryptoArgs->cryptoData(p, "passphrase", true, false, true);
      has |= 4;
      if (passphrase->type->isError()) {
        return call;
      }
    }
    return this->cryptoArgs->cryptoResult(
        call, format == 0 ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer,
        {self, this->cryptoArgs->cryptoInt(loc, has), keyType, cipherName, passphrase},
        format == 0 ? Type::string() : buffer);
  }
  if (kind == Crypto::KeyObject && name == "equals") {
    ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    Expr *other = this->cryptoArgs->cryptoKeyObject(args[0], "other key");
    if (other->type->isError()) {
      return call;
    }
    return this->cryptoArgs->cryptoCall(call, CryptoFn::KeyEquals, {self, other}, Type::boolTy());
  }
  if (kind == Crypto::Ecdh) {
    auto format = [&](size_t index) -> Expr * {
      if (!present(index)) {
        return this->cryptoArgs->cryptoInt(loc, 0);
      }
      std::string point = ErrorTextCryptoPoint::format();
      int which = this->cryptoArgs->cryptoChoice(args[index], point,
                                                 {"uncompressed", "compressed", "hybrid"});

      return which < 0 ? nullptr : this->cryptoArgs->cryptoInt(loc, which);
    };
    if (name == "generateKeys" || name == "getPublicKey") {
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 2);
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
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      return bytesOrText(CryptoFn::EcdhGetPrivateKey, {self}, 0, true);
    }
    if (name == "setPrivateKey") {
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *key = this->cryptoArgs->cryptoData(args[0], "private key", true, false, true);
      Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = this->cryptoArgs->cryptoInt(loc, 0);
      }
      return this->cryptoArgs->cryptoResult(call, CryptoFn::EcdhSetPrivateKey,
                                            {self, key, encoding}, Type::voidTy());
    }
    if (name == "computeSecret") {
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 3);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *key = this->cryptoArgs->cryptoData(args[0], "public key", true, false, true);
      Expr *encoding = this->cryptoArgs->cryptoEncoding(call, 1, name, 0);
      if (key->type->isError() || !encoding) {
        return call;
      }
      if (int64_t(cast<IntLitExpr>(encoding)->value) < 0) {
        encoding = this->cryptoArgs->cryptoInt(loc, 0);
      }
      return bytesOrText(CryptoFn::EcdhComputeSecret, {self, key, encoding}, 2, true);
    }
  }
  if (kind == Crypto::X509) {
    if (name == "toString" || name == "toJSON") {
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      return this->cryptoArgs->cryptoCall(call, CryptoFn::X509Pem, {self}, Type::string());
    }
    if (name == "checkHost" || name == "checkEmail" || name == "checkIP") {
      ArgumentChecker::ResultArityMatches arity =
          this->argumentChecker->checkArity(call, 1, name == "checkIP" ? 1 : 2);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *value = this->argumentChecker->checkArg(args[0], Type::string());
      llvm::StringMap<Expr *> options;
      std::vector<llvm::StringRef> allowed = {"subject"};
      if (name == "checkHost") {
        allowed.insert(allowed.end(), {"wildcards", "partialWildcards", "multiLabelWildcards",
                                       "singleLabelSubdomains"});
      }
      if (present(1) && !this->cryptoArgs->cryptoOptions(args[1], name, allowed, options)) {
        return call;
      }
      int subject = 0;
      if (Expr *s = options.lookup("subject")) {
        if ((subject = this->cryptoArgs->cryptoChoice(s, "subject option",
                                                      {"default", "always", "never"})) < 0) {
          return call;
        }
      }
      const Type *optional = Type::optional(Type::string());
      if (name == "checkIP") {
        return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckIp, {self, value}, optional);
      }
      if (name == "checkEmail") {
        return this->cryptoArgs->cryptoCall(
            call, CryptoFn::X509CheckEmail,
            {self, value, this->cryptoArgs->cryptoInt(loc, subject)}, optional);
      }
      std::vector<Expr *> hostArgs = {self, value, this->cryptoArgs->cryptoInt(loc, subject)};
      int has = 0;
      std::vector<Expr *> flags;
      llvm::StringRef names[] = {"wildcards", "partialWildcards", "multiLabelWildcards",
                                 "singleLabelSubdomains"};
      for (int i = 0; i < 4; ++i) {
        Expr *flag = options.lookup(names[i]);
        has |= flag ? 1 << i : 0;
        flags.push_back(flag ? this->argumentChecker->checkArg(flag, Type::boolTy())
                             : this->cryptoArgs->cryptoBool(loc, false));
      }
      hostArgs.push_back(this->cryptoArgs->cryptoInt(loc, has));
      hostArgs.insert(hostArgs.end(), flags.begin(), flags.end());
      return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckHost, std::move(hostArgs),
                                          optional);
    }
    if (name == "checkIssued" || name == "checkPrivateKey" || name == "verify") {
      ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
      if (arity.isErr()) {
        this->state.getDiagnostics().error(loc, arity.error().message);
        return call;
      }
      Expr *other = name == "checkIssued" ? this->exprChecker->checkExpr(args[0])
                                          : this->cryptoArgs->cryptoKeyObject(args[0], "key");
      ResultIssuerChecked issued =
          name == "checkIssued" ? CryptoMethodChecker::issuer(other) : ok(other);
      if (issued.isErr()) {
        this->state.getDiagnostics().error(other->loc(), issued.error().message);
        return call;
      }
      if (other->type->isError()) {
        return call;
      }
      if (name == "checkIssued") {
        return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckIssued, {self, other},
                                            Type::boolTy());
      }
      return this->cryptoArgs->cryptoResult(
          call, name == "checkPrivateKey" ? CryptoFn::X509CheckPrivateKey : CryptoFn::X509Verify,
          {self, other}, Type::boolTy());
    }
  }
  static const llvm::StringLiteral streamMethods[] = {
      "write",   "end",         "pipe", "unpipe", "read",           "on",
      "once",    "off",         "emit", "pause",  "resume",         "setEncoding",
      "destroy", "addListener", "cork", "uncork", "removeListener", "setDefaultEncoding"};
  if ((hash || cipher) && llvm::is_contained(streamMethods, name)) {
    std::string message = ErrorTextCryptoMethod::stream(name, type->name(), kind, cipher);
    this->state.getDiagnostics().error(loc, message);
    return loosely();
  }
  std::string message =
      ErrorTextCryptoMethod::noMethod(name, type->name(), CryptoNames::methodsOf(kind));
  this->state.getDiagnostics().error(loc, message);

  return loosely();
}

}
