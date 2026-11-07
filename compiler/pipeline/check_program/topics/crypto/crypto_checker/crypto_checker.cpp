#include "compiler/pipeline/check_program/topics/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/library/crypto/crypto_names/crypto_names.h"
#include "compiler/pipeline/check_program/topics/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_args/crypto_args.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;

namespace tz {

CryptoChecker::CryptoChecker(CheckState &owner)
    : state(owner) {}

Expr *CryptoChecker::checkCryptoValue(llvm::StringRef name, Expr *expr) {
  if (name == "constants") {
    std::string message = ErrorTextCryptoValue::constants();
    this->state.getDiagnostics().error(expr->loc(), message);
    return expr;
  }
  if (this->cryptoTypeNamed(name)) {
    std::string message = ErrorTextCryptoValue::isType(name);
    this->state.getDiagnostics().error(expr->loc(), message);
    return expr;
  }

  return nullptr;
}

Expr *CryptoChecker::checkCryptoModuleMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *n = object ? BuiltinModuleChecker(this->state).nodeName(object->name) : nullptr;
  if (!n || n->module != NodeModule::Crypto || n->name != "constants") {
    return nullptr;
  }

  return this->cryptoConstant(member);
}

const Type *CryptoChecker::cryptoTypeNamed(llvm::StringRef name) const {
  const NodeName *n = BuiltinModuleChecker(this->state).nodeName(name);
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
      Expr *lit = this->state.getAst().make<IntLitExpr>(member->loc(),
                                                        uint64_t(value < 0 ? -value : value));
      lit->type = Type::intTy();
      if (value >= 0) {
        return lit;
      }
      auto *negated = this->state.getAst().make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
      return ExprChecker(this->state).checkExpr(negated);
    }
  }
  std::string message = ErrorTextCryptoValue::constant(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
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
    auto *call = this->state.getAst().make<CallExpr>(loc, member, std::vector<Expr *>{});
    return CryptoArgs(this->state).cryptoCall(call, fn, std::move(args), result);
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
      std::string message = ErrorTextCryptoMember::keyDetails();
      this->state.getDiagnostics().error(loc, message);
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
        return read(p.fn, result, {CryptoArgs(this->state).cryptoInt(loc, p.bits)});
      }
      return read(p.fn, result);
    }
    if (name == "issuerCertificate") {
      std::string message = ErrorTextCryptoMember::issuerCert();
      this->state.getDiagnostics().error(loc, message);
      member->type = Type::error();
      return member;
    }
  }
  std::string message =
      ErrorTextCryptoMember::onlyCalled(name, type->name(), CryptoNames::methodsOf(kind));
  this->state.getDiagnostics().error(loc, message);
  member->type = Type::error();

  return member;
}

Expr *CryptoChecker::checkCryptoNew(NewExpr *expr, llvm::StringRef cls) {
  expr->type = Type::error();
  if (cls != "X509Certificate") {
    std::string message = ErrorTextCryptoMake::notNode(cls);
    this->state.getDiagnostics().error(expr->loc(), message);
    for (Expr *&arg : expr->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return expr;
  }
  std::string message = ErrorTextCryptoMake::x509();
  this->state.getDiagnostics().error(expr->loc(), message);
  for (Expr *&arg : expr->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }

  return expr;
}

Expr *CryptoChecker::checkX509Parse(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "parse") {
    std::string message = ErrorTextX509::noStatic(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  if (call->args.size() != 1) {
    std::string message = ErrorTextX509::parseArgs();
    this->state.getDiagnostics().error(call->loc(), message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  std::string what = ErrorTextX509::data();
  Expr *data = CryptoArgs(this->state).cryptoData(call->args[0], what, true, false, true);
  if (data->type->isError()) {
    return call;
  }

  return CryptoArgs(this->state)
      .cryptoResult(call, CryptoFn::X509New, {data}, Type::cryptoType(Crypto::X509));
}

bool CryptoChecker::isX509Class(Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  const NodeName *n = name ? BuiltinModuleChecker(this->state).nodeName(name->name) : nullptr;

  return n && n->module == NodeModule::Crypto && n->name == "X509Certificate";
}

Expr *CryptoChecker::checkWebCryptoCall(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "randomUUID" || name == "getRandomValues") {
    return CryptoFunctionChecker(this->state).checkCryptoCall(call, name, member->loc());
  }
  call->type = Type::error();
  for (Expr *&arg : call->args) {
    arg = ExprChecker(this->state).checkExpr(arg);
  }
  std::string message = name == "subtle" ? ErrorTextGlobalCrypto::subtleCall()
                                         : ErrorTextGlobalCrypto::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

Expr *CryptoChecker::checkWebCryptoMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->name == "subtle") {
    return WebCryptoChecker(this->state).subtleObject(member);
  }
  std::string message = member->name == "randomUUID" || member->name == "getRandomValues"
                            ? ErrorTextGlobalCrypto::onlyCalled(member->name)
                            : ErrorTextGlobalCrypto::noMember(member->name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

}
