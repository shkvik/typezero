#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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
  std::optional<GlobalName> global = GlobalNameTable::find(expr->name);
  if (global != GlobalName::ARRAY) {
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
  this->bindValueCallee(expr);
  if (expr->callee != nullptr) {
    return this->classValueChecker->checkValueNew(expr, expected);
  }
  std::optional<GlobalName> global = GlobalNameTable::find(expr->name);
  if (!expr->member.empty()) {
    return this->checkNamespaceNew(expr, expected, global);
  }
  ClassDecl *cls = this->topLevelChecker->lookupValue(expr->name).cls();
  if (cls != nullptr) {
    return this->checkClassInstanceNew(expr, cls, expected);
  }
  const NodeName *node = this->builtinModuleChecker->nodeName(expr->name);
  if (node != nullptr && node->module == NodeModule::Crypto) {
    return this->cryptoChecker->checkCryptoNew(expr, node->name);
  }
  ResultSupportedNew supported = NewChecker::supportedNew(expr);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), supported.error().message);
    return this->checkArgsLoosely(expr);
  }
  Expr *builtin = this->checkBuiltinNew(expr, expected, global);
  if (builtin != nullptr) {
    return builtin;
  }
  ResultClassIsFound found = NewChecker::classFound(expr);
  if (found.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), found.error().message);
    return this->checkArgsLoosely(expr);
  }

  return this->checkArrayNew(expr, expected);
}

Expr *NewChecker::checkArgsLoosely(NewExpr *expr) {
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return expr;
}

void NewChecker::bindValueCallee(NewExpr *expr) {
  bool variable = expr->callee == nullptr && !expr->name.empty() &&
                  this->topLevelChecker->lookupValue(expr->name).var() != nullptr;
  if (!variable) {
    return;
  }
  expr->callee = this->state.getAst().make<NameExpr>(expr->loc(), expr->name);
  if (!expr->member.empty()) {
    expr->callee = this->state.getAst().make<MemberExpr>(expr->loc(), expr->callee, expr->member);
  }
}

Expr *NewChecker::checkNamespaceNew(NewExpr *expr, const Type *expected,
                                    std::optional<GlobalName> global) {
  ScopeEntry intl = this->scopes->lookup(expr->name);
  bool globalIntl = global == GlobalName::INTL && intl.var() == nullptr && intl.func() == nullptr &&
                    intl.cls() == nullptr;
  if (globalIntl && expr->member == "Locale") {
    return this->intlChecker->checkLocaleNew(expr, expected);
  }
  if (globalIntl && expr->member == "DateTimeFormat") {
    return this->dateFormatChecker->checkDateFormatNew(expr->loc(), expr->args);
  }
  if (globalIntl) {
    return this->intlChecker->checkIntlNew(expr->loc(), expr->member, expr->args, true);
  }
  ResultNamespaceNew plain = NewChecker::namespaceNew(expr);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plain.error().message);
  }

  return this->checkArgsLoosely(expr);
}

Expr *NewChecker::checkClassInstanceNew(NewExpr *expr, ClassDecl *cls, const Type *expected) {
  ResultCryptoMadeOk crypto = this->cryptoNew(expr, cls);
  if (crypto.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), crypto.error().message);
    return this->checkArgsLoosely(expr);
  }
  ClassDecl *finalClass = ClassValueChecker::finalClass(cls);
  Expr *checked = this->methodCallChecker->checkClassNew(expr, finalClass, expected);
  auto *made = dyn_cast<NewExpr>(checked);
  if (made != nullptr && cls->library()) {
    this->noteLibraryInstance(made, cls);
  }

  return checked;
}

void NewChecker::noteLibraryInstance(NewExpr *made, const ClassDecl *cls) {
  if (cls->name == "URL") {
    this->urlChecker->checkUrlNew(made);
  }
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

Expr *NewChecker::checkBuiltinNew(NewExpr *expr, const Type *expected,
                                  std::optional<GlobalName> global) {
  ScopeEntry named = this->scopes->lookup(expr->name);
  bool unshadowed = named.var() == nullptr && named.func() == nullptr;
  if (global == GlobalName::REG_EXP && unshadowed) {
    this->rejectTypeArgs(expr);
    return this->regexChecker->checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (global == GlobalName::DATE && unshadowed) {
    this->rejectTypeArgs(expr);
    return this->dateChecker->checkDateNew(expr->loc(), expr->args);
  }
  bool collection = global == GlobalName::MAP || global == GlobalName::SET ||
                    global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET;
  if (collection && unshadowed) {
    return this->dictNewChecker->checkDictNew(expr, expected);
  }
  if (global == GlobalName::PROMISE && unshadowed) {
    return this->promiseChecker->checkPromiseNew(expr, expected);
  }
  if (global == GlobalName::WEAK_REF && unshadowed) {
    return this->weakRefChecker->checkWeakRefNew(expr, expected);
  }
  const Type *bytes = BytesChecker::bytesTypeNamed(expr->name);
  if (bytes != nullptr && unshadowed) {
    return this->bytesNewChecker->checkBytesNew(expr, bytes);
  }
  if (global == GlobalName::BIG_INT64_ARRAY || global == GlobalName::BIG_UINT64_ARRAY) {
    std::string what = ErrorTextFunctionName::quoted(expr->name);
    std::string bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(expr->loc(), bigint);
    return this->checkArgsLoosely(expr);
  }

  return nullptr;
}

void NewChecker::rejectTypeArgs(const NewExpr *expr) {
  ResultNoTypeArgsOk plainNew = NewChecker::noTypeArgs(expr);
  if (plainNew.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plainNew.error().message);
  }
}

Expr *NewChecker::checkArrayNew(NewExpr *expr, const Type *expected) {
  ResultArrayElement given = this->arrayElement(expr, expected);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  const Type *element = given.isOk() ? given.value() : Type::error();
  ResultArrayArgsFit fits = NewChecker::arrayArgs(expr);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), fits.error().message);
    return this->checkArgsLoosely(expr);
  }
  if (expr->args.size() == 1) {
    this->walkArrayLength(expr, element);
  }
  if (!element->isError()) {
    expr->type = Type::array(element);
  }

  return expr;
}

void NewChecker::walkArrayLength(NewExpr *expr, const Type *element) {
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
}
