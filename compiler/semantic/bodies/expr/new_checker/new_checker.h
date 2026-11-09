#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

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

  static ResultNamespaceNew namespaceNew(const NewExpr *expr);
  ResultCryptoMadeOk cryptoNew(const NewExpr *expr, const ClassDecl *cls) const;
  static ResultSupportedNew supportedNew(const NewExpr *expr);
  static ResultNoTypeArgsOk noTypeArgs(const NewExpr *expr);
  static ResultClassIsFound classFound(const NewExpr *expr);
  ResultArrayElement arrayElement(const NewExpr *expr, const Type *expected);
  static ResultArrayArgsFit arrayArgs(const NewExpr *expr);
  static ResultHasZeroValue zeroValue(const Type *element);

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
