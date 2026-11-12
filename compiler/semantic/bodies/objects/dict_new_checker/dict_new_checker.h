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
class DecimalChecker;
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

  ResultPartsJoined partsJoin(bool keys, const Type *common, const Type *type) const;
  std::vector<const ReturnStmt *> branchReturns(const Stmt *body) const;
  static bool returnsPair(const ReturnStmt *returnStmt);
  ResultPairTypesOk pairsKnown(const Type *key, const Type *value) const;
  ResultSourceShape sourceShape(const Type *type, const Type *given, const Type *wanted, bool pairs,
                                llvm::StringRef what) const;
  ResultOneArgument argumentCount(const NewExpr *expr, bool isMap) const;
  ResultTypesStated typesGiven(const NewExpr *expr, const Type *type, bool isMap,
                               bool undefinedArg) const;
  ResultPairsShaped pairShape(const Type *element) const;
  ResultDefinedType elementsKnown(const NewExpr *expr, const Type *key, const Type *element,
                                  bool isMap) const;

  struct DictSource {
    const Type *type;
    Expr *arg;
  };

  Expr *failDictNew(NewExpr *expr);
  Expr *reportWeakDict(NewExpr *expr);
  std::optional<const Type *> statedDictType(NewExpr *expr, const Type *expected, bool isMap);
  const Type *wantedDict(const Type *expected, bool isMap) const;
  const Type *onlyDictMember(const Type *expected, bool isMap) const;
  std::optional<DictSource> sourcedDict(NewExpr *expr, const Type *type, bool isMap);
  std::optional<DictSource> inferredPairsDict(Expr *arg, const Type *key, const Type *value);
  std::optional<DictSource> checkedSourceDict(NewExpr *expr, Expr *arg, const Type *type,
                                              bool isMap);
  std::optional<const Type *> sourceElementDict(const NewExpr *expr, const Expr *arg,
                                                const Type *element, bool isMap);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
