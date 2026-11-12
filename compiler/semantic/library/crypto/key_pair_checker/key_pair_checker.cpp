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
  this->cryptoArgs = &all.cryptoArgs;
  this->cryptoErrors = &all.cryptoErrors;
  this->argumentChecker = &all.argumentChecker;
}

std::optional<std::string> KeyPairChecker::literalOf(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
  }
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }

  return std::nullopt;
}

KeyPairChecker::ResultKeyOptionsGiven
KeyPairChecker::keyOptions(int which, llvm::StringRef type,
                           const llvm::StringMap<Expr *> &options) const {
  if (which >= 0 && which <= 2 && !options.lookup("modulusLength")) {
    auto message = ErrorTextKeyPair::modulus(type);

    return err(KeyPairCheckerKeyOptionsError{
        .code = KeyPairCheckerKeyOptionsErrorCode::NO_MODULUS_LENGTH,
        .message = message,
    });
  }
  if (which == 3 && !options.lookup("namedCurve")) {
    auto message = ErrorTextKeyPair::curve();

    return err(KeyPairCheckerKeyOptionsError{
        .code = KeyPairCheckerKeyOptionsErrorCode::NO_NAMED_CURVE,
        .message = message,
    });
  }

  return ok(which);
}

Expr *KeyPairChecker::checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc,
                                           llvm::StringRef shown) {
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  std::optional<KeyPairKind> kind = this->keyPairKind(call, loc);
  if (!kind.has_value()) {
    return call;
  }
  auto options = llvm::StringMap<Expr *>();
  if (!this->keyPairOptionsRead(call, shown, options)) {
    return call;
  }
  llvm::StringRef typeName =
      kind->which >= 0 ? this->pairTypeNames()[kind->which] : llvm::StringRef();
  auto given = this->keyOptions(kind->which, typeName, options);
  if (given.isErr()) {
    this->state.getDiagnostics().error(loc, given.error().message);
    return call;
  }
  std::optional<KeyPairNumbers> numbers = this->keyPairNumbers(loc, kind->which, options);
  if (!numbers.has_value()) {
    return call;
  }
  std::vector<Expr *> parts = {this->generateKeyCall(call, loc, *kind, *numbers)};
  const auto *publicKey = this->appendedEncoding(loc, options, false, parts);
  if (publicKey == nullptr) {
    return call;
  }
  const auto *privateKey = this->appendedEncoding(loc, options, true, parts);
  if (privateKey == nullptr) {
    return call;
  }
  call->args = std::move(parts);
  call->spread.clear();
  call->target = BuiltinCall{.builtin = Builtin::CryptoKeyPair};
  const auto *pair =
      Type::object({{"publicKey", publicKey, false}, {"privateKey", privateKey, false}}, "KeyPair");
  call->type = Type::result(pair, this->cryptoErrors->named("GenerateKeyPairError"));

  return call;
}

llvm::ArrayRef<llvm::StringRef> KeyPairChecker::pairTypeNames() const {
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
  if (!this->literalOf(call->args[0]).has_value()) {
    kind.name = this->argumentChecker->checkArg(call->args[0], Type::string());
    return kind;
  }
  auto label = ErrorTextKeyPairLabel::pairType();
  auto types = this->pairTypeNames();
  kind.which = this->cryptoArgs->cryptoChoice(call->args[0], label, types);
  if (kind.which < 0) {
    return std::nullopt;
  }

  return kind;
}

bool KeyPairChecker::keyPairOptionsRead(CallExpr *call, llvm::StringRef shown,
                                        llvm::StringMap<Expr *> &options) {
  bool isRead =
      call->args.size() <= 1 ||
      this->cryptoArgs->cryptoOptions(call->args[1], shown,
                                      {"modulusLength", "publicExponent", "namedCurve",
                                       "publicKeyEncoding", "privateKeyEncoding", "divisorLength",
                                       "hashAlgorithm", "mgf1HashAlgorithm", "saltLength"},
                                      options);
  if (!isRead) {
    return false;
  }
  for (llvm::StringRef unsupported : {"hashAlgorithm", "mgf1HashAlgorithm", "saltLength"}) {
    Expr *expr = options.lookup(unsupported);
    if (expr != nullptr) {
      auto message = ErrorTextKeyPair::option(unsupported);
      this->state.getDiagnostics().error(expr->loc(), message);
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
    auto bits = this->cryptoArgs->knownNumber(numbers.modulus);
    bool isRsa = which == 0 || which == 1;
    if (isRsa && bits.has_value() && *bits < 512) {
      auto message = ErrorTextKeyPair::small(this->cryptoArgs->numberText(*bits));
      this->state.getDiagnostics().error(m->loc(), message);
      return std::nullopt;
    }
  }
  Expr *expr = options.lookup("publicExponent");
  if (expr != nullptr) {
    numbers.exponent = this->cryptoArgs->cryptoNumber(expr);
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
  auto *which = this->cryptoArgs->cryptoInt(loc, kind.which < 0 ? 0xffffffff : kind.which);
  auto *has = this->cryptoArgs->cryptoInt(loc, numbers.has);
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
  auto *format = this->cryptoArgs->cryptoInt(loc, chosen.format < 0 ? 0xffffffff : chosen.format);
  auto *flags = this->cryptoArgs->cryptoInt(loc, chosen.flags);
  parts.insert(parts.end(), {format, flags, chosen.type, chosen.cipher, chosen.passphrase});

  return this->encodedKeyType(chosen.format);
}

bool KeyPairChecker::encodingRead(Expr *encoding, llvm::StringRef option, bool isPrivate,
                                  KeyEncoding &chosen) {
  auto e = llvm::StringMap<Expr *>();
  llvm::SmallVector<llvm::StringRef, 4> allowed = {"type", "format"};
  if (isPrivate) {
    allowed.append({"cipher", "passphrase"});
  }
  if (!this->cryptoArgs->cryptoOptions(encoding, option, allowed, e)) {
    return false;
  }
  Expr *format = e.lookup("format");
  if (format == nullptr) {
    auto message = ErrorTextKeyPair::noFormat(option);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  auto formatLabel = ErrorTextKeyPairLabel::format();
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
    auto message = ErrorTextKeyPair::noType(option, !isPrivate);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  auto typeLabel = ErrorTextKeyPairLabel::keyType();
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
      auto message = ErrorTextKeyPair::passphrase();
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

const Type *KeyPairChecker::encodedKeyType(int format) const {
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
