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

enum class SourceItems {
  PAIRS,
  VALUES,
};

class DictNewChecker {
public:
  struct InferredPairs {
    const Type *key;
    const Type *value;
  };

  struct SourceElement {
    Expr *arg;
    const Type *element;
  };

  DictNewChecker(SemanticState &owner, SemanticParts &all);

  void markPairs(Expr *arg);
  std::optional<InferredPairs> inferPairs(Expr *arg);
  SourceElement checkDictSource(Expr *arg, SourceItems items, const Type *wanted,
                                llvm::StringRef what);
  Expr *checkDictNew(NewExpr *expr, const Type *expected);

private:
  using ResultPartsJoined = Result<const Type *, DictNewCheckerPartsJoinError>;
  using ResultPairTypesOk = Result<const Type *, DictNewCheckerPairsKnownError>;
  using ResultSourceShape = Result<const Type *, DictNewCheckerSourceShapeError>;
  using ResultOneArgument = Result<const NewExpr *, DictNewCheckerArgumentCountError>;
  using ResultTypesStated = Result<const Type *, DictNewCheckerTypesGivenError>;
  using ResultPairsShaped = Result<const Type *, DictNewCheckerPairShapeError>;
  using ResultDefinedType = Result<const Type *, DictNewCheckerElementsKnownError>;

  enum class PairPart {
    KEY,
    VALUE,
  };

  struct PairJoin {
    const Type *key;
    const Type *value;
    bool isTyped;
  };

  struct DictSource {
    const Type *type;
    Expr *arg;
  };

  std::vector<const ReturnStmt *> branchReturns(const Stmt *body) const;
  void pushBlockStatements(const BlockStmt *block, std::vector<const Stmt *> &stack) const;
  bool isPairReturn(const ReturnStmt *returnStmt) const;
  ResultPartsJoined checkPartsJoin(PairPart part, const Type *common, const Type *type) const;
  ResultPairTypesOk checkPairsKnown(const Type *key, const Type *value) const;
  ResultSourceShape checkSourceShape(const Type *type, const Type *given, const Type *wanted,
                                     SourceItems items, llvm::StringRef what) const;
  bool isFittingPair(const Type *given, const Type *wanted) const;
  ResultOneArgument checkArgumentCount(const NewExpr *expr, bool isMap) const;
  ResultTypesStated checkTypesGiven(const NewExpr *expr, const Type *type, bool isMap,
                                    bool isUndefinedArg) const;
  ResultPairsShaped checkPairShape(const Type *element) const;
  ResultDefinedType checkElementsKnown(const NewExpr *expr, const Type *key, const Type *element,
                                       bool isMap) const;
  bool isPairElement(const ArrayLitExpr::Element &element) const;
  PairJoin joinPart(const PairJoin &join, Expr *&part, PairPart which);
  void typePairs(ArrayLitExpr *literal, const Type *key, const Type *value);
  void walkSourceSends(const Expr *arg, const Type *type, llvm::StringRef what);
  Expr *checkEmptyDictNew(NewExpr *expr, const Type *type, bool isMap);
  Expr *finishDictNew(NewExpr *expr, const Type *type, std::vector<Expr *> args);
  Expr *failDictNew(NewExpr *expr);
  Expr *reportWeakDict(NewExpr *expr);
  std::optional<const Type *> statedDictType(NewExpr *expr, const Type *expected, bool isMap);
  const Type *wantedDict(const Type *expected, bool isMap) const;
  const Type *onlyDictMember(const Type *expected, bool isMap) const;
  std::optional<DictSource> sourcedDict(NewExpr *expr, const Type *type, bool isMap);
  std::optional<DictSource> inferredPairsDict(Expr *arg, const InferredPairs &pairs);
  std::optional<DictSource> checkedSourceDict(NewExpr *expr, Expr *arg, const Type *type,
                                              bool isMap);
  const Type *wantedElement(const Type *type, bool isMap) const;
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
