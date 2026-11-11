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
  this->cryptoArgs = &all.cryptoArgs;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->argumentChecker = &all.argumentChecker;
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
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  Crypto kind = member->object->type->crypto();
  const CryptoMethodInfo *found = CryptoMethodTable::find(kind, member->name);
  if (found == nullptr) {
    return this->reportNoMethod(call, member);
  }

  CryptoMethodCall crypto{
      .call = call,
      .member = member,
      .method = found->method,
  };
  switch (found->method) {
    case CryptoMethod::HASH_UPDATE:
      return this->checkHashUpdate(crypto);
    case CryptoMethod::DIGEST:
      return this->checkDigest(crypto);
    case CryptoMethod::HASH_COPY:
      return this->checkHashCopy(crypto);
    case CryptoMethod::SIGN:
      return this->checkSign(crypto);
    case CryptoMethod::VERIFY:
      return this->checkVerify(crypto);
    case CryptoMethod::CIPHER_UPDATE:
      return this->checkCipherUpdate(crypto);
    case CryptoMethod::CIPHER_FINAL:
      return this->checkCipherFinal(crypto);
    case CryptoMethod::SET_AAD:
      return this->checkSetAad(crypto);
    case CryptoMethod::GET_AUTH_TAG:
      return this->checkGetAuthTag(crypto);
    case CryptoMethod::SET_AUTH_TAG:
      return this->checkSetAuthTag(crypto);
    case CryptoMethod::SET_AUTO_PADDING:
      return this->checkSetAutoPadding(crypto);
    case CryptoMethod::KEY_EXPORT:
      return this->checkKeyExport(crypto);
    case CryptoMethod::KEY_EQUALS:
      return this->checkKeyEquals(crypto);
    case CryptoMethod::ECDH_GENERATE_KEYS:
    case CryptoMethod::ECDH_GET_PUBLIC_KEY:
      return this->checkEcdhPublicKey(crypto);
    case CryptoMethod::ECDH_GET_PRIVATE_KEY:
      return this->checkEcdhGetPrivateKey(crypto);
    case CryptoMethod::ECDH_SET_PRIVATE_KEY:
      return this->checkEcdhSetPrivateKey(crypto);
    case CryptoMethod::ECDH_COMPUTE_SECRET:
      return this->checkEcdhComputeSecret(crypto);
    case CryptoMethod::X509_PEM:
      return this->checkX509Pem(crypto);
    case CryptoMethod::X509_CHECK_HOST:
    case CryptoMethod::X509_CHECK_EMAIL:
    case CryptoMethod::X509_CHECK_IP:
      return this->checkX509Name(crypto);
    case CryptoMethod::X509_CHECK_ISSUED:
    case CryptoMethod::X509_CHECK_PRIVATE_KEY:
    case CryptoMethod::X509_VERIFY:
      return this->checkX509Verify(crypto);
  }

  return call;
}

Expr *CryptoMethodChecker::reportNoMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  bool hash = kind == Crypto::Hash || kind == Crypto::Hmac || kind == Crypto::Sign ||
              kind == Crypto::Verify;
  bool cipher = kind == Crypto::Cipheriv || kind == Crypto::Decipheriv;
  static const llvm::StringLiteral streamMethods[] = {
      "write",   "end",         "pipe", "unpipe", "read",           "on",
      "once",    "off",         "emit", "pause",  "resume",         "setEncoding",
      "destroy", "addListener", "cork", "uncork", "removeListener", "setDefaultEncoding"};
  if ((hash || cipher) && llvm::is_contained(streamMethods, name)) {
    std::string message = ErrorTextCryptoMethod::stream(name, type->name(), kind, cipher);
    this->state.getDiagnostics().error(loc, message);
    return this->checkLoosely(call);
  }
  std::string message =
      ErrorTextCryptoMethod::noMethod(name, type->name(), CryptoNames::methodsOf(kind));
  this->state.getDiagnostics().error(loc, message);

  return this->checkLoosely(call);
}

Expr *CryptoMethodChecker::checkLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

bool CryptoMethodChecker::isPresent(const std::vector<Expr *> &args, size_t i) {
  return i < args.size() && !isa<UndefinedExpr>(args[i]);
}

Expr *CryptoMethodChecker::bytesOrText(const CryptoMethodCall &crypto, CryptoFn fn,
                                       std::vector<Expr *> fnArgs, size_t index, bool fallible) {
  CallExpr *call = crypto.call;
  Expr *encoding = this->cryptoArgs->cryptoEncoding(call, index, crypto.member->name, -1);
  if (encoding == nullptr) {
    return call;
  }
  auto *made =
      this->state.getAst().make<CallExpr>(call->loc(), call->callee, std::vector<Expr *>{});
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  if (fallible) {
    this->cryptoArgs->cryptoResult(made, fn, std::move(fnArgs), buffer);
  } else {
    this->cryptoArgs->cryptoCall(made, fn, std::move(fnArgs), buffer);
  }

  return this->cryptoArgs->cryptoText(made, encoding);
}

