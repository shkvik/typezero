#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/async/promise_checker/promise_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/tz_modules/http_checker/http_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

NewChecker::NewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->urlChecker = &all.getUrlChecker();
  this->dateChecker = &all.getDateChecker();
  this->exprChecker = &all.getExprChecker();
  this->httpChecker = &all.getHttpChecker();
  this->indexProofs = &all.getIndexProofs();
  this->intlChecker = &all.getIntlChecker();
  this->libraryCalls = &all.getLibraryCalls();
  this->regexChecker = &all.getRegexChecker();
  this->typeResolver = &all.getTypeResolver();
  this->cryptoChecker = &all.getCryptoChecker();
  this->memberChecker = &all.getMemberChecker();
  this->dictNewChecker = &all.getDictNewChecker();
  this->promiseChecker = &all.getPromiseChecker();
  this->weakRefChecker = &all.getWeakRefChecker();
  this->bytesNewChecker = &all.getBytesNewChecker();
  this->topLevelChecker = &all.getTopLevelChecker();
  this->tzModulesChecker = &all.getTzModulesChecker();
  this->classValueChecker = &all.getClassValueChecker();
  this->dateFormatChecker = &all.getDateFormatChecker();
  this->methodCallChecker = &all.getMethodCallChecker();
  this->builtinModuleChecker = &all.getBuiltinModuleChecker();
}

