#include "compiler/semantic/library/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/semantic/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/semantic/library/crypto/crypto_errors/crypto_errors.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

KeyPairChecker::KeyPairChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->cryptoArgs = &all.getCryptoArgs();
  this->cryptoErrors = &all.getCryptoErrors();
  this->argumentChecker = &all.getArgumentChecker();
}

std::optional<std::string> KeyPairChecker::literalOf(const Expr *e) {
  if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
    e = conv->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(e)) {
    return ToUtf8::toUtf8(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(e); tmpl && tmpl->exprs.empty()) {
    return ToUtf8::toUtf8(tmpl->quasis[0]);
  }

  return std::nullopt;
}

KeyPairChecker::ResultKeyOptionsGiven
KeyPairChecker::keyOptions(int which, llvm::StringRef type,
                           const llvm::StringMap<Expr *> &options) {
  if (which >= 0 && which <= 2 && !options.lookup("modulusLength")) {
    std::string message = ErrorTextKeyPair::modulus(type);

    return err(KeyPairCheckerKeyOptionsError{
        .code = KeyPairCheckerKeyOptionsErrorCode::NO_MODULUS_LENGTH,
        .message = message,
    });
  }
  if (which == 3 && !options.lookup("namedCurve")) {
    std::string message = ErrorTextKeyPair::curve();

    return err(KeyPairCheckerKeyOptionsError{
        .code = KeyPairCheckerKeyOptionsErrorCode::NO_NAMED_CURVE,
        .message = message,
    });
  }

  return ok(which);
}

Expr *KeyPairChecker::checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc,
                                           llvm::StringRef shown) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  std::optional<KeyPairKind> kind = this->keyPairKind(call, loc);
  if (!kind.has_value()) {
    return call;
  }
  llvm::StringMap<Expr *> options;
  if (!this->keyPairOptionsRead(call, shown, options)) {
    return call;
  }
  llvm::StringRef typeName =
      kind->which >= 0 ? KeyPairChecker::pairTypeNames()[kind->which] : llvm::StringRef();
  ResultKeyOptionsGiven given = KeyPairChecker::keyOptions(kind->which, typeName, options);
  if (given.isErr()) {
    this->state.getDiagnostics().error(loc, given.error().message);
    return call;
  }
  std::optional<KeyPairNumbers> numbers = this->keyPairNumbers(loc, kind->which, options);
  if (!numbers.has_value()) {
    return call;
  }
  std::vector<Expr *> parts = {this->generateKeyCall(call, loc, *kind, *numbers)};
  const Type *publicKey = this->appendedEncoding(loc, options, false, parts);
  if (publicKey == nullptr) {
    return call;
  }
  const Type *privateKey = this->appendedEncoding(loc, options, true, parts);
  if (privateKey == nullptr) {
    return call;
  }
  call->args = std::move(parts);
  call->spread.clear();
  call->target = BuiltinCall{.builtin = Builtin::CryptoKeyPair};
  const Type *pair =
      Type::object({{"publicKey", publicKey, false}, {"privateKey", privateKey, false}}, "KeyPair");
  call->type = Type::result(pair, this->cryptoErrors->named("GenerateKeyPairError"));

  return call;
}

llvm::ArrayRef<llvm::StringRef> KeyPairChecker::pairTypeNames() {
  static const llvm::StringRef types[] = {"rsa",     "rsa-pss", "dsa",    "ec",
                                          "ed25519", "ed448",   "x25519", "x448"};

  return types;
}

std::optional<KeyPairChecker::KeyPairKind> KeyPairChecker::keyPairKind(CallExpr *call,
                                                                       llvm::SMLoc loc) {
  KeyPairKind kind{
      .which = -1,
      .name = this->cryptoArgs->cryptoString(loc, ""),
  };
  if (!KeyPairChecker::literalOf(call->args[0]).has_value()) {
    kind.name = this->argumentChecker->checkArg(call->args[0], Type::string());
    return kind;
  }
  std::string label = ErrorTextKeyPairLabel::pairType();
  llvm::ArrayRef<llvm::StringRef> types = KeyPairChecker::pairTypeNames();
  kind.which = this->cryptoArgs->cryptoChoice(call->args[0], label, types);
  if (kind.which < 0) {
    return std::nullopt;
  }

  return kind;
}