Expr *CryptoMethodChecker::pointFormat(const CryptoMethodCall &crypto, size_t index) {
  std::vector<Expr *> &args = crypto.call->args;
  llvm::SMLoc loc = crypto.member->loc();
  if (!CryptoMethodChecker::isPresent(args, index)) {
    return this->cryptoArgs->cryptoInt(loc, 0);
  }
  std::string point = ErrorTextCryptoPoint::format();
  int which =
      this->cryptoArgs->cryptoChoice(args[index], point, {"uncompressed", "compressed", "hybrid"});
  if (which < 0) {
    return nullptr;
  }

  return this->cryptoArgs->cryptoInt(loc, which);
}

Expr *CryptoMethodChecker::checkHashUpdate(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  const Type *type = crypto.member->object->type;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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

Expr *CryptoMethodChecker::checkDigest(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
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

Expr *CryptoMethodChecker::checkHashCopy(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  const Type *type = crypto.member->object->type;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  llvm::StringMap<Expr *> options;
  if (CryptoMethodChecker::isPresent(args, 0) &&
      !this->cryptoArgs->cryptoOptions(args[0], name, {"outputLength"}, options)) {
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

Expr *CryptoMethodChecker::checkSign(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  std::vector<Expr *> signArgs = {self};
  if (!this->cryptoArgs->cryptoSignKey(args[0], name, true, signArgs)) {
    return call;
  }
  return this->bytesOrText(crypto, CryptoFn::SignFinal, std::move(signArgs), 1, true);
}

Expr *CryptoMethodChecker::checkVerify(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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

Expr *CryptoMethodChecker::checkCipherUpdate(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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
  return this->bytesOrText(crypto, CryptoFn::CipherUpdate, {self, data, encoding}, 2, false);
}

Expr *CryptoMethodChecker::checkCipherFinal(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  return this->bytesOrText(crypto, CryptoFn::CipherFinal, {self}, 0, true);
}

Expr *CryptoMethodChecker::checkSetAad(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  const Type *type = crypto.member->object->type;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *data = this->cryptoArgs->cryptoData(args[0], "AAD", false, false, true);
  llvm::StringMap<Expr *> options;
  if (CryptoMethodChecker::isPresent(args, 1) &&
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

Expr *CryptoMethodChecker::checkGetAuthTag(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  return this->cryptoArgs->cryptoResult(call, CryptoFn::CipherGetAuthTag, {self}, buffer);
}

Expr *CryptoMethodChecker::checkSetAuthTag(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  const Type *type = crypto.member->object->type;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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

Expr *CryptoMethodChecker::checkSetAutoPadding(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  const Type *type = crypto.member->object->type;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *padding = CryptoMethodChecker::isPresent(args, 0)
                      ? this->argumentChecker->checkArg(args[0], Type::boolTy())
                      : this->cryptoArgs->cryptoBool(loc, true);
  return this->cryptoArgs->cryptoCall(call, CryptoFn::CipherSetAutoPadding, {self, padding}, type);
}

Expr *CryptoMethodChecker::checkKeyExport(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  if (!CryptoMethodChecker::isPresent(args, 0)) {
    return this->cryptoArgs->cryptoResult(call, CryptoFn::KeyExportSecret, {self}, buffer);
  }
  llvm::StringMap<Expr *> options;
  bool read = this->cryptoArgs->cryptoOptions(args[0], name,
                                              {"format", "type", "cipher", "passphrase"}, options);
  if (!read) {
    return call;
  }
  int format = 3;
  Expr *formatOption = options.lookup("format");
  if (formatOption != nullptr) {
    std::string keyFormat = ErrorTextCryptoFnLabel::keyFormat();
    format =
        this->cryptoArgs->cryptoChoice(formatOption, keyFormat, {"pem", "der", "jwk", "buffer"});
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

  return this->checkEncodedKeyExport(crypto, options, format == 0);
}

Expr *CryptoMethodChecker::checkEncodedKeyExport(const CryptoMethodCall &crypto,
                                                 llvm::StringMap<Expr *> &options, bool pem) {
  CallExpr *call = crypto.call;
  llvm::SMLoc loc = crypto.member->loc();
  int has = 0;
  Expr *keyType = this->cryptoArgs->cryptoString(loc, "");
  Expr *cipherName = this->cryptoArgs->cryptoString(loc, "");
  Expr *passphrase = this->cryptoArgs->cryptoUndefined(loc);
  Expr *typeOption = options.lookup("type");
  if (typeOption != nullptr) {
    std::string typeWhat = ErrorTextCryptoFnLabel::keyType();
    int which =
        this->cryptoArgs->cryptoChoice(typeOption, typeWhat, {"pkcs1", "spki", "pkcs8", "sec1"});
    if (which < 0) {
      return call;
    }
    static const char *const types[] = {"pkcs1", "spki", "pkcs8", "sec1"};
    keyType = this->cryptoArgs->cryptoString(typeOption->loc(), types[which]);
    has |= 1;
  }
  Expr *cipherOption = options.lookup("cipher");
  if (cipherOption != nullptr) {
    cipherName = this->argumentChecker->checkArg(cipherOption, Type::string());
    has |= 2;
  }
  Expr *passphraseOption = options.lookup("passphrase");
  if (passphraseOption != nullptr) {
    passphrase = this->cryptoArgs->cryptoData(passphraseOption, "passphrase", true, false, true);
    has |= 4;
    if (passphrase->type->isError()) {
      return call;
    }
  }
  CryptoFn fn = pem ? CryptoFn::KeyExportPem : CryptoFn::KeyExportDer;
  const Type *result = pem ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  std::vector<Expr *> exportArgs{crypto.member->object, this->cryptoArgs->cryptoInt(loc, has),
                                 keyType, cipherName, passphrase};

  return this->cryptoArgs->cryptoResult(call, fn, std::move(exportArgs), result);
}

Expr *CryptoMethodChecker::checkKeyEquals(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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

Expr *CryptoMethodChecker::checkEcdhPublicKey(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *f = this->pointFormat(crypto, 1);
  if (!f) {
    return call;
  }
  return this->bytesOrText(crypto,
                           crypto.method == CryptoMethod::ECDH_GENERATE_KEYS
                               ? CryptoFn::EcdhGenerateKeys
                               : CryptoFn::EcdhGetPublicKey,
                           {self, f}, 0, crypto.method == CryptoMethod::ECDH_GET_PUBLIC_KEY);
}

Expr *CryptoMethodChecker::checkEcdhGetPrivateKey(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  return this->bytesOrText(crypto, CryptoFn::EcdhGetPrivateKey, {self}, 0, true);
}

Expr *CryptoMethodChecker::checkEcdhSetPrivateKey(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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
  return this->cryptoArgs->cryptoResult(call, CryptoFn::EcdhSetPrivateKey, {self, key, encoding},
                                        Type::voidTy());
}

Expr *CryptoMethodChecker::checkEcdhComputeSecret(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
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
  return this->bytesOrText(crypto, CryptoFn::EcdhComputeSecret, {self, key, encoding}, 2, true);
}

Expr *CryptoMethodChecker::checkX509Pem(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  return this->cryptoArgs->cryptoCall(call, CryptoFn::X509Pem, {self}, Type::string());
}

Expr *CryptoMethodChecker::checkX509Name(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::StringRef name = crypto.member->name;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(
      call, 1, crypto.method == CryptoMethod::X509_CHECK_IP ? 1 : 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *value = this->argumentChecker->checkArg(args[0], Type::string());
  llvm::StringMap<Expr *> options;
  std::vector<llvm::StringRef> allowed = {"subject"};
  if (crypto.method == CryptoMethod::X509_CHECK_HOST) {
    allowed.insert(allowed.end(), {"wildcards", "partialWildcards", "multiLabelWildcards",
                                   "singleLabelSubdomains"});
  }
  if (CryptoMethodChecker::isPresent(args, 1) &&
      !this->cryptoArgs->cryptoOptions(args[1], name, allowed, options)) {
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
  if (crypto.method == CryptoMethod::X509_CHECK_IP) {
    return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckIp, {self, value}, optional);
  }
  if (crypto.method == CryptoMethod::X509_CHECK_EMAIL) {
    return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckEmail,
                                        {self, value, this->cryptoArgs->cryptoInt(loc, subject)},
                                        optional);
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
  return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckHost, std::move(hostArgs), optional);
}

Expr *CryptoMethodChecker::checkX509Verify(const CryptoMethodCall &crypto) {
  CallExpr *call = crypto.call;
  std::vector<Expr *> &args = call->args;
  llvm::SMLoc loc = crypto.member->loc();
  Expr *self = crypto.member->object;
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *other = crypto.method == CryptoMethod::X509_CHECK_ISSUED
                    ? this->exprChecker->checkExpr(args[0])
                    : this->cryptoArgs->cryptoKeyObject(args[0], "key");
  ResultIssuerChecked issued = crypto.method == CryptoMethod::X509_CHECK_ISSUED
                                   ? CryptoMethodChecker::issuer(other)
                                   : ok(other);
  if (issued.isErr()) {
    this->state.getDiagnostics().error(other->loc(), issued.error().message);
    return call;
  }
  if (other->type->isError()) {
    return call;
  }
  if (crypto.method == CryptoMethod::X509_CHECK_ISSUED) {
    return this->cryptoArgs->cryptoCall(call, CryptoFn::X509CheckIssued, {self, other},
                                        Type::boolTy());
  }
  return this->cryptoArgs->cryptoResult(call,
                                        crypto.method == CryptoMethod::X509_CHECK_PRIVATE_KEY
                                            ? CryptoFn::X509CheckPrivateKey
                                            : CryptoFn::X509Verify,
                                        {self, other}, Type::boolTy());
}

}
