#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class BytesChecker;
class SemanticParts;
class Scopes;
class UrlChecker;
class DateChecker;
class ExprChecker;
class HttpChecker;
class IndexProofs;
class IntlChecker;
class LibraryCalls;
class RegexChecker;
class TypeResolver;
class CryptoChecker;
class MemberChecker;
class DictNewChecker;
class PromiseChecker;
class WeakRefChecker;
class BytesNewChecker;
class TopLevelChecker;
class TzModulesChecker;
class ClassValueChecker;
class DateFormatChecker;
class MethodCallChecker;
class BuiltinModuleChecker;

class NewChecker {
public:
  NewChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkNew(NewExpr *expr, const Type *expected);

private:
  using ResultNamespaceNew = Result<const NewExpr *, NewCheckerNamespaceNewError>;
  using ResultCryptoMadeOk = Result<const ClassDecl *, NewCheckerCryptoNewError>;
  using ResultSupportedNew = Result<const NewExpr *, NewCheckerSupportedNewError>;
  using ResultNoTypeArgsOk = Result<const NewExpr *, NewCheckerNoTypeArgsError>;
  using ResultClassIsFound = Result<const NewExpr *, NewCheckerClassFoundError>;
  using ResultArrayElement = Result<const Type *, NewCheckerArrayElementError>;
  using ResultArrayArgsFit = Result<const NewExpr *, NewCheckerArrayArgsError>;
  using ResultHasZeroValue = Result<const Type *, NewCheckerZeroValueError>;

  ResultNamespaceNew checkNotNamespaced(const NewExpr *expr) const;
  ResultCryptoMadeOk checkCryptoClassNew(const NewExpr *expr, const ClassDecl *classDecl) const;
  bool isCryptoClass(const ClassDecl *classDecl) const;
  ResultSupportedNew checkSupportedNew(const NewExpr *expr) const;
  ResultNoTypeArgsOk checkNoTypeArgs(const NewExpr *expr) const;
  ResultClassIsFound checkClassFound(const NewExpr *expr) const;
  ResultArrayElement checkArrayElement(const NewExpr *expr, const Type *expected);
  ResultArrayArgsFit checkArrayArgs(const NewExpr *expr) const;
  ResultHasZeroValue checkZeroValue(const Type *element) const;

  void bindValueCallee(NewExpr *expr);
  Expr *checkArgsLoosely(NewExpr *expr);
  void rejectTypeArgs(const NewExpr *expr);
  void walkArrayLength(NewExpr *expr, const Type *element);
  Expr *checkArrayNew(NewExpr *expr, const Type *expected);
  void noteLibraryInstance(NewExpr *made, const ClassDecl *classDecl);
  Expr *checkClassInstanceNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected);
  Expr *checkBuiltinNew(NewExpr *expr, const Type *expected, std::optional<GlobalName> global);
  Expr *checkNamespaceNew(NewExpr *expr, const Type *expected, std::optional<GlobalName> global);

  SemanticState &state;
  Scopes *scopes = nullptr;
  UrlChecker *urlChecker = nullptr;
  DateChecker *dateChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  HttpChecker *httpChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  IntlChecker *intlChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  RegexChecker *regexChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  BytesChecker *bytesChecker = nullptr;
  CryptoChecker *cryptoChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  DictNewChecker *dictNewChecker = nullptr;
  PromiseChecker *promiseChecker = nullptr;
  WeakRefChecker *weakRefChecker = nullptr;
  BytesNewChecker *bytesNewChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  DateFormatChecker *dateFormatChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
