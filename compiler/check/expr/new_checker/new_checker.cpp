#include "compiler/check/expr/new_checker/new_checker.h"
#include "compiler/check/async/promise_checker/promise_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/date/date_format_checker/date_format_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "compiler/library/tz_modules/http_checker/http_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/library/tz_modules/url_checker/url_checker.h"
#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

NewChecker::NewChecker(Checker &owner)
    : checker(owner) {}

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
  bool hidden = cls->library() && !this->checker.libraryCalls().inLibrary(expr->loc()) &&
                (cls == this->checker.libraryClass("CryptoKey") ||
                 cls == this->checker.libraryClass("SubtleCrypto"));
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

  const Type *element = this->checker.typeResolver().resolveType(expr->typeArgs[0], false);

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
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  if (!expr->callee && !expr->name.empty() &&
      this->checker.topLevelChecker().lookupValue(expr->name).var()) {
    expr->callee = this->checker.ctx_.make<NameExpr>(expr->loc(), expr->name);
    if (!expr->member.empty()) {
      expr->callee = this->checker.ctx_.make<MemberExpr>(expr->loc(), expr->callee, expr->member);
    }
  }
  if (expr->callee) {
    return this->checker.classValueChecker().checkValueNew(expr, expected);
  }
  if (!expr->member.empty()) {
    ScopeEntry intl = this->checker.scopes().lookup("Intl");
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls() &&
        expr->member == "Locale") {
      return this->checker.intlChecker().checkLocaleNew(expr, expected);
    }
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls()) {
      return expr->member == "DateTimeFormat"
                 ? this->checker.dateFormatChecker().checkDateFormatNew(expr->loc(), expr->args)
                 : this->checker.intlChecker().checkIntlNew(expr->loc(), expr->member, expr->args,
                                                            true);
    }
    ResultNamespaceNew plain = NewChecker::namespaceNew(expr);
    if (plain.isErr()) {
      this->checker.diags_.error(expr->loc(), plain.error().message);
    }
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = this->checker.topLevelChecker().lookupValue(expr->name).cls()) {
    ResultCryptoMadeOk crypto = this->cryptoNew(expr, cls);
    if (crypto.isErr()) {
      this->checker.diags_.error(expr->loc(), crypto.error().message);
      checkArgsLoosely();
      return expr;
    }
    Expr *checked = this->checker.methodCallChecker().checkClassNew(
        expr, ClassValueChecker::finalClass(cls), expected);
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library() && cls->name == "URL") {
      this->checker.urlChecker().checkUrlNew(made);
    }
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library()) {
      if (cls->name == "Server" || cls->name == "Socket") {
        this->checker.tzModulesChecker().noteServer(made);
      }
      if (cls->name == "Response") {
        this->checker.httpChecker().checkResponseNew(made);
      } else if (cls->name == "Request") {
        this->checker.httpChecker().checkRequestNew(made);
      } else if (cls->name == "Headers") {
        this->checker.httpChecker().checkHeadersNew(made);
      }
    }
    return checked;
  }
  if (const Checker::NodeName *n = this->checker.builtinModuleChecker().nodeName(expr->name);
      n && n->module == NodeModule::Crypto) {
    return this->checker.cryptoChecker().checkCryptoNew(expr, n->name);
  }
  ResultSupportedNew supported = NewChecker::supportedNew(expr);
  if (supported.isErr()) {
    this->checker.diags_.error(expr->loc(), supported.error().message);
    checkArgsLoosely();
    return expr;
  }
  if (expr->name == "RegExp" && !this->checker.scopes().lookup("RegExp").var() &&
      !this->checker.scopes().lookup("RegExp").func()) {
    ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
    if (plainNew.isErr()) {
      this->checker.diags_.error(expr->loc(), plainNew.error().message);
    }
    return this->checker.regexChecker().checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (expr->name == "Date" && !this->checker.scopes().lookup("Date").var() &&
      !this->checker.scopes().lookup("Date").func()) {
    ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
    if (plainNew.isErr()) {
      this->checker.diags_.error(expr->loc(), plainNew.error().message);
    }
    return this->checker.dateChecker().checkDateNew(expr->loc(), expr->args);
  }
  if ((expr->name == "Map" || expr->name == "Set" || expr->name == "WeakMap" ||
       expr->name == "WeakSet") &&
      !this->checker.scopes().lookup(expr->name).var() &&
      !this->checker.scopes().lookup(expr->name).func()) {
    return this->checker.dictNewChecker().checkDictNew(expr, expected);
  }
  if (expr->name == "Promise" && !this->checker.scopes().lookup("Promise").var() &&
      !this->checker.scopes().lookup("Promise").func()) {
    return this->checker.promiseChecker().checkPromiseNew(expr, expected);
  }
  if (expr->name == "WeakRef" && !this->checker.scopes().lookup("WeakRef").var() &&
      !this->checker.scopes().lookup("WeakRef").func()) {
    return this->checker.weakRefChecker().checkWeakRefNew(expr, expected);
  }
  if (const Type *bytes = BytesChecker::bytesTypeNamed(expr->name);
      bytes && !this->checker.scopes().lookup(expr->name).var() &&
      !this->checker.scopes().lookup(expr->name).func()) {
    return this->checker.bytesNewChecker().checkBytesNew(expr, bytes);
  }
  if (expr->name == "BigInt64Array" || expr->name == "BigUint64Array") {
    std::string what = quotedName(expr->name);
    this->checker.decimalChecker().reportBigInt(expr->loc(), what);
    checkArgsLoosely();
    return expr;
  }
  ResultClassIsFound found = NewChecker::classFound(expr);
  if (found.isErr()) {
    this->checker.diags_.error(expr->loc(), found.error().message);
    checkArgsLoosely();
    return expr;
  }

  ResultArrayElement given = this->arrayElement(expr, expected);
  if (given.isErr()) {
    this->checker.diags_.error(expr->loc(), given.error().message);
  }
  const Type *element = given.isOk() ? given.value() : Type::error();

  ResultArrayArgsFit fits = NewChecker::arrayArgs(expr);
  if (fits.isErr()) {
    this->checker.diags_.error(expr->loc(), fits.error().message);
    checkArgsLoosely();
    return expr;
  }
  if (expr->args.size() == 1) {
    expr->args[0] = this->checker.memberChecker().checkPosition(expr->args[0]);
    ResultHasZeroValue zero = NewChecker::zeroValue(element);
    if (zero.isErr()) {
      this->checker.diags_.error(expr->loc(), zero.error().message);
    }
    std::string what = ErrorTextNewArray::length();
    IndexProofs::ResultBetweenProven between =
        this->checker.indexProofs().requireBetween(expr->args[0], 0, std::nullopt, what);
    if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
      this->checker.diags_.error(expr->args[0]->loc(), between.error().message);
    }
  }
  if (!element->isError()) {
    expr->type = Type::array(element);
  }

  return expr;
}

}
