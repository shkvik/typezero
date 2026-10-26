#include "compiler/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/crypto_args/crypto_args.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

struct SubtleIntrinsic {
  llvm::StringLiteral name;
  CryptoFn fn;
  llvm::StringLiteral params;
  char result;
};

const SubtleIntrinsic kSubtleIntrinsics[] = {
    {"__tz_subtle_digest", CryptoFn::SubtleDigest, "md", 'B'},
    {"__tz_subtle_hmac", CryptoFn::SubtleHmac, "mdd", 'B'},
    {"__tz_subtle_hmacVerify", CryptoFn::SubtleHmacVerify, "mddd", 'I'},
    {"__tz_subtle_pbkdf2", CryptoFn::SubtlePbkdf2, "mddff", 'B'},
    {"__tz_subtle_hkdf", CryptoFn::SubtleHkdf, "mdddf", 'U'},
    {"__tz_subtle_aes", CryptoFn::SubtleAes, "cdbddiid", 'U'},
    {"__tz_subtle_aesKw", CryptoFn::SubtleAesKw, "dbd", 'U'},
    {"__tz_subtle_sign", CryptoFn::SubtleSign, "oisid", 'U'},
    {"__tz_subtle_verify", CryptoFn::SubtleVerify, "oisidd", 'I'},
    {"__tz_subtle_rsaOaep", CryptoFn::SubtleRsaOaep, "osdbd", 'U'},
    {"__tz_subtle_derive", CryptoFn::SubtleDerive, "oo", 'U'},
    {"__tz_subtle_generate", CryptoFn::SubtleGenerate, "iffs", 'K'},
};

}

WebCryptoChecker::WebCryptoChecker(Checker &checker) : checker_(checker) {}

Expr *WebCryptoChecker::checkSubtleIntrinsic(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  const SubtleIntrinsic *intrinsic = llvm::find_if(
      kSubtleIntrinsics, [&](const SubtleIntrinsic &i) { return i.name == name->name; });
  if (intrinsic == std::end(kSubtleIntrinsics) || call->args.size() != intrinsic->params.size() ||
      call->hasSpread()) {
    checker_.diags_.error(name->loc(),
                          "'" + name->name + "' is no intrinsic of tz, or is called wrongly");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  std::vector<Expr *> args;
  bool ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    Expr *checked = nullptr;
    switch (intrinsic->params[i]) {
    case 'm':
      checked = checker_.cryptoArgs().cryptoMd(arg, true);
      break;
    case 'c':
      checked = checker_.cryptoArgs().cryptoCipher(arg);
      break;
    case 'd':
      checked = checker_.cryptoArgs().cryptoData(arg, "data", false, true, true);
      break;
    case 'o':
      checked = checker_.cryptoArgs().cryptoKeyObject(arg, "key");
      break;
    case 'b':
      checked = checker_.argumentChecker().checkArg(arg, Type::boolTy());
      break;
    case 'i':
      checked = checker_.argumentChecker().checkArg(arg, Type::intTy());
      break;
    case 'f':
      checked = checker_.argumentChecker().checkArg(arg, Type::doubleTy());
      break;
    default:
      checked = checker_.argumentChecker().checkArg(arg, Type::string());
      break;
    }
    char kind = intrinsic->params[i];
    if (checked && (kind == 'm' || kind == 'c') && !isa<IntLitExpr>(checked)) {
      checker_.diags_.error(arg->loc(), "the hash or the cipher of '" + name->name +
                                            "' is a literal of the direct layer");
      checked = nullptr;
    }
    if (!checked || checked->type->isError()) {
      ok = false;
      checked = checked ? checked : arg;
    }
    args.push_back(checked);
  }
  if (!ok) {
    return call;
  }
  const Type *buffer = Type::bytesType(Type::Bytes::ArrayBuffer);
  const Type *value = intrinsic->result == 'B'   ? buffer
                      : intrinsic->result == 'U' ? Type::optional(buffer)
                      : intrinsic->result == 'I'
                          ? Type::intTy()
                          : Type::optional(Type::cryptoType(Type::Crypto::KeyObject));

  return checker_.cryptoArgs().cryptoCall(call, intrinsic->fn, std::move(args),
                                          Type::promise(value));
}

bool WebCryptoChecker::isSubtleClass(const Type *type, llvm::StringRef name) const {
  const ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;

  return cls && cls->library && cls->name == name && cls == checker_.libraryClass(name);
}

Expr *WebCryptoChecker::subtleObject(MemberExpr *member) {
  if (!checker_.scopes().lookup(kSubtleName).var) {
    checker_.diags_.error(member->loc(), "'crypto.subtle' is not available here");
    member->type = Type::error();
    return member;
  }
  auto *name = checker_.ctx_.make<NameExpr>(member->loc(), kSubtleName);

  return checker_.exprChecker().checkExpr(name);
}

}
