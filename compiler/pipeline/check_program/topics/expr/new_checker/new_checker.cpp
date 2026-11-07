#include "compiler/pipeline/check_program/topics/expr/new_checker/new_checker.h"
#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/async/promise_checker/promise_checker.h"
#include "compiler/pipeline/check_program/topics/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_new_checker/bytes_new_checker.h"
#include "compiler/pipeline/check_program/topics/classes/class_value_checker/class_value_checker.h"
#include "compiler/pipeline/check_program/topics/classes/method_call_checker/method_call_checker.h"
#include "compiler/pipeline/check_program/topics/crypto/crypto_checker/crypto_checker.h"
#include "compiler/pipeline/check_program/topics/date/date_checker/date_checker.h"
#include "compiler/pipeline/check_program/topics/date/date_format_checker/date_format_checker.h"
#include "compiler/pipeline/check_program/topics/decimal/decimal_checker/decimal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker_errors.h"
#include "compiler/pipeline/check_program/topics/intl/intl_checker/intl_checker.h"
#include "compiler/pipeline/check_program/topics/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/pipeline/check_program/topics/regex/regex_checker/regex_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/http_checker/http_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/topics/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/url_checker/url_checker.h"
#include "compiler/pipeline/check_program/topics/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

NewChecker::NewChecker(CheckState &owner)
    : state(owner) {}

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
  bool hidden = cls->library() && !LibraryCalls(this->state).inLibrary(expr->loc()) &&
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

  const Type *element = TypeResolver(this->state).resolveType(expr->typeArgs[0], false);

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
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  if (!expr->callee && !expr->name.empty() &&
      TopLevelChecker(this->state).lookupValue(expr->name).var()) {
    expr->callee = this->state.getAst().make<NameExpr>(expr->loc(), expr->name);
    if (!expr->member.empty()) {
      expr->callee = this->state.getAst().make<MemberExpr>(expr->loc(), expr->callee, expr->member);
    }
  }
  if (expr->callee) {
    return ClassValueChecker(this->state).checkValueNew(expr, expected);
  }
  if (!expr->member.empty()) {
    ScopeEntry intl = Scopes(this->state).lookup("Intl");
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls() &&
        expr->member == "Locale") {
      return IntlChecker(this->state).checkLocaleNew(expr, expected);
    }
    if (expr->name == "Intl" && !intl.var() && !intl.func() && !intl.cls()) {
      return expr->member == "DateTimeFormat"
                 ? DateFormatChecker(this->state).checkDateFormatNew(expr->loc(), expr->args)
                 : IntlChecker(this->state)
                       .checkIntlNew(expr->loc(), expr->member, expr->args, true);
    }
    ResultNamespaceNew plain = NewChecker::namespaceNew(expr);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), plain.error().message);
    }
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = TopLevelChecker(this->state).lookupValue(expr->name).cls()) {
    ResultCryptoMadeOk crypto = this->cryptoNew(expr, cls);
    if (crypto.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), crypto.error().message);
      checkArgsLoosely();
      return expr;
    }
    Expr *checked = MethodCallChecker(this->state)
                        .checkClassNew(expr, ClassValueChecker::finalClass(cls), expected);
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library() && cls->name == "URL") {
      UrlChecker(this->state).checkUrlNew(made);
    }
    if (auto *made = dyn_cast<NewExpr>(checked); made && cls->library()) {
      if (cls->name == "Server" || cls->name == "Socket") {
        TzModulesChecker(this->state).noteServer(made);
      }
      if (cls->name == "Response") {
        HttpChecker(this->state).checkResponseNew(made);
      } else if (cls->name == "Request") {
        HttpChecker(this->state).checkRequestNew(made);
      } else if (cls->name == "Headers") {
        HttpChecker(this->state).checkHeadersNew(made);
      }
    }
    return checked;
  }
  if (const NodeName *n = BuiltinModuleChecker(this->state).nodeName(expr->name);
      n && n->module == NodeModule::Crypto) {
    return CryptoChecker(this->state).checkCryptoNew(expr, n->name);
  }
  ResultSupportedNew supported = NewChecker::supportedNew(expr);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), supported.error().message);
    checkArgsLoosely();
    return expr;
  }
  if (expr->name == "RegExp" && !Scopes(this->state).lookup("RegExp").var() &&
      !Scopes(this->state).lookup("RegExp").func()) {
    ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
    if (plainNew.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), plainNew.error().message);
    }
    return RegexChecker(this->state).checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (expr->name == "Date" && !Scopes(this->state).lookup("Date").var() &&
      !Scopes(this->state).lookup("Date").func()) {
    ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
    if (plainNew.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), plainNew.error().message);
    }
    return DateChecker(this->state).checkDateNew(expr->loc(), expr->args);
  }
  if ((expr->name == "Map" || expr->name == "Set" || expr->name == "WeakMap" ||
       expr->name == "WeakSet") &&
      !Scopes(this->state).lookup(expr->name).var() &&
      !Scopes(this->state).lookup(expr->name).func()) {
    return DictNewChecker(this->state).checkDictNew(expr, expected);
  }
  if (expr->name == "Promise" && !Scopes(this->state).lookup("Promise").var() &&
      !Scopes(this->state).lookup("Promise").func()) {
    return PromiseChecker(this->state).checkPromiseNew(expr, expected);
  }
  if (expr->name == "WeakRef" && !Scopes(this->state).lookup("WeakRef").var() &&
      !Scopes(this->state).lookup("WeakRef").func()) {
    return WeakRefChecker(this->state).checkWeakRefNew(expr, expected);
  }
  if (const Type *bytes = BytesChecker::bytesTypeNamed(expr->name);
      bytes && !Scopes(this->state).lookup(expr->name).var() &&
      !Scopes(this->state).lookup(expr->name).func()) {
    return BytesNewChecker(this->state).checkBytesNew(expr, bytes);
  }
  if (expr->name == "BigInt64Array" || expr->name == "BigUint64Array") {
    std::string what = ErrorTextFunctionName::quoted(expr->name);
    DecimalChecker(this->state).reportBigInt(expr->loc(), what);
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
    expr->args[0] = MemberChecker(this->state).checkPosition(expr->args[0]);
    ResultHasZeroValue zero = NewChecker::zeroValue(element);
    if (zero.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), zero.error().message);
    }
    std::string what = ErrorTextNewArray::length();
    IndexProofs::ResultBetweenProven between =
        IndexProofs(this->state).requireBetween(expr->args[0], 0, std::nullopt, what);
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