NewChecker::ResultNamespaceNew NewChecker::namespaceNew(const NewExpr *expr) {
  if (!expr->member.empty()) {
    std::string message = ErrorTextNamespaceNew::namespaced(expr->name, expr->member);

    return err(NewCheckerNamespaceNewError{
        .code = NewCheckerNamespaceNewErrorCode::NAMESPACE_CLASS,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultCryptoMadeOk NewChecker::cryptoNew(const NewExpr *expr,
                                                     const ClassDecl *cls) const {
  bool hidden = cls->library() && !this->libraryCalls->inLibrary(expr->loc()) &&
                (cls == this->state.getLibraryDeclarations().findLibraryClass("CryptoKey") ||
                 cls == this->state.getLibraryDeclarations().findLibraryClass("SubtleCrypto"));
  if (hidden && cls->name == "CryptoKey") {
    std::string message = ErrorTextCryptoNew::cryptoKey();

    return err(NewCheckerCryptoNewError{
        .code = NewCheckerCryptoNewErrorCode::CRYPTO_KEY,
        .message = message,
    });
  }
  if (hidden) {
    std::string message = ErrorTextCryptoNew::subtle();

    return err(NewCheckerCryptoNewError{
        .code = NewCheckerCryptoNewErrorCode::SUBTLE_CRYPTO,
        .message = message,
    });
  }

  return ok(cls);
}

NewChecker::ResultSupportedNew NewChecker::supportedNew(const NewExpr *expr) {
  std::string why = ErrorTextUnsupported::why(expr->name);
  if (!why.empty()) {
    std::string message = ErrorTextSupportedNew::unsupported(expr->name, why);

    return err(NewCheckerSupportedNewError{
        .code = NewCheckerSupportedNewErrorCode::UNSUPPORTED,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultNoTypeArgsOk NewChecker::noTypeArgs(const NewExpr *expr) {
  if (!expr->typeArgs.empty()) {
    std::string message = ErrorTextNoTypeArgs::given(expr->name);

    return err(NewCheckerNoTypeArgsError{
        .code = NewCheckerNoTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultClassIsFound NewChecker::classFound(const NewExpr *expr) {
  if (expr->name != "Array") {
    std::string message = ErrorTextClassFound::notFound(expr->name);

    return err(NewCheckerClassFoundError{
        .code = NewCheckerClassFoundErrorCode::CLASS_NOT_FOUND,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultArrayElement NewChecker::arrayElement(const NewExpr *expr, const Type *expected) {
  bool fromExpected = expr->typeArgs.empty() && expected && expected->nonOptional()->isArray();
  if (expr->typeArgs.size() > 1) {
    std::string message = ErrorTextNewArray::oneArg();

    return err(NewCheckerArrayElementError{
        .code = NewCheckerArrayElementErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  if (expr->typeArgs.empty() && !fromExpected) {
    std::string message = ErrorTextNewArray::needs();

    return err(NewCheckerArrayElementError{
        .code = NewCheckerArrayElementErrorCode::NEEDS_ELEMENT_TYPE,
        .message = message,
    });
  }
  if (fromExpected) {
    return ok(expected->nonOptional()->inner());
  }

  const Type *element = this->typeResolver->resolveType(expr->typeArgs[0], false);

  return ok(element);
}

NewChecker::ResultArrayArgsFit NewChecker::arrayArgs(const NewExpr *expr) {
  if (expr->args.size() > 1) {
    std::string message = ErrorTextArrayArgs::literal();

    return err(NewCheckerArrayArgsError{
        .code = NewCheckerArrayArgsErrorCode::VALUES_NEED_LITERAL,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultHasZeroValue NewChecker::zeroValue(const Type *element) {
  if (!element->isError() && !element->hasZeroValue()) {
    std::string message = ErrorTextZeroValue::none(element->name());

    return err(NewCheckerZeroValueError{
        .code = NewCheckerZeroValueErrorCode::NO_ZERO_VALUE,
        .message = message,
    });
  }

  return ok(element);
}

Expr *NewChecker::checkNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  if (!expr->callee && !expr->name.empty() &&
      this->topLevelChecker->lookupValue(expr->name).var()) {
    expr->callee = this->state.getAst().make<NameExpr>(expr->loc(), expr->name);
    if (!expr->member.empty()) {
      expr->callee = this->state.getAst().make<MemberExpr>(expr->loc(), expr->callee, expr->member);
    }
  }
  if (expr->callee) {
    return this->classValueChecker->checkValueNew(expr, expected);
  }
  if (!expr->member.empty()) {
    ScopeEntry intl = this->scopes->lookup("Intl");
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls() &&
        expr->member == "Locale") {
      return this->intlChecker->checkLocaleNew(expr, expected);
    }
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls()) {
      return expr->member == "DateTimeFormat"
                 ? this->dateFormatChecker->checkDateFormatNew(expr->loc(), expr->args)
                 : this->intlChecker->checkIntlNew(expr->loc(), expr->member, expr->args, true);
    }
    ResultNamespaceNew plain = NewChecker::namespaceNew(expr);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), plain.error().message);
    }
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = this->topLevelChecker->lookupValue(expr->name).cls()) {
    ResultCryptoMadeOk crypto = this->cryptoNew(expr, cls);
    if (crypto.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), crypto.error().message);
      checkArgsLoosely();
      return expr;
    }
    Expr *checked =
        this->methodCallChecker->checkClassNew(expr, ClassValueChecker::finalClass(cls), expected);
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library() && cls->name == "URL") {
      this->urlChecker->checkUrlNew(made);
    }
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library()) {
      if (cls->name == "Server" || cls->name == "Socket") {
        this->tzModulesChecker->noteServer(made);
      }
      if (cls->name == "Response") {
        this->httpChecker->checkResponseNew(made);
      } else if (cls->name == "Request") {
        this->httpChecker->checkRequestNew(made);
      } else if (cls->name == "Headers") {
        this->httpChecker->checkHeadersNew(made);
      }
    }
    return checked;
  }
  if (const NodeName *n = this->builtinModuleChecker->nodeName(expr->name);
      n && n->module == NodeModule::Crypto) {
    return this->cryptoChecker->checkCryptoNew(expr, n->name);
  }
  ResultSupportedNew supported = NewChecker::supportedNew(expr);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), supported.error().message);
    checkArgsLoosely();
    return expr;
  }
  if (expr->name == "RegExp" && !this->scopes->lookup("RegExp").var() &&
      !this->scopes->lookup("RegExp").func()) {
    ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
    if (plainNew.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), plainNew.error().message);
    }
    return this->regexChecker->checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (expr->name == "Date" && !this->scopes->lookup("Date").var() &&
      !this->scopes->lookup("Date").func()) {
    ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
    if (plainNew.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), plainNew.error().message);
    }
    return this->dateChecker->checkDateNew(expr->loc(), expr->args);
  }
  if ((expr->name == "Map" || expr->name == "Set" || expr->name == "WeakMap" ||
       expr->name == "WeakSet") &&
      !this->scopes->lookup(expr->name).var() && !this->scopes->lookup(expr->name).func()) {
    return this->dictNewChecker->checkDictNew(expr, expected);
  }
  if (expr->name == "Promise" && !this->scopes->lookup("Promise").var() &&
      !this->scopes->lookup("Promise").func()) {
    return this->promiseChecker->checkPromiseNew(expr, expected);
  }
  if (expr->name == "WeakRef" && !this->scopes->lookup("WeakRef").var() &&
      !this->scopes->lookup("WeakRef").func()) {
    return this->weakRefChecker->checkWeakRefNew(expr, expected);
  }
  if (const Type *bytes = BytesChecker::bytesTypeNamed(expr->name);
      bytes && !this->scopes->lookup(expr->name).var() &&
      !this->scopes->lookup(expr->name).func()) {
    return this->bytesNewChecker->checkBytesNew(expr, bytes);
  }
  if (expr->name == "BigInt64Array" || expr->name == "BigUint64Array") {
    std::string what = ErrorTextFunctionName::quoted(expr->name);
    std::string bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(expr->loc(), bigint);
    checkArgsLoosely();
    return expr;
  }
  ResultClassIsFound found = NewChecker::classFound(expr);
  if (found.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), found.error().message);
    checkArgsLoosely();
    return expr;
  }

  ResultArrayElement given = this->arrayElement(expr, expected);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  const Type *element = given.isOk() ? given.value() : Type::error();

  ResultArrayArgsFit fits = NewChecker::arrayArgs(expr);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), fits.error().message);
    checkArgsLoosely();
    return expr;
  }
  if (expr->args.size() == 1) {
    expr->args[0] = this->memberChecker->checkPosition(expr->args[0]);
    ResultHasZeroValue zero = NewChecker::zeroValue(element);
    if (zero.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), zero.error().message);
    }
    std::string what = ErrorTextNewArray::length();
    IndexProofs::ResultBetweenProven between =
        this->indexProofs->requireBetween(expr->args[0], 0, std::nullopt, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.getDiagnostics().error(expr->args[0]->loc(), between.error().message);
    }
  }
  if (!element->isError()) {
    expr->type = Type::array(element);
  }

  return expr;
}

}
