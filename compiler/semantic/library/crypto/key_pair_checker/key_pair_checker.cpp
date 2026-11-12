#include "compiler/semantic/library/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/program/library/crypto/key_pair_shape/key_pair_shape.h"
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
KeyPairChecker::checkKeyOptions(int which, llvm::StringRef type,
                                const CryptoOptions &options) const {
  bool needsModulus = which >= int(KeyPairType::RSA) && which <= int(KeyPairType::DSA);
  if (needsModulus && !options.lookup(CryptoOption::MODULUS_LENGTH)) {
    auto message = ErrorTextKeyPair::modulus(type);

    return err(KeyPairCheckerKeyOptionsError{
        .code = KeyPairCheckerKeyOptionsErrorCode::NO_MODULUS_LENGTH,
        .message = message,
    });
  }
  if (which == int(KeyPairType::EC) && !options.lookup(CryptoOption::NAMED_CURVE)) {
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
  auto options = CryptoOptions();
  if (!this->readKeyPairOptions(call, shown, options)) {
    return call;
  }
  auto types = CryptoChoiceTable::getValues(CryptoChoice::KEY_PAIR_TYPE);
  llvm::StringRef typeName = kind->which >= 0 ? types[kind->which] : llvm::StringRef();
  auto given = this->checkKeyOptions(kind->which, typeName, options);
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
  auto publicName = KeyPairShape::getFieldName(KeyPairField::PUBLIC_KEY);
  auto privateName = KeyPairShape::getFieldName(KeyPairField::PRIVATE_KEY);
  const auto *pair =
      Type::object({{publicName.str(), publicKey, false}, {privateName.str(), privateKey, false}},
                   KeyPairShape::getTypeName());
  auto errorName = CryptoErrorTypes::callErrorName(CryptoFn::GenerateKeyPair);
  call->type = Type::result(pair, this->cryptoErrors->named(*errorName));

  return call;
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
  kind.which = this->cryptoArgs->cryptoChoice(call->args[0], label, CryptoChoice::KEY_PAIR_TYPE);
  if (kind.which < 0) {
    return std::nullopt;
  }

  return kind;
}

bool KeyPairChecker::readKeyPairOptions(CallExpr *call, llvm::StringRef shown,
                                        CryptoOptions &options) {
  bool isRead =
      call->args.size() <= 1 ||
      this->cryptoArgs->readCryptoOptions(
          call->args[1], shown,
          {CryptoOption::MODULUS_LENGTH, CryptoOption::PUBLIC_EXPONENT, CryptoOption::NAMED_CURVE,
           CryptoOption::PUBLIC_KEY_ENCODING, CryptoOption::PRIVATE_KEY_ENCODING,
           CryptoOption::DIVISOR_LENGTH, CryptoOption::HASH_ALGORITHM,
           CryptoOption::MGF1_HASH_ALGORITHM, CryptoOption::SALT_LENGTH},
          options);
  if (!isRead) {
    return false;
  }
  for (CryptoOption unsupported : {CryptoOption::HASH_ALGORITHM, CryptoOption::MGF1_HASH_ALGORITHM,
                                   CryptoOption::SALT_LENGTH}) {
    Expr *expr = options.lookup(unsupported);
    if (expr != nullptr) {
      auto name = CryptoOptionTable::getName(unsupported);
      auto message = ErrorTextKeyPair::option(name);
      this->state.getDiagnostics().error(expr->loc(), message);
      return false;
    }
  }

  return true;
}

std::optional<KeyPairChecker::KeyPairNumbers>
KeyPairChecker::keyPairNumbers(llvm::SMLoc loc, int which, const CryptoOptions &options) {
  KeyPairNumbers numbers{
      .modulus = this->cryptoArgs->cryptoDouble(loc, 0),
      .exponent = this->cryptoArgs->cryptoDouble(loc, 65537),
      .curve = this->cryptoArgs->cryptoString(loc, ""),
      .has = 0,
  };
  Expr *m = options.lookup(CryptoOption::MODULUS_LENGTH);
  if (m != nullptr) {
    numbers.modulus = this->cryptoArgs->cryptoNumber(m);
    auto bits = this->cryptoArgs->knownNumber(numbers.modulus);
    bool isRsa = which == int(KeyPairType::RSA) || which == int(KeyPairType::RSA_PSS);
    if (isRsa && bits.has_value() && *bits < 512) {
      auto message = ErrorTextKeyPair::small(this->cryptoArgs->numberText(*bits));
      this->state.getDiagnostics().error(m->loc(), message);
      return std::nullopt;
    }
  }
  Expr *expr = options.lookup(CryptoOption::PUBLIC_EXPONENT);
  if (expr != nullptr) {
    numbers.exponent = this->cryptoArgs->cryptoNumber(expr);
    numbers.has |= 2;
  }
  Expr *d = options.lookup(CryptoOption::DIVISOR_LENGTH);
  if (d != nullptr) {
    this->cryptoArgs->cryptoNumber(d);
  }
  Expr *c = options.lookup(CryptoOption::NAMED_CURVE);
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

const Type *KeyPairChecker::appendedEncoding(llvm::SMLoc loc, const CryptoOptions &options,
                                             bool isPrivate, std::vector<Expr *> &parts) {
  CryptoOption option =
      isPrivate ? CryptoOption::PRIVATE_KEY_ENCODING : CryptoOption::PUBLIC_KEY_ENCODING;
  auto optionName = CryptoOptionTable::getName(option);
  Expr *encoding = options.lookup(option);
  KeyEncoding chosen{
      .format = -1,
      .flags = 0,
      .type = this->cryptoArgs->cryptoString(loc, ""),
      .cipher = this->cryptoArgs->cryptoString(loc, ""),
      .passphrase = this->cryptoArgs->cryptoUndefined(loc),
  };
  if (encoding != nullptr && !this->readEncoding(encoding, optionName, isPrivate, chosen)) {
    return nullptr;
  }
  auto *format = this->cryptoArgs->cryptoInt(loc, chosen.format < 0 ? 0xffffffff : chosen.format);
  auto *flags = this->cryptoArgs->cryptoInt(loc, chosen.flags);
  parts.insert(parts.end(), {format, flags, chosen.type, chosen.cipher, chosen.passphrase});

  return this->encodedKeyType(chosen.format);
}

bool KeyPairChecker::readEncoding(Expr *encoding, llvm::StringRef option, bool isPrivate,
                                  KeyEncoding &chosen) {
  auto e = CryptoOptions();
  llvm::SmallVector<CryptoOption, 4> allowed = {CryptoOption::TYPE, CryptoOption::FORMAT};
  if (isPrivate) {
    allowed.append({CryptoOption::CIPHER, CryptoOption::PASSPHRASE});
  }
  if (!this->cryptoArgs->readCryptoOptions(encoding, option, allowed, e)) {
    return false;
  }
  Expr *format = e.lookup(CryptoOption::FORMAT);
  if (format == nullptr) {
    auto message = ErrorTextKeyPair::noFormat(option);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  auto formatLabel = ErrorTextKeyPairLabel::format();
  chosen.format = this->cryptoArgs->cryptoChoice(format, formatLabel, CryptoChoice::KEY_FORMAT);
  if (chosen.format < 0) {
    return false;
  }
  if (!this->readEncodingType(encoding, option, isPrivate, e, chosen)) {
    return false;
  }

  return this->readEncodingCipher(encoding, e, chosen);
}

bool KeyPairChecker::readEncodingType(const Expr *encoding, llvm::StringRef option, bool isPrivate,
                                      const CryptoOptions &e, KeyEncoding &chosen) {
  Expr *t = e.lookup(CryptoOption::TYPE);
  if (t == nullptr && chosen.format == int(CryptoKeyFormat::JWK)) {
    return true;
  }
  if (t == nullptr) {
    auto message = ErrorTextKeyPair::noType(option, !isPrivate);
    this->state.getDiagnostics().error(encoding->loc(), message);
    return false;
  }
  auto typeLabel = ErrorTextKeyPairLabel::keyType();
  int typeIndex = this->cryptoArgs->cryptoChoice(t, typeLabel, CryptoChoice::KEY_ENCODING_TYPE);
  if (typeIndex < 0) {
    return false;
  }
  auto names = CryptoChoiceTable::getValues(CryptoChoice::KEY_ENCODING_TYPE);
  chosen.type = this->cryptoArgs->cryptoString(t->loc(), names[typeIndex]);
  chosen.flags |= 1;

  return true;
}

bool KeyPairChecker::readEncodingCipher(const Expr *encoding, const CryptoOptions &e,
                                        KeyEncoding &chosen) {
  Expr *c = e.lookup(CryptoOption::CIPHER);
  if (c != nullptr) {
    chosen.cipher = this->argumentChecker->checkArg(c, Type::string());
    chosen.flags |= 2;
    if (e.lookup(CryptoOption::PASSPHRASE) == nullptr) {
      auto message = ErrorTextKeyPair::passphrase();
      this->state.getDiagnostics().error(encoding->loc(), message);
      return false;
    }
  }
  Expr *p = e.lookup(CryptoOption::PASSPHRASE);
  if (p == nullptr) {
    return true;
  }
  chosen.passphrase =
      this->cryptoArgs->cryptoOptionData(e, CryptoOption::PASSPHRASE, kTextOrBuffers);
  chosen.flags |= 4;

  return !chosen.passphrase->type->isError();
}

const Type *KeyPairChecker::encodedKeyType(int format) const {
  if (format < 0) {
    return Type::cryptoType(Crypto::KeyObject);
  }
  if (format == int(CryptoKeyFormat::PEM)) {
    return Type::string();
  }
  if (format == int(CryptoKeyFormat::DER)) {
    return Type::bytesType(Type::Bytes::Buffer);
  }

  return Type::json();
}

}
