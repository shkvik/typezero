#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/objects/dict_new_checker/dict_new_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class DictNewChecker {
public:
  explicit DictNewChecker(CheckState &owner);

  void markPairs(Expr *arg);
  bool inferPairs(Expr *arg, const Type *&key, const Type *&value);
  Expr *checkDictSource(Expr *arg, bool pairs, const Type *&element, llvm::StringRef what);
  Expr *checkDictNew(NewExpr *expr, const Type *expected);

private:
  using ResultPartsJoined = Result<const Type *, DictNewCheckerPartsJoinError>;
  using ResultPairTypesOk = Result<const Type *, DictNewCheckerPairsKnownError>;
  using ResultSourceShape = Result<const Type *, DictNewCheckerSourceShapeError>;
  using ResultOneArgument = Result<const NewExpr *, DictNewCheckerArgumentCountError>;
  using ResultTypesStated = Result<const Type *, DictNewCheckerTypesGivenError>;
  using ResultPairsShaped = Result<const Type *, DictNewCheckerPairShapeError>;
  using ResultDefinedType = Result<const Type *, DictNewCheckerElementsKnownError>;

  static ResultPartsJoined partsJoin(bool keys, const Type *common, const Type *type);
  static ResultPairTypesOk pairsKnown(const Type *key, const Type *value);
  static ResultSourceShape sourceShape(const Type *type, const Type *given, const Type *wanted,
                                       bool pairs, llvm::StringRef what);
  static ResultOneArgument argumentCount(const NewExpr *expr, bool isMap);
  static ResultTypesStated typesGiven(const NewExpr *expr, const Type *type, bool isMap,
                                      bool undefinedArg);
  static ResultPairsShaped pairShape(const Type *element);
  static ResultDefinedType elementsKnown(const NewExpr *expr, const Type *key, const Type *element,
                                         bool isMap);

  CheckState &state;
};

}
