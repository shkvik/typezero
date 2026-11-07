#include "compiler/pipeline/check_program/topics/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/load/library_modules/library_modules.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_args/crypto_args.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

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
    {"subtleDigest", CryptoFn::SubtleDigest, "md", 'B'},
    {"subtleHmac", CryptoFn::SubtleHmac, "mdd", 'B'},
    {"subtleHmacVerify", CryptoFn::SubtleHmacVerify, "mddd", 'I'},
    {"subtlePbkdf2", CryptoFn::SubtlePbkdf2, "mddff", 'B'},
    {"subtleHkdf", CryptoFn::SubtleHkdf, "mdddf", 'U'},
    {"subtleAes", CryptoFn::SubtleAes, "cdbddiid", 'U'},
    {"subtleAesKw", CryptoFn::SubtleAesKw, "dbd", 'U'},
    {"subtleSign", CryptoFn::SubtleSign, "oisid", 'U'},
    {"subtleVerify", CryptoFn::SubtleVerify, "oisidd", 'I'},
    {"subtleRsaOaep", CryptoFn::SubtleRsaOaep, "osdbd", 'U'},
    {"subtleDerive", CryptoFn::SubtleDerive, "oo", 'U'},
    {"subtleGenerate", CryptoFn::SubtleGenerate, "iffs", 'K'},
};

}

WebCryptoChecker::WebCryptoChecker(CheckState &owner)
    : state(owner) {}

Expr *WebCryptoChecker::checkSubtleIntrinsic(CallExpr *call, NameExpr *name,
                                             llvm::StringRef intrinsicName) {
  call->type = Type::error();
  const SubtleIntrinsic *intrinsic = llvm::find_if(
      kSubtleIntrinsics, [&](const SubtleIntrinsic &i) { return i.name == intrinsicName; });
  if (intrinsic == std::end(kSubtleIntrinsics) || call->args.size() != intrinsic->params.size() ||
      call->hasSpread()) {
    this->state.getDiagnostics().error(
        name->loc(), "'" + intrinsicName + "' is no intrinsic of tz, or is called wrongly");
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
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
      checked = CryptoArgs(this->state).cryptoMd(arg, true);
      break;
    case 'c':
      checked = CryptoArgs(this->state).cryptoCipher(arg);
      break;
    case 'd':
      checked = CryptoArgs(this->state).cryptoData(arg, "data", false, true, true);
      break;
    case 'o':
      checked = CryptoArgs(this->state).cryptoKeyObject(arg, "key");
      break;
    case 'b':
      checked = ArgumentChecker(this->state).checkArg(arg, Type::boolTy());
      break;
    case 'i':
      checked = ArgumentChecker(this->state).checkArg(arg, Type::intTy());
      break;
    case 'f':
      checked = ArgumentChecker(this->state).checkArg(arg, Type::doubleTy());
      break;
    default:
      checked = ArgumentChecker(this->state).checkArg(arg, Type::string());
      break;
    }
    char kind = intrinsic->params[i];
    if (checked && (kind == 'm' || kind == 'c') && !isa<IntLitExpr>(checked)) {
      this->state.getDiagnostics().error(arg->loc(), "the hash or the cipher of '" + intrinsicName +
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

  return CryptoArgs(this->state)
      .cryptoCall(call, intrinsic->fn, std::move(args), Type::promise(value));
}

bool WebCryptoChecker::isSubtleClass(const Type *type, llvm::StringRef name) const {
  const ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;

  return cls && cls->library() && cls->name == name &&
         cls == this->state.getLibraryDeclarations().findLibraryClass(name);
}

Expr *WebCryptoChecker::subtleObject(MemberExpr *member) {
  if (!Scopes(this->state).lookup(kSubtleName).var()) {
    this->state.getDiagnostics().error(member->loc(), "'crypto.subtle' is not available here");
    member->type = Type::error();
    return member;
  }
  auto *name = this->state.getAst().make<NameExpr>(member->loc(), kSubtleName);

  return ExprChecker(this->state).checkExpr(name);
}

}
