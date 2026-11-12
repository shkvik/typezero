#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/crypto_property_table/crypto_property_table.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/semantic/library/crypto/crypto_check_helpers/crypto_check_helpers.h"
#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/semantic/library/crypto/crypto_names/crypto_names.h"
#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;

namespace tz {

CryptoChecker::CryptoChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->cryptoArgs = &all.cryptoArgs;
  this->exprChecker = &all.exprChecker;
  this->webCryptoChecker = &all.webCryptoChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
  this->cryptoFunctionChecker = &all.cryptoFunctionChecker;
}

Expr *CryptoChecker::checkCryptoValue(llvm::StringRef name, Expr *expr) {
  if (name == "constants") {
    auto message = ErrorTextCryptoValue::constants();
    this->state.getDiagnostics().error(expr->loc(), message);
    return expr;
  }
  if (this->cryptoTypeNamed(name) != nullptr) {
    auto message = ErrorTextCryptoValue::isType(name);
    this->state.getDiagnostics().error(expr->loc(), message);
    return expr;
  }

  return nullptr;
}

Expr *CryptoChecker::checkCryptoModuleMember(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *nodeName =
      object != nullptr ? this->builtinModuleChecker->nodeName(object->name) : nullptr;
  if (nodeName == nullptr || nodeName->module != NodeModule::Crypto ||
      nodeName->name != "constants") {
    return nullptr;
  }

  return this->cryptoConstant(member);
}