bool KeyPairChecker::keyPairOptionsRead(CallExpr *call, llvm::StringRef shown,
                                        llvm::StringMap<Expr *> &options) {
  bool read =
      call->args.size() <= 1 ||
      this->cryptoArgs->cryptoOptions(call->args[1], shown,
                                      {"modulusLength", "publicExponent", "namedCurve",
                                       "publicKeyEncoding", "privateKeyEncoding", "divisorLength",
                                       "hashAlgorithm", "mgf1HashAlgorithm", "saltLength"},
                                      options);
  if (!read) {
    return false;
  }
  for (llvm::StringRef unsupported : {"hashAlgorithm", "mgf1HashAlgorithm", "saltLength"}) {
    Expr *e = options.lookup(unsupported);
    if (e != nullptr) {
      std::string message = ErrorTextKeyPair::option(unsupported);
      this->state.getDiagnostics().error(e->loc(), message);
      return false;
    }
  }

  return true;
}

std::optional<KeyPairChecker::KeyPairNumbers>
KeyPairChecker::keyPairNumbers(llvm::SMLoc loc, int which, const llvm::StringMap<Expr *> &options) {
  KeyPairNumbers numbers{
      .modulus = this->cryptoArgs->cryptoDouble(loc, 0),
      .exponent = this->cryptoArgs->cryptoDouble(loc, 65537),
      .curve = this->cryptoArgs->cryptoString(loc, ""),
      .has = 0,
  };
  Expr *m = options.lookup("modulusLength");
  if (m != nullptr) {
    numbers.modulus = this->cryptoArgs->cryptoNumber(m);
    std::optional<double> bits = this->cryptoArgs->knownNumber(numbers.modulus);
    bool rsa = which == 0 || which == 1;
    if (rsa && bits.has_value() && *bits < 512) {
      std::string message = ErrorTextKeyPair::small(CryptoArgs::numberText(*bits));
      this->state.getDiagnostics().error(m->loc(), message);
      return std::nullopt;
    }
  }
  Expr *e = options.lookup("publicExponent");
  if (e != nullptr) {
    numbers.exponent = this->cryptoArgs->cryptoNumber(e);
    numbers.has |= 2;
  }
  Expr *d = options.lookup("divisorLength");
  if (d != nullptr) {
    this->cryptoArgs->cryptoNumber(d);
  }
  Expr *c = options.lookup("namedCurve");
  if (c != nullptr) {
    numbers.curve = this->argumentChecker->checkArg(c, Type::string());
    numbers.has |= 1;
  }

  return numbers;
}

Expr *KeyPairChecker::generateKeyCall(CallExpr *call, llvm::SMLoc loc, const KeyPairKind &kind,
                                      const KeyPairNumbers &numbers) {
  auto *generate = this->state.getAst().make<CallExpr>(loc, call->callee, std::vector<Expr *>{});
  Expr *which = this->cryptoArgs->cryptoInt(loc, kind.which < 0 ? 0xffffffff : kind.which);
  Expr *has = this->cryptoArgs->cryptoInt(loc, numbers.has);
  std::vector<Expr *> args = {which, kind.name,    numbers.modulus, numbers.exponent,
                              has,   numbers.curve};
  this->cryptoArgs->cryptoCall(generate, CryptoFn::GenerateKeyPair, std::move(args),
                               Type::cryptoType(Crypto::KeyObject));

  return generate;
}

