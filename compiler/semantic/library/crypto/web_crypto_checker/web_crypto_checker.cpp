#include "compiler/semantic/library/crypto/web_crypto_checker/web_crypto_checker.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/library/crypto/subtle_intrinsic_table/subtle_intrinsic_table.h"
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

WebCryptoChecker::WebCryptoChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->cryptoArgs = &all.cryptoArgs;
  this->exprChecker = &all.exprChecker;
  this->classDeclarer = &all.classDeclarer;
  this->argumentChecker = &all.argumentChecker;
}

WebCryptoChecker::ResultSubtleCallShape
WebCryptoChecker::checkSubtleShape(const CallExpr *call, llvm::StringRef intrinsicName) const {
  const SubtleIntrinsic *intrinsic = SubtleIntrinsicTable::find(intrinsicName);
  bool isWrong =
      intrinsic == nullptr || call->args.size() != intrinsic->params.size() || call->hasSpread();
  if (isWrong) {
    auto message = ErrorTextIntrinsicCall::wrong(intrinsicName);

    return err(WebCryptoCheckerSubtleShapeError{
        .code = WebCryptoCheckerSubtleShapeErrorCode::NOT_AN_INTRINSIC,
        .message = message,
    });
  }

  return ok(intrinsic);
}

WebCryptoChecker::ResultAlgorithmChosen
WebCryptoChecker::checkLiteralAlgorithm(const Expr *checked, char kind,
                                        llvm::StringRef intrinsicName) const {
  if (checked != nullptr && (kind == 'm' || kind == 'c') && !isa<IntLitExpr>(checked)) {
    auto message = ErrorTextSubtle::literal(intrinsicName);

    return err(WebCryptoCheckerLiteralAlgorithmError{
        .code = WebCryptoCheckerLiteralAlgorithmErrorCode::NOT_A_LITERAL,
        .message = message,
    });
  }

  return ok(checked);
}

WebCryptoChecker::ResultSubtleAvailable WebCryptoChecker::checkSubtleAvailable() const {
  VarSymbol *subtle = this->scopes->lookup(LibraryModuleNames::kSubtleName).var();
  if (subtle == nullptr) {
    auto message = ErrorTextSubtle::noSubtle();

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
  auto shape = this->checkSubtleShape(call, intrinsicName);
  if (shape.isErr()) {
    this->state.getDiagnostics().error(name->loc(), shape.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  const SubtleIntrinsic *intrinsic = shape.value();
  auto args = std::vector<Expr *>();
  bool isAllChecked = true;
  for (size_t index = 0; index < call->args.size(); ++index) {
    Expr *arg = call->args[index];
    auto *checked = this->checkSubtleArg(arg, intrinsic->params[index], intrinsicName);
    isAllChecked &= checked != nullptr && !checked->type->isError();
    args.push_back(checked != nullptr ? checked : arg);
  }
  if (!isAllChecked) {
    return call;
  }
  const auto *value = this->subtleResult(intrinsic->result);
  const auto *promise = Type::promise(value);

  return this->cryptoArgs->cryptoCall(call, intrinsic->fn, std::move(args), promise);
}

Expr *WebCryptoChecker::checkSubtleArg(Expr *arg, char kind, llvm::StringRef intrinsicName) {
  auto *checked = this->checkSubtleArgKind(arg, kind);
  auto literal = this->checkLiteralAlgorithm(checked, kind, intrinsicName);
  if (literal.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), literal.error().message);
    return nullptr;
  }

  return checked;
}

Expr *WebCryptoChecker::checkSubtleArgKind(Expr *arg, char kind) {
  switch (kind) {
    case 'm':
      return this->cryptoArgs->cryptoMd(arg, DigestUse::HMAC);
    case 'c':
      return this->cryptoArgs->cryptoCipher(arg);
    case 'd': {
      auto dataWhat = ErrorTextCryptoLabel::data();
      return this->cryptoArgs->cryptoData(arg, dataWhat, kKeysOrBuffers);
    }
    case 'o': {
      auto keyWhat = ErrorTextCryptoLabel::key();
      return this->cryptoArgs->cryptoKeyObject(arg, keyWhat);
    }
    case 'b':
      return this->argumentChecker->checkArg(arg, Type::boolTy());
    case 'i':
      return this->argumentChecker->checkArg(arg, Type::intTy());
    case 'f':
      return this->argumentChecker->checkArg(arg, Type::doubleTy());
  }

  return this->argumentChecker->checkArg(arg, Type::string());
}

const Type *WebCryptoChecker::subtleResult(char result) const {
  const auto *buffer = Type::bytesType(Type::Bytes::ArrayBuffer);
  switch (result) {
    case 'B':
      return buffer;
    case 'U':
      return Type::optional(buffer);
    case 'I':
      return Type::intTy();
  }

  const auto *keyObject = Type::cryptoType(Type::Crypto::KeyObject);

  return Type::optional(keyObject);
}

bool WebCryptoChecker::isSubtleClass(const Type *type) const {
  auto name = LibraryClassTable::getName(LibraryClass::SUBTLE_CRYPTO);
  const ClassDecl *classDecl = type != nullptr ? this->classDeclarer->classOf(type) : nullptr;

  return classDecl != nullptr && classDecl->library() && classDecl->name == name &&
         classDecl == this->state.getLibraryDeclarations().findLibraryClass(name);
}

Expr *WebCryptoChecker::subtleObject(MemberExpr *member) {
  auto subtle = this->checkSubtleAvailable();
  if (subtle.isErr()) {
    this->state.getDiagnostics().error(member->loc(), subtle.error().message);
    member->type = Type::error();
    return member;
  }
  auto *name = this->state.getAst().make<NameExpr>(member->loc(), LibraryModuleNames::kSubtleName);

  return this->exprChecker->checkExpr(name);
}

}