const Type *CryptoChecker::cryptoTypeNamed(llvm::StringRef name) const {
  const auto *nodeName = this->builtinModuleChecker->nodeName(name);
  if (nodeName == nullptr || nodeName->module != NodeModule::Crypto) {
    return nullptr;
  }
  static const std::pair<llvm::StringLiteral, Crypto> classes[] = {
      {"Hash", Crypto::Hash},           {"Hmac", Crypto::Hmac},
      {"Cipheriv", Crypto::Cipheriv},   {"Decipheriv", Crypto::Decipheriv},
      {"KeyObject", Crypto::KeyObject}, {"Sign", Crypto::Sign},
      {"Verify", Crypto::Verify},       {"ECDH", Crypto::Ecdh},
      {"X509Certificate", Crypto::X509}};
  for (const auto &[className, kind] : classes) {
    if (className == nodeName->name) {
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
      Expr *literal = this->state.getAst().make<IntLitExpr>(member->loc(),
                                                            uint64_t(value < 0 ? -value : value));
      literal->type = Type::intTy();
      if (value >= 0) {
        return literal;
      }
      auto *negated = this->state.getAst().make<UnaryExpr>(member->loc(), UnaryOp::Neg, literal);
      return this->exprChecker->checkExpr(negated);
    }
  }
  auto message = ErrorTextCryptoValue::constant(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *CryptoChecker::checkCryptoMember(MemberExpr *member) {
  const auto *found = CryptoPropertyTable::find(member->object->type->crypto(), member->name);
  if (found == nullptr) {
    return this->reportOnlyCalled(member);
  }
  switch (found->property) {
    case CryptoProperty::KEY_TYPE:
      return this->readCryptoProperty(member, CryptoFn::KeyType, Type::string());
    case CryptoProperty::KEY_ASYMMETRIC_KEY_TYPE:
      return this->readCryptoProperty(member, CryptoFn::KeyAsymmetricType,
                                      Type::optional(Type::string()));
    case CryptoProperty::KEY_SYMMETRIC_KEY_SIZE:
      return this->readCryptoProperty(member, CryptoFn::KeySymmetricSize,
                                      Type::optional(Type::intTy()));
    case CryptoProperty::KEY_ASYMMETRIC_KEY_DETAILS:
      return this->reportCryptoMember(member, ErrorTextCryptoMember::keyDetails());
    case CryptoProperty::X509_SUBJECT:
      return this->readCryptoProperty(member, CryptoFn::X509Subject, Type::string());
    case CryptoProperty::X509_ISSUER:
      return this->readCryptoProperty(member, CryptoFn::X509Issuer, Type::string());
    case CryptoProperty::X509_VALID_FROM:
      return this->readCryptoProperty(member, CryptoFn::X509ValidFrom, Type::string());
    case CryptoProperty::X509_VALID_TO:
      return this->readCryptoProperty(member, CryptoFn::X509ValidTo, Type::string());
    case CryptoProperty::X509_VALID_FROM_DATE:
      return this->readCryptoProperty(member, CryptoFn::X509ValidFromDate, Type::date());
    case CryptoProperty::X509_VALID_TO_DATE:
      return this->readCryptoProperty(member, CryptoFn::X509ValidToDate, Type::date());
    case CryptoProperty::X509_SERIAL_NUMBER:
      return this->readCryptoProperty(member, CryptoFn::X509SerialNumber, Type::string());
    case CryptoProperty::X509_FINGERPRINT:
      return this->readFingerprint(member, 1);
    case CryptoProperty::X509_FINGERPRINT256:
      return this->readFingerprint(member, 256);
    case CryptoProperty::X509_FINGERPRINT512:
      return this->readFingerprint(member, 512);
    case CryptoProperty::X509_SUBJECT_ALT_NAME:
      return this->readCryptoProperty(member, CryptoFn::X509SubjectAltName,
                                      Type::optional(Type::string()));
    case CryptoProperty::X509_INFO_ACCESS:
      return this->readCryptoProperty(member, CryptoFn::X509InfoAccess,
                                      Type::optional(Type::string()));
    case CryptoProperty::X509_KEY_USAGE:
      return this->readCryptoProperty(member, CryptoFn::X509KeyUsage,
                                      Type::optional(Type::array(Type::string())));
    case CryptoProperty::X509_CA:
      return this->readCryptoProperty(member, CryptoFn::X509Ca, Type::boolTy());
    case CryptoProperty::X509_RAW:
      return this->readCryptoProperty(member, CryptoFn::X509Raw,
                                      Type::bytesType(Type::Bytes::Buffer));
    case CryptoProperty::X509_PUBLIC_KEY:
      return this->readCryptoProperty(member, CryptoFn::X509PublicKey,
                                      Type::cryptoType(Type::Crypto::KeyObject));
    case CryptoProperty::X509_ISSUER_CERTIFICATE:
      return this->reportCryptoMember(member, ErrorTextCryptoMember::issuerCert());
  }

  return this->reportOnlyCalled(member);
}

Expr *CryptoChecker::readCryptoProperty(MemberExpr *member, CryptoFn fn, const Type *result) {
  std::vector<Expr *> args{member->object};
  auto *call = this->state.getAst().make<CallExpr>(member->loc(), member, std::vector<Expr *>{});

  return this->cryptoArgs->cryptoCall(call, fn, std::move(args), result);
}

Expr *CryptoChecker::readFingerprint(MemberExpr *member, int bits) {
  auto *size = this->cryptoArgs->cryptoInt(member->loc(), bits);
  std::vector<Expr *> args{member->object, size};
  auto *call = this->state.getAst().make<CallExpr>(member->loc(), member, std::vector<Expr *>{});

  return this->cryptoArgs->cryptoCall(call, CryptoFn::X509Fingerprint, std::move(args),
                                      Type::string());
}

Expr *CryptoChecker::reportCryptoMember(MemberExpr *member, const std::string &message) {
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *CryptoChecker::reportOnlyCalled(MemberExpr *member) {
  const Type *type = member->object->type;
  Type::Crypto kind = type->crypto();
  auto message =
      ErrorTextCryptoMember::onlyCalled(member->name, type->name(), CryptoNames::methodsOf(kind));

  return this->reportCryptoMember(member, message);
}

Expr *CryptoChecker::checkCryptoNew(NewExpr *expr, llvm::StringRef className) {
  expr->type = Type::error();
  if (className != "X509Certificate") {
    auto message = ErrorTextCryptoMake::notNode(className);
    this->state.getDiagnostics().error(expr->loc(), message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  auto message = ErrorTextCryptoMake::x509();
  this->state.getDiagnostics().error(expr->loc(), message);
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return expr;
}

Expr *CryptoChecker::checkX509Parse(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "parse") {
    auto message = ErrorTextX509::noStatic(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  if (call->args.size() != 1) {
    auto message = ErrorTextX509::parseArgs();
    this->state.getDiagnostics().error(call->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  auto what = ErrorTextX509::data();
  auto *data = this->cryptoArgs->cryptoData(call->args[0], what, true, false, true);
  if (data->type->isError()) {
    return call;
  }

  return this->cryptoArgs->cryptoResult(call, CryptoFn::X509New, {data},
                                        Type::cryptoType(Crypto::X509));
}

bool CryptoChecker::isX509Class(Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  const NodeName *nodeName =
      name != nullptr ? this->builtinModuleChecker->nodeName(name->name) : nullptr;

  return nodeName != nullptr && nodeName->module == NodeModule::Crypto &&
         nodeName->name == "X509Certificate";
}

Expr *CryptoChecker::checkWebCryptoCall(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  if (name == "randomUUID" || name == "getRandomValues") {
    return this->cryptoFunctionChecker->checkCryptoCall(call, name, member->loc());
  }
  call->type = Type::error();
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  std::string message = name == "subtle" ? ErrorTextGlobalCrypto::subtleCall()
                                         : ErrorTextGlobalCrypto::noMember(name);
  this->state.getDiagnostics().error(member->loc(), message);

  return call;
}

Expr *CryptoChecker::checkWebCryptoMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->name == "subtle") {
    return this->webCryptoChecker->subtleObject(member);
  }
  std::string message = member->name == "randomUUID" || member->name == "getRandomValues"
                            ? ErrorTextGlobalCrypto::onlyCalled(member->name)
                            : ErrorTextGlobalCrypto::noMember(member->name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

}
