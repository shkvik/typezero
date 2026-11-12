#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
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

namespace {

constexpr llvm::StringLiteral kDateTimeFormatName = "DateTimeFormat";

}

NewChecker::NewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->urlChecker = &all.urlChecker;
  this->dateChecker = &all.dateChecker;
  this->exprChecker = &all.exprChecker;
  this->httpChecker = &all.httpChecker;
  this->indexProofs = &all.indexProofs;
  this->intlChecker = &all.intlChecker;
  this->libraryCalls = &all.libraryCalls;
  this->regexChecker = &all.regexChecker;
  this->typeResolver = &all.typeResolver;
  this->bytesChecker = &all.bytesChecker;
  this->cryptoChecker = &all.cryptoChecker;
  this->memberChecker = &all.memberChecker;
  this->dictNewChecker = &all.dictNewChecker;
  this->promiseChecker = &all.promiseChecker;
  this->weakRefChecker = &all.weakRefChecker;
  this->bytesNewChecker = &all.bytesNewChecker;
  this->topLevelChecker = &all.topLevelChecker;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->classValueChecker = &all.classValueChecker;
  this->dateFormatChecker = &all.dateFormatChecker;
  this->methodCallChecker = &all.methodCallChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

NewChecker::ResultNamespaceNew NewChecker::checkNotNamespaced(const NewExpr *expr) const {
  if (!expr->member.empty()) {
    auto message = ErrorTextNamespaceNew::namespaced(expr->name, expr->member);

    return err(NewCheckerNamespaceNewError{
        .code = NewCheckerNamespaceNewErrorCode::NAMESPACE_CLASS,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultCryptoMadeOk NewChecker::checkCryptoClassNew(const NewExpr *expr,
                                                               const ClassDecl *classDecl) const {
  bool isCryptoKey = LibraryClassTable::isName(classDecl->name, LibraryClass::CRYPTO_KEY);
  bool isHidden = classDecl->library() && !this->libraryCalls->inLibrary(expr->loc()) &&
                  this->isCryptoClass(classDecl);
  if (isHidden && isCryptoKey) {
    auto message = ErrorTextCryptoNew::cryptoKey();

    return err(NewCheckerCryptoNewError{
        .code = NewCheckerCryptoNewErrorCode::CRYPTO_KEY,
        .message = message,
    });
  }
  if (isHidden) {
    auto message = ErrorTextCryptoNew::subtle();

    return err(NewCheckerCryptoNewError{
        .code = NewCheckerCryptoNewErrorCode::SUBTLE_CRYPTO,
        .message = message,
    });
  }

  return ok(classDecl);
}

bool NewChecker::isCryptoClass(const ClassDecl *classDecl) const {
  auto &declarations = this->state.getLibraryDeclarations();
  auto keyName = LibraryClassTable::getName(LibraryClass::CRYPTO_KEY);
  auto subtleName = LibraryClassTable::getName(LibraryClass::SUBTLE_CRYPTO);

  return classDecl == declarations.findLibraryClass(keyName) ||
         classDecl == declarations.findLibraryClass(subtleName);
}

NewChecker::ResultSupportedNew NewChecker::checkSupportedNew(const NewExpr *expr) const {
  auto why = ErrorTextUnsupported::why(expr->name);
  if (!why.empty()) {
    auto message = ErrorTextSupportedNew::unsupported(expr->name, why);

    return err(NewCheckerSupportedNewError{
        .code = NewCheckerSupportedNewErrorCode::UNSUPPORTED,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultNoTypeArgsOk NewChecker::checkNoTypeArgs(const NewExpr *expr) const {
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextNoTypeArgs::given(expr->name);

    return err(NewCheckerNoTypeArgsError{
        .code = NewCheckerNoTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultClassIsFound NewChecker::checkClassFound(const NewExpr *expr) const {
  auto global = GlobalNameTable::find(expr->name);
  if (global != GlobalName::ARRAY) {
    auto message = ErrorTextClassFound::notFound(expr->name);

    return err(NewCheckerClassFoundError{
        .code = NewCheckerClassFoundErrorCode::CLASS_NOT_FOUND,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultArrayElement NewChecker::checkArrayElement(const NewExpr *expr,
                                                             const Type *expected) {
  bool isFromExpected =
      expr->typeArgs.empty() && expected != nullptr && expected->nonOptional()->isArray();
  if (expr->typeArgs.size() > 1) {
    auto message = ErrorTextNewArray::oneArg();

    return err(NewCheckerArrayElementError{
        .code = NewCheckerArrayElementErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  if (expr->typeArgs.empty() && !isFromExpected) {
    auto message = ErrorTextNewArray::needs();

    return err(NewCheckerArrayElementError{
        .code = NewCheckerArrayElementErrorCode::NEEDS_ELEMENT_TYPE,
        .message = message,
    });
  }
  if (isFromExpected) {
    return ok(expected->nonOptional()->inner());
  }

  const auto *element = this->typeResolver->resolveType(expr->typeArgs[0]);

  return ok(element);
}

NewChecker::ResultArrayArgsFit NewChecker::checkArrayArgs(const NewExpr *expr) const {
  if (expr->args.size() > 1) {
    auto message = ErrorTextArrayArgs::literal();

    return err(NewCheckerArrayArgsError{
        .code = NewCheckerArrayArgsErrorCode::VALUES_NEED_LITERAL,
        .message = message,
    });
  }

  return ok(expr);
}

NewChecker::ResultHasZeroValue NewChecker::checkZeroValue(const Type *element) const {
  if (!element->isError() && !element->hasZeroValue()) {
    auto message = ErrorTextZeroValue::none(element->name());

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
  auto global = GlobalNameTable::find(expr->name);
  if (!expr->member.empty()) {
    return this->checkNamespaceNew(expr, expected, global);
  }
  ClassDecl *classDecl = this->topLevelChecker->lookupValue(expr->name).cls();
  if (classDecl != nullptr) {
    return this->checkClassInstanceNew(expr, classDecl, expected);
  }
  const auto *node = this->builtinModuleChecker->nodeName(expr->name);
  if (node != nullptr && node->module == NodeModule::Crypto) {
    return this->cryptoChecker->checkCryptoNew(expr, node->name);
  }
  auto supported = this->checkSupportedNew(expr);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), supported.error().message);
    return this->checkArgsLoosely(expr);
  }
  auto *builtin = this->checkBuiltinNew(expr, expected, global);
  if (builtin != nullptr) {
    return builtin;
  }
  auto found = this->checkClassFound(expr);
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
  bool isVariable = expr->callee == nullptr && !expr->name.empty() &&
                    this->topLevelChecker->lookupValue(expr->name).var() != nullptr;
  if (!isVariable) {
    return;
  }
  expr->callee = this->state.getAst().make<NameExpr>(expr->loc(), expr->name);
  if (!expr->member.empty()) {
    expr->callee = this->state.getAst().make<MemberExpr>(expr->loc(), expr->callee, expr->member);
  }
}

Expr *NewChecker::checkNamespaceNew(NewExpr *expr, const Type *expected,
                                    std::optional<GlobalName> global) {
  auto intl = this->scopes->lookup(expr->name);
  bool isGlobalIntl = global == GlobalName::INTL && intl.var() == nullptr &&
                      intl.func() == nullptr && intl.cls() == nullptr;
  bool isLocale = LibraryClassTable::isName(expr->member, LibraryClass::LOCALE);
  if (isGlobalIntl && isLocale) {
    return this->intlChecker->checkLocaleNew(expr, expected);
  }
  if (isGlobalIntl && expr->member == kDateTimeFormatName) {
    return this->dateFormatChecker->checkDateFormatNew(expr->loc(), expr->args);
  }
  if (isGlobalIntl) {
    return this->intlChecker->checkIntlNew(expr->loc(), expr->member, expr->args,
                                           IntlCreation::NEW);
  }
  auto plain = this->checkNotNamespaced(expr);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plain.error().message);
  }

  return this->checkArgsLoosely(expr);
}

Expr *NewChecker::checkClassInstanceNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected) {
  auto crypto = this->checkCryptoClassNew(expr, classDecl);
  if (crypto.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), crypto.error().message);
    return this->checkArgsLoosely(expr);
  }
  auto *finalClass = this->classValueChecker->finalClass(classDecl);
  auto *checked = this->methodCallChecker->checkClassNew(expr, finalClass, expected);
  auto *made = dyn_cast<NewExpr>(checked);
  if (made != nullptr && classDecl->library()) {
    this->noteLibraryInstance(made, classDecl);
  }

  return checked;
}

void NewChecker::noteLibraryInstance(NewExpr *made, const ClassDecl *classDecl) {
  llvm::StringRef name = classDecl->name;
  bool isServer = LibraryClassTable::isName(name, LibraryClass::SERVER) ||
                  LibraryClassTable::isName(name, LibraryClass::SOCKET);
  if (LibraryClassTable::isName(name, LibraryClass::URL)) {
    this->urlChecker->walkUrlNew(made);
  }
  if (isServer) {
    this->tzModulesChecker->noteServer(made);
  }
  if (LibraryClassTable::isName(name, LibraryClass::RESPONSE)) {
    this->httpChecker->walkResponseNew(made);
  }
  if (LibraryClassTable::isName(name, LibraryClass::REQUEST)) {
    this->httpChecker->walkRequestNew(made);
  }
  if (LibraryClassTable::isName(name, LibraryClass::HEADERS)) {
    this->httpChecker->walkHeadersNew(made);
  }
}

Expr *NewChecker::checkBuiltinNew(NewExpr *expr, const Type *expected,
                                  std::optional<GlobalName> global) {
  auto named = this->scopes->lookup(expr->name);
  bool isUnshadowed = named.var() == nullptr && named.func() == nullptr;
  if (global == GlobalName::REG_EXP && isUnshadowed) {
    this->rejectTypeArgs(expr);
    return this->regexChecker->checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (global == GlobalName::DATE && isUnshadowed) {
    this->rejectTypeArgs(expr);
    return this->dateChecker->checkDateNew(expr->loc(), expr->args);
  }
  bool isCollection = global == GlobalName::MAP || global == GlobalName::SET ||
                      global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET;
  if (isCollection && isUnshadowed) {
    return this->dictNewChecker->checkDictNew(expr, expected);
  }
  if (global == GlobalName::PROMISE && isUnshadowed) {
    return this->promiseChecker->checkPromiseNew(expr, expected);
  }
  if (global == GlobalName::WEAK_REF && isUnshadowed) {
    return this->weakRefChecker->checkWeakRefNew(expr, expected);
  }
  const auto *bytes = this->bytesChecker->bytesTypeNamed(expr->name);
  if (bytes != nullptr && isUnshadowed) {
    return this->bytesNewChecker->checkBytesNew(expr, bytes);
  }
  if (global == GlobalName::BIG_INT64_ARRAY || global == GlobalName::BIG_UINT64_ARRAY) {
    auto what = ErrorTextFunctionName::quoted(expr->name);
    auto bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(expr->loc(), bigint);
    return this->checkArgsLoosely(expr);
  }

  return nullptr;
}

void NewChecker::rejectTypeArgs(const NewExpr *expr) {
  auto plainNew = this->checkNoTypeArgs(expr);
  if (plainNew.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plainNew.error().message);
  }
}

Expr *NewChecker::checkArrayNew(NewExpr *expr, const Type *expected) {
  auto given = this->checkArrayElement(expr, expected);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  const Type *element = given.isOk() ? given.value() : Type::error();
  auto fits = this->checkArrayArgs(expr);
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
  auto zero = this->checkZeroValue(element);
  if (zero.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), zero.error().message);
  }
  auto what = ErrorTextNewArray::length();
  auto between = this->indexProofs->requireBetween(expr->args[0], 0, std::nullopt, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(expr->args[0]->loc(), between.error().message);
  }
}

}
