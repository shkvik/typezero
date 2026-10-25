#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "compiler/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
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

CryptoChecker::CryptoChecker(Sema &sema) : sema_(sema) {}

const Type *CryptoChecker::cryptoTypeNamed(llvm::StringRef name) const {
  const Sema::NodeName *n = sema_.builtinModuleChecker().nodeName(name);
  if (!n || n->module != NodeModule::Crypto) {
    return nullptr;
  }
  static const std::pair<llvm::StringLiteral, Crypto> classes[] = {
      {"Hash", Crypto::Hash},           {"Hmac", Crypto::Hmac},
      {"Cipheriv", Crypto::Cipheriv},   {"Decipheriv", Crypto::Decipheriv},
      {"KeyObject", Crypto::KeyObject}, {"Sign", Crypto::Sign},
      {"Verify", Crypto::Verify},       {"ECDH", Crypto::Ecdh},
      {"X509Certificate", Crypto::X509}};
  for (const auto &[className, kind] : classes) {
    if (className == n->name) {
      return Type::cryptoType(kind);
    }
  }

  return nullptr;
}

// constants.RSA_PKCS1_PSS_PADDING and the others of node:crypto, as node has them.
Expr *CryptoChecker::cryptoConstant(MemberExpr *member) {
  static const std::pair<llvm::StringLiteral, int> constants[] = {
      {"RSA_PKCS1_PADDING", 1},           {"RSA_NO_PADDING", 3},
      {"RSA_PKCS1_OAEP_PADDING", 4},      {"RSA_X931_PADDING", 5},
      {"RSA_PKCS1_PSS_PADDING", 6},       {"RSA_PSS_SALTLEN_DIGEST", -1},
      {"RSA_PSS_SALTLEN_MAX_SIGN", -2},   {"RSA_PSS_SALTLEN_AUTO", -2},
      {"POINT_CONVERSION_COMPRESSED", 2}, {"POINT_CONVERSION_UNCOMPRESSED", 4},
      {"POINT_CONVERSION_HYBRID", 6}};
  for (const auto &[name, value] : constants) {
    if (name == member->name) {
      Expr *lit = sema_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(value < 0 ? -value : value));
      lit->type = Type::intTy();
      if (value >= 0) {
        return lit;
      }
      auto *negated = sema_.ctx_.make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
      return sema_.exprChecker().checkExpr(negated);
    }
  }
  sema_.diags_.error(member->loc(), "'constants." + member->name +
                                  "' of 'node:crypto' is not supported yet; tz has the RSA_ and "
                                  "POINT_CONVERSION_ ones");
  member->type = Type::error();

  return member;
}

Expr *CryptoChecker::checkCryptoMember(MemberExpr *member) {
  const Type *type = member->object->type;
  Crypto kind = type->crypto();
  llvm::StringRef name = member->name;
  llvm::SMLoc loc = member->loc();
  auto read = [&](CryptoFn fn, const Type *result, std::vector<Expr *> args = {}) {
    args.insert(args.begin(), member->object);
    auto *call = sema_.ctx_.make<CallExpr>(loc, member, std::vector<Expr *>{});
    return sema_.cryptoArgs().cryptoCall(call, fn, std::move(args), result);
  };
  if (kind == Crypto::KeyObject) {
    if (name == "type") {
      return read(CryptoFn::KeyType, Type::string());
    }
    if (name == "asymmetricKeyType") {
      return read(CryptoFn::KeyAsymmetricType, Type::optional(Type::string()));
    }
    if (name == "symmetricKeySize") {
      return read(CryptoFn::KeySymmetricSize, Type::optional(Type::intTy()));
    }
    if (name == "asymmetricKeyDetails") {
      sema_.diags_.error(loc, "'asymmetricKeyDetails' is not supported yet: node gives the exponent of "
                        "RSA as a bigint, which tz has not");
      member->type = Type::error();
      return member;
    }
  }
  if (kind == Crypto::X509) {
    struct Property {
      llvm::StringLiteral name;
      CryptoFn fn;
      int bits;
    };
    static const Property properties[] = {
        {"subject", CryptoFn::X509Subject, 0},
        {"issuer", CryptoFn::X509Issuer, 0},
        {"validFrom", CryptoFn::X509ValidFrom, 0},
        {"validTo", CryptoFn::X509ValidTo, 0},
        {"validFromDate", CryptoFn::X509ValidFromDate, 0},
        {"validToDate", CryptoFn::X509ValidToDate, 0},
        {"serialNumber", CryptoFn::X509SerialNumber, 0},
        {"fingerprint", CryptoFn::X509Fingerprint, 1},
        {"fingerprint256", CryptoFn::X509Fingerprint, 256},
        {"fingerprint512", CryptoFn::X509Fingerprint, 512},
        {"subjectAltName", CryptoFn::X509SubjectAltName, 0},
        {"infoAccess", CryptoFn::X509InfoAccess, 0},
        {"keyUsage", CryptoFn::X509KeyUsage, 0},
        {"ca", CryptoFn::X509Ca, 0},
        {"raw", CryptoFn::X509Raw, 0},
        {"publicKey", CryptoFn::X509PublicKey, 0},
    };
    for (const Property &p : properties) {
      if (p.name != name) {
        continue;
      }
      const Type *result =
          p.fn == CryptoFn::X509ValidFromDate || p.fn == CryptoFn::X509ValidToDate ? Type::date()
          : p.fn == CryptoFn::X509SubjectAltName || p.fn == CryptoFn::X509InfoAccess
              ? Type::optional(Type::string())
          : p.fn == CryptoFn::X509KeyUsage  ? Type::optional(Type::array(Type::string()))
          : p.fn == CryptoFn::X509Ca        ? Type::boolTy()
          : p.fn == CryptoFn::X509Raw       ? Type::bytesType(Type::Bytes::Buffer)
          : p.fn == CryptoFn::X509PublicKey ? Type::cryptoType(Crypto::KeyObject)
                                            : Type::string();
      if (p.bits) {
        return read(p.fn, result, {sema_.cryptoArgs().cryptoInt(loc, p.bits)});
      }
      return read(p.fn, result);
    }
    if (name == "issuerCertificate") {
      sema_.diags_.error(loc, "'issuerCertificate' comes only with TLS (docs/net.md)");
      member->type = Type::error();
      return member;
    }
  }
  sema_.diags_.error(loc, "property '" + name + "' of " + type->name() + " can only be called; it has " +
                        methodsOf(kind));
  member->type = Type::error();

  return member;
}

