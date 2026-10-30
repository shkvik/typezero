#include "compiler/library/crypto/key_pair_checker/key_pair_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_error_types/crypto_error_types.h"
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

}

KeyPairChecker::KeyPairChecker(Checker &checker) : checker_(checker) {}

Expr *KeyPairChecker::checkGenerateKeyPair(CallExpr *call, llvm::SMLoc loc) {
  std::vector<Expr *> &args = call->args;
  if (!checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  static const llvm::StringRef types[] = {"rsa",     "rsa-pss", "dsa",    "ec",
                                          "ed25519", "ed448",   "x25519", "x448"};
  int which = -1;
  Expr *name = checker_.cryptoArgs().cryptoString(loc, "");
  if (literalOf(args[0])) {
    if ((which = checker_.cryptoArgs().cryptoChoice(args[0], "type of the key pair", types)) < 0) {
      return call;
    }
  } else {
    name = checker_.argumentChecker().checkArg(args[0], Type::string());
  }
  llvm::StringMap<Expr *> options;
  if (args.size() > 1 && !checker_.cryptoArgs().cryptoOptions(
                             args[1], "generateKeyPairSync",
                             {"modulusLength", "publicExponent", "namedCurve", "publicKeyEncoding",
                              "privateKeyEncoding", "divisorLength", "hashAlgorithm",
                              "mgf1HashAlgorithm", "saltLength"},
                             options)) {
    return call;
  }
  for (llvm::StringRef unsupported : {"hashAlgorithm", "mgf1HashAlgorithm", "saltLength"}) {
    if (Expr *e = options.lookup(unsupported)) {
      checker_.diags_.error(e->loc(), "the option '" + unsupported +
                                          "' of 'rsa-pss' keys is not "
                                          "supported yet");
      return call;
    }
  }
  if (which >= 0 && which <= 2 && !options.lookup("modulusLength")) {
    checker_.diags_.error(loc, "'" + types[which] + "' keys need '{ modulusLength: 2048 }'");
    return call;
  }
  if (which == 3 && !options.lookup("namedCurve")) {
    checker_.diags_.error(loc, "'ec' keys need '{ namedCurve: \"P-256\" }'");
    return call;
  }
  int has = 0;
  Expr *modulus = checker_.cryptoArgs().cryptoDouble(loc, 0),
       *exponent = checker_.cryptoArgs().cryptoDouble(loc, 65537);
  Expr *curve = checker_.cryptoArgs().cryptoString(loc, "");
  if (Expr *m = options.lookup("modulusLength")) {
    modulus = checker_.cryptoArgs().cryptoNumber(m);
    std::optional<double> bits = checker_.cryptoArgs().knownNumber(modulus);
    if ((which == 0 || which == 1) && bits && *bits < 512) {
      checker_.diags_.error(m->loc(), "an RSA key needs a modulusLength of 512 bits or more, not " +
                                          CryptoArgs::numberText(*bits));
      return call;
    }
  }
  if (Expr *e = options.lookup("publicExponent")) {
    exponent = checker_.cryptoArgs().cryptoNumber(e);
    has |= 2;
  }
  if (Expr *d = options.lookup("divisorLength")) {
    checker_.cryptoArgs().cryptoNumber(d);
  }
  if (Expr *c = options.lookup("namedCurve")) {
    curve = checker_.argumentChecker().checkArg(c, Type::string());
    has |= 1;
  }
  auto *generate = checker_.ctx_.make<CallExpr>(loc, call->callee, std::vector<Expr *>{});
  checker_.cryptoArgs().cryptoCall(
      generate, CryptoFn::GenerateKeyPair,
      {checker_.cryptoArgs().cryptoInt(loc, which < 0 ? 0xffffffff : which), name, modulus,
       exponent, checker_.cryptoArgs().cryptoInt(loc, has), curve},
      Type::cryptoType(Crypto::KeyObject));
  std::vector<Expr *> parts = {generate};
  const Type *fields[2];
  for (int k = 0; k < 2; ++k) {
    llvm::StringRef option = k == 0 ? "publicKeyEncoding" : "privateKeyEncoding";
    Expr *encoding = options.lookup(option);
    int format = -1, flags = 0;
    Expr *type = checker_.cryptoArgs().cryptoString(loc, ""),
         *cipher = checker_.cryptoArgs().cryptoString(loc, "");
    Expr *passphrase = checker_.cryptoArgs().cryptoUndefined(loc);
    if (encoding) {
      llvm::StringMap<Expr *> e;
      llvm::SmallVector<llvm::StringRef, 4> allowed = {"type", "format"};
      if (k == 1) {
        allowed.append({"cipher", "passphrase"});
      }
      if (!checker_.cryptoArgs().cryptoOptions(encoding, option, allowed, e)) {
        return call;
      }
      if (!e.lookup("format")) {
        checker_.diags_.error(encoding->loc(),
                              "'" + option + "' needs 'format': 'pem', 'der' or 'jwk'");
        return call;
      }
      if ((format = checker_.cryptoArgs().cryptoChoice(e["format"], "format of the key",
                                                       {"pem", "der", "jwk"})) < 0) {
        return call;
      }
      if (Expr *t = e.lookup("type")) {
        int typeIndex = checker_.cryptoArgs().cryptoChoice(t, "type of the key",
                                                           {"pkcs1", "spki", "pkcs8", "sec1"});
        if (typeIndex < 0) {
          return call;
        }
        static const char *const names[] = {"pkcs1", "spki", "pkcs8", "sec1"};
        type = checker_.cryptoArgs().cryptoString(t->loc(), names[typeIndex]);
        flags |= 1;
      } else if (format != 2) {
        checker_.diags_.error(encoding->loc(),
                              "'" + option + "' needs 'type': " +
                                  (k == 0 ? "'spki' or 'pkcs1'" : "'pkcs8', 'pkcs1' or 'sec1'"));
        return call;
      }
      if (Expr *c = e.lookup("cipher")) {
        cipher = checker_.argumentChecker().checkArg(c, Type::string());
        flags |= 2;
        if (!e.lookup("passphrase")) {
          checker_.diags_.error(encoding->loc(), "a cipher needs a 'passphrase'");
          return call;
        }
      }
      if (Expr *p = e.lookup("passphrase")) {
        passphrase = checker_.cryptoArgs().cryptoData(p, "passphrase", true, false, true);
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
                 {checker_.cryptoArgs().cryptoInt(loc, format < 0 ? 0xffffffff : format),
                  checker_.cryptoArgs().cryptoInt(loc, flags), type, cipher, passphrase});
  }
  call->args = std::move(parts);
  call->spread.clear();
  call->builtin = Builtin::CryptoKeyPair;
  call->type = Type::result(
      Type::object({{"publicKey", fields[0], false}, {"privateKey", fields[1], false}}, "KeyPair"),
      checker_.cryptoErrorTypes().named("GenerateKeyPairError"));

  return call;
}

}
