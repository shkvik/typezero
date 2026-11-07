#include "compiler/pipeline/check_program/topics/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_args/crypto_args.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_error_types/crypto_error_types.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

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

KeyPairChecker::KeyPairChecker(CheckState &owner)
    : state(owner) {}

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
  std::vector<Expr *> &args = call->args;
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  static const llvm::StringRef types[] = {"rsa",     "rsa-pss", "dsa",    "ec",
                                          "ed25519", "ed448",   "x25519", "x448"};
  int which = -1;
  Expr *name = CryptoArgs(this->state).cryptoString(loc, "");
  if (KeyPairChecker::literalOf(args[0])) {
    std::string label = ErrorTextKeyPairLabel::pairType();
    which = CryptoArgs(this->state).cryptoChoice(args[0], label, types);
    if (which < 0) {
      return call;
    }
  } else {
    name = ArgumentChecker(this->state).checkArg(args[0], Type::string());
  }
  llvm::StringMap<Expr *> options;
  if (args.size() > 1 &&
      !CryptoArgs(this->state)
           .cryptoOptions(args[1], shown,
                          {"modulusLength", "publicExponent", "namedCurve", "publicKeyEncoding",
                           "privateKeyEncoding", "divisorLength", "hashAlgorithm",
                           "mgf1HashAlgorithm", "saltLength"},
                          options)) {
    return call;
  }
  for (llvm::StringRef unsupported : {"hashAlgorithm", "mgf1HashAlgorithm", "saltLength"}) {
    if (Expr *e = options.lookup(unsupported)) {
      std::string message = ErrorTextKeyPair::option(unsupported);
      this->state.getDiagnostics().error(e->loc(), message);
      return call;
    }
  }
  llvm::StringRef typeName = which >= 0 ? types[which] : llvm::StringRef();
  ResultKeyOptionsGiven given = KeyPairChecker::keyOptions(which, typeName, options);
  if (given.isErr()) {
    this->state.getDiagnostics().error(loc, given.error().message);
    return call;
  }
  int has = 0;
  Expr *modulus = CryptoArgs(this->state).cryptoDouble(loc, 0),
       *exponent = CryptoArgs(this->state).cryptoDouble(loc, 65537);
  Expr *curve = CryptoArgs(this->state).cryptoString(loc, "");
  if (Expr *m = options.lookup("modulusLength")) {
    modulus = CryptoArgs(this->state).cryptoNumber(m);
    std::optional<double> bits = CryptoArgs(this->state).knownNumber(modulus);
    if ((which == 0 || which == 1) && bits && *bits < 512) {
      std::string message = ErrorTextKeyPair::small(CryptoArgs::numberText(*bits));
      this->state.getDiagnostics().error(m->loc(), message);
      return call;
    }
  }
  if (Expr *e = options.lookup("publicExponent")) {
    exponent = CryptoArgs(this->state).cryptoNumber(e);
    has |= 2;
  }
  if (Expr *d = options.lookup("divisorLength")) {
    CryptoArgs(this->state).cryptoNumber(d);
  }
  if (Expr *c = options.lookup("namedCurve")) {
    curve = ArgumentChecker(this->state).checkArg(c, Type::string());
    has |= 1;
  }
  auto *generate = this->state.getAst().make<CallExpr>(loc, call->callee, std::vector<Expr *>{});
  CryptoArgs(this->state)
      .cryptoCall(generate, CryptoFn::GenerateKeyPair,
                  {CryptoArgs(this->state).cryptoInt(loc, which < 0 ? 0xffffffff : which), name,
                   modulus, exponent, CryptoArgs(this->state).cryptoInt(loc, has), curve},
                  Type::cryptoType(Crypto::KeyObject));
  std::vector<Expr *> parts = {generate};
  const Type *fields[2];
  for (int k = 0; k < 2; ++k) {
    llvm::StringRef option = k == 0 ? "publicKeyEncoding" : "privateKeyEncoding";
    Expr *encoding = options.lookup(option);
    int format = -1;
    int flags = 0;
    Expr *type = CryptoArgs(this->state).cryptoString(loc, ""),
         *cipher = CryptoArgs(this->state).cryptoString(loc, "");
    Expr *passphrase = CryptoArgs(this->state).cryptoUndefined(loc);
    if (encoding) {
      llvm::StringMap<Expr *> e;
      llvm::SmallVector<llvm::StringRef, 4> allowed = {"type", "format"};
      if (k == 1) {
        allowed.append({"cipher", "passphrase"});
      }
      if (!CryptoArgs(this->state).cryptoOptions(encoding, option, allowed, e)) {
        return call;
      }
      if (!e.lookup("format")) {
        std::string message = ErrorTextKeyPair::noFormat(option);
        this->state.getDiagnostics().error(encoding->loc(), message);
        return call;
      }
      std::string formatLabel = ErrorTextKeyPairLabel::format();
      format =
          CryptoArgs(this->state).cryptoChoice(e["format"], formatLabel, {"pem", "der", "jwk"});
      if (format < 0) {
        return call;
      }
      if (Expr *t = e.lookup("type")) {
        std::string typeLabel = ErrorTextKeyPairLabel::keyType();
        int typeIndex =
            CryptoArgs(this->state).cryptoChoice(t, typeLabel, {"pkcs1", "spki", "pkcs8", "sec1"});
        if (typeIndex < 0) {
          return call;
        }
        static const char *const names[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = CryptoArgs(this->state).cryptoString(t->loc(), names[typeIndex]);
        flags |= 1;
      } else if (format != 2) {
        std::string message = ErrorTextKeyPair::noType(option, k == 0);
        this->state.getDiagnostics().error(encoding->loc(), message);
        return call;
      }
      if (Expr *c = e.lookup("cipher")) {
        cipher = ArgumentChecker(this->state).checkArg(c, Type::string());
        flags |= 2;
        if (!e.lookup("passphrase")) {
          std::string message = ErrorTextKeyPair::passphrase();
          this->state.getDiagnostics().error(encoding->loc(), message);
          return call;
        }
      }
      if (Expr *p = e.lookup("passphrase")) {
        passphrase = CryptoArgs(this->state).cryptoData(p, "passphrase", true, false, true);
        flags |= 4;
        if (passphrase->type->isError()) {
          return call;
        }
      }
    }
    fields[k] = format < 0    ? Type::cryptoType(Crypto::KeyObject)
                : format == 0 ? Type::string()
                : format == 1 ? Type::bytesType(Type::Bytes::Buffer)
                              : Type::json();
    parts.insert(parts.end(),
                 {CryptoArgs(this->state).cryptoInt(loc, format < 0 ? 0xffffffff : format),
                  CryptoArgs(this->state).cryptoInt(loc, flags), type, cipher, passphrase});
  }
  call->args = std::move(parts);
  call->spread.clear();
  call->target = BuiltinCall{.builtin = Builtin::CryptoKeyPair};
  call->type = Type::result(
      Type::object({{"publicKey", fields[0], false}, {"privateKey", fields[1], false}}, "KeyPair"),
      CryptoErrorTypes(this->state).named("GenerateKeyPairError"));

  return call;
}

}
