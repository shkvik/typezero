#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/crypto/crypto_args/crypto_args.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker_errors.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::isa;

namespace tz {

struct SubtleIntrinsic {
  llvm::StringLiteral name;
  CryptoFn fn;
  llvm::StringLiteral params;
  char result;
};

namespace {

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

WebCryptoChecker::WebCryptoChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->cryptoArgs = &all.getCryptoArgs();
  this->exprChecker = &all.getExprChecker();
  this->argumentChecker = &all.getArgumentChecker();
}

WebCryptoChecker::ResultSubtleCallShape
WebCryptoChecker::subtleShape(const CallExpr *call, llvm::StringRef intrinsicName) {
  auto named = [&](const SubtleIntrinsic &i) { return i.name == intrinsicName; };
  const SubtleIntrinsic *intrinsic = llvm::find_if(kSubtleIntrinsics, named);
  bool wrong = intrinsic == std::end(kSubtleIntrinsics) ||
               call->args.size() != intrinsic->params.size() || call->hasSpread();
  if (wrong) {
    std::string message = ErrorTextIntrinsicCall::wrong(intrinsicName);

    return err(WebCryptoCheckerSubtleShapeError{
        .code = WebCryptoCheckerSubtleShapeErrorCode::NOT_AN_INTRINSIC,
        .message = message,
    });
  }

  return ok(intrinsic);
}

WebCryptoChecker::ResultAlgorithmChosen
WebCryptoChecker::literalAlgorithm(const Expr *checked, char kind, llvm::StringRef intrinsicName) {
  if (checked && (kind == 'm' || kind == 'c') && !isa<IntLitExpr>(checked)) {
    std::string message = ErrorTextSubtle::literal(intrinsicName);

    return err(WebCryptoCheckerLiteralAlgorithmError{
        .code = WebCryptoCheckerLiteralAlgorithmErrorCode::NOT_A_LITERAL,
        .message = message,
    });
  }

  return ok(checked);
}

WebCryptoChecker::ResultSubtleAvailable WebCryptoChecker::subtleAvailable() const {
  VarSymbol *subtle = this->scopes->lookup(LibraryModuleNames::kSubtleName).var();
  if (!subtle) {
    std::string message = ErrorTextSubtle::noSubtle();

    return err(WebCryptoCheckerSubtleAvailableError{
        .code = WebCryptoCheckerSubtleAvailableErrorCode::NO_SUBTLE,
        .message = message,
    });
  }

  return ok(subtle);
}

Expr *WebCryptoChecker::checkSubtleIntrinsic(CallExpr *call, NameExpr *name,
                                             llvm::StringRef intrinsicName) {
  call->type = Type::error();
  ResultSubtleCallShape shape = WebCryptoChecker::subtleShape(call, intrinsicName);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(name->loc(), shape.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  const SubtleIntrinsic *intrinsic = shape.value();
  std::vector<Expr *> args;
  bool ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *arg = call->args[i];
    Expr *checked = nullptr;
    switch (intrinsic->params[i]) {
    case 'm':
      checked = this->cryptoArgs->cryptoMd(arg, true);
      break;
    case 'c':
      checked = this->cryptoArgs->cryptoCipher(arg);
      break;
    case 'd':
      checked = this->cryptoArgs->cryptoData(arg, "data", false, true, true);
      break;
    case 'o':
      checked = this->cryptoArgs->cryptoKeyObject(arg, "key");
      break;
    case 'b':
      checked = this->argumentChecker->checkArg(arg, Type::boolTy());
      break;
    case 'i':
      checked = this->argumentChecker->checkArg(arg, Type::intTy());
      break;
    case 'f':
      checked = this->argumentChecker->checkArg(arg, Type::doubleTy());
      break;
    default:
      checked = this->argumentChecker->checkArg(arg, Type::string());
      break;
    }
    char kind = intrinsic->params[i];
    ResultAlgorithmChosen literal =
        WebCryptoChecker::literalAlgorithm(checked, kind, intrinsicName);
    if (literal.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), literal.error().message);
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

  return this->cryptoArgs->cryptoCall(call, intrinsic->fn, std::move(args), Type::promise(value));
}

bool WebCryptoChecker::isSubtleClass(const Type *type, llvm::StringRef name) const {
  const ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;

  return cls && cls->library() && cls->name == name &&
         cls == this->state.getLibraryDeclarations().findLibraryClass(name);
}

Expr *WebCryptoChecker::subtleObject(MemberExpr *member) {
  ResultSubtleAvailable subtle = this->subtleAvailable();
  if (subtle.isErr()) {
    this->state.getDiagnostics().error(member->loc(), subtle.error().message);
    member->type = Type::error();
    return member;
  }
  auto *name = this->state.getAst().make<NameExpr>(member->loc(), LibraryModuleNames::kSubtleName);

  return this->exprChecker->checkExpr(name);
}

}
