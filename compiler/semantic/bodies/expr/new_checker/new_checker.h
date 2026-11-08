#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class NewChecker {
public:
  explicit NewChecker(SemanticState &owner);

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
};

}