// new X509Certificate(buffer), the one class of node:crypto made with new.
Expr *CryptoChecker::checkCryptoNew(NewExpr *expr, llvm::StringRef cls) {
  expr->type = Type::error();
  if (cls != "X509Certificate") {
    sema_.diags_.error(expr->loc(), "'new " + cls + "' is not how node makes one: " +
                                  (cls == "KeyObject" ? "use 'createSecretKey', 'createPublicKey' "
                                                        "or 'createPrivateKey'"
                                   : cls == "ECDH"    ? "use 'createECDH(curve)'"
                                   : cls == "Sign" || cls == "Verify"
                                       ? "use 'create" + cls.str() + "(algorithm)'"
                                   : cls == "Hash" || cls == "Hmac"
                                       ? "use 'create" + cls.str() + "(algorithm)'"
                                       : "use 'create" + cls.str() + "(algorithm, key, iv)'"));
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  if (expr->args.size() != 1) {
    sema_.diags_.error(expr->loc(), "'new X509Certificate' takes the certificate, PEM or DER");
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  Expr *data = sema_.cryptoArgs().cryptoData(expr->args[0], "certificate", true, false, true);
  if (data->type->isError()) {
    return expr;
  }
  auto *callee = sema_.ctx_.make<NameExpr>(expr->loc(), "X509Certificate");
  auto *call = sema_.ctx_.make<CallExpr>(expr->loc(), callee, std::vector<Expr *>{});

  return sema_.cryptoArgs().cryptoCall(call, CryptoFn::X509New, {data}, Type::cryptoType(Crypto::X509));
}

Expr *CryptoChecker::checkWebCryptoCall(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "randomUUID" || name == "getRandomValues") {
    return sema_.cryptoFunctionChecker().checkCryptoCall(call, name, member->loc());
  }
  call->type = Type::error();
  for (Expr *&arg : call->args) {
    arg = sema_.exprChecker().checkExpr(arg);
  }
  if (name == "subtle") {
    sema_.diags_.error(member->loc(), "'crypto.subtle' is an object, not a function");
  } else {
    sema_.diags_.error(member->loc(), "'crypto." + name +
                                    "' does not exist; the global crypto has "
                                    "'randomUUID', 'getRandomValues' and "
                                    "'subtle'");
  }

  return call;
}

Expr *CryptoChecker::checkWebCryptoMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->name == "subtle") {
    return sema_.webCryptoChecker().subtleObject(member);
  }
  if (member->name == "randomUUID" || member->name == "getRandomValues") {
    sema_.diags_.error(member->loc(), "'crypto." + member->name + "' can only be called");
  } else {
    sema_.diags_.error(member->loc(), "'crypto." + member->name +
                                    "' does not exist; the global crypto "
                                    "has 'randomUUID', 'getRandomValues' "
                                    "and 'subtle'");
  }

  return member;
}

} // namespace tz
