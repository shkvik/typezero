#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/dict_new_checker/dict_new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class DictChecker;
class ExprChecker;
class FlowChecker;
class TypeRelations;
class IteratorMethods;
class GeneratorChecker;

class DictNewChecker {
public:
  DictNewChecker(SemanticState &owner, SemanticParts &all);

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

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