const Type *KeyPairChecker::appendedEncoding(llvm::SMLoc loc,
                                             const llvm::StringMap<Expr *> &options, bool isPrivate,
                                             std::vector<Expr *> &parts) {
  llvm::StringRef option = isPrivate ? "privateKeyEncoding" : "publicKeyEncoding";
  Expr *encoding = options.lookup(option);
  KeyEncoding chosen{
      .format = -1,
      .flags = 0,
      .type = this->cryptoArgs->cryptoString(loc, ""),
      .cipher = this->cryptoArgs->cryptoString(loc, ""),
      .passphrase = this->cryptoArgs->cryptoUndefined(loc),
  };
  if (encoding != nullptr && !this->encodingRead(encoding, option, isPrivate, chosen)) {
    return nullptr;
  }
  Expr *format = this->cryptoArgs->cryptoInt(loc, chosen.format < 0 ? 0xffffffff : chosen.format);
  Expr *flags = this->cryptoArgs->cryptoInt(loc, chosen.flags);
  parts.insert(parts.end(), {format, flags, chosen.type, chosen.cipher, chosen.passphrase});

  return KeyPairChecker::encodedKeyType(chosen.format);
}

bool KeyPairChecker::encodingRead(Expr *encoding, llvm::StringRef option, bool isPrivate,
                                  KeyEncoding &chosen) {
  llvm::StringMap<Expr *> e;
  llvm::SmallVector<llvm::StringRef, 4> allowed = {"type", "format"};
  if (isPrivate) {
    allowed.append({"cipher", "passphrase"});
  }
  if (!this->cryptoArgs->cryptoOptions(encoding, option, allowed, e)) {
    return false;
  }
  Expr *format = e.lookup("format");
  if (format == nullptr) {
    std::string message = ErrorTextKeyPair::noFormat(option);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  std::string formatLabel = ErrorTextKeyPairLabel::format();
  chosen.format = this->cryptoArgs->cryptoChoice(format, formatLabel, {"pem", "der", "jwk"});
  if (chosen.format < 0) {
    return false;
  }
  if (!this->encodingTypeRead(encoding, option, isPrivate, e, chosen)) {
    return false;
  }

  return this->encodingCipherRead(encoding, e, chosen);
}

bool KeyPairChecker::encodingTypeRead(const Expr *encoding, llvm::StringRef option, bool isPrivate,
                                      const llvm::StringMap<Expr *> &e, KeyEncoding &chosen) {
  Expr *t = e.lookup("type");
  if (t == nullptr && chosen.format == 2) {
    return true;
  }
  if (t == nullptr) {
    std::string message = ErrorTextKeyPair::noType(option, !isPrivate);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  std::string typeLabel = ErrorTextKeyPairLabel::keyType();
  int typeIndex = this->cryptoArgs->cryptoChoice(t, typeLabel, {"pkcs1", "spki", "pkcs8", "sec1"});
  if (typeIndex < 0) {
    return false;
  }
  static const char *const names[] = {"pkcs1", "spki", "pkcs8", "sec1"};
  chosen.type = this->cryptoArgs->cryptoString(t->loc(), names[typeIndex]);
  chosen.flags |= 1;

  return true;
}

bool KeyPairChecker::encodingCipherRead(const Expr *encoding, const llvm::StringMap<Expr *> &e,
                                        KeyEncoding &chosen) {
  Expr *c = e.lookup("cipher");
  if (c != nullptr) {
    chosen.cipher = this->argumentChecker->checkArg(c, Type::string());
    chosen.flags |= 2;
    if (e.lookup("passphrase") == nullptr) {
      std::string message = ErrorTextKeyPair::passphrase();
      this->state.getDiagnostics().error(encoding->loc(), message);
      return false;
    }
  }
  Expr *p = e.lookup("passphrase");
  if (p == nullptr) {
    return true;
  }
  chosen.passphrase = this->cryptoArgs->cryptoData(p, "passphrase", true, false, true);
  chosen.flags |= 4;

  return !chosen.passphrase->type->isError();
}

const Type *KeyPairChecker::encodedKeyType(int format) {
  if (format < 0) {
    return Type::cryptoType(Crypto::KeyObject);
  }
  if (format == 0) {
    return Type::string();
  }
  if (format == 1) {
    return Type::bytesType(Type::Bytes::Buffer);
  }

  return Type::json();
}
}
