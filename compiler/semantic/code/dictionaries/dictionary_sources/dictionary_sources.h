#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/dictionaries/code_dictionaries_registry_errors.h"
#include "compiler/semantic/code/dictionaries/dictionary_rules/dictionary_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class DictionarySources {
public:
  DictionarySources(SemanticState &owner, DictionaryRules &sharedDictionaryRules);

  void nextPairPart(const NextPairPart &task);
  void afterPairPart(const AfterPairPart &task);
  void finishPairsDict(const FinishPairsDict &task);
  void afterDictSource(const AfterDictSource &task);
  void finishDictSource(const FinishDictSource &task);

private:
  enum class PairPart {
    KEY,
    VALUE,
  };
  enum class SourceItems {
    PAIRS,
    VALUES,
  };

  using ResultPairsShaped = Result<const Type *, CodeDictionariesRegistryPairShapeError>;
  using ResultPartsJoined = Result<const Type *, CodeDictionariesRegistryPartsJoinError>;
  using ResultPairTypesOk = Result<const Type *, CodeDictionariesRegistryPairsKnownError>;
  using ResultSourceShape = Result<const Type *, CodeDictionariesRegistrySourceShapeError>;
  using ResultDefinedType = Result<const Type *, CodeDictionariesRegistryElementsKnownError>;

  void finishInferredPairs(const PairsDraft &draft);
  void joinPairPart(PairsDraft &draft, const Expr *part);
  void pushPairCoercions(ArrayLitExpr *literal, const Type *key, const Type *value);


  bool isFittingPair(const Type *given, const Type *wanted) const;


  ResultPairsShaped checkPairShape(const Type *element) const;
  ResultPairTypesOk checkPairsKnown(const Type *key, const Type *value) const;
  ResultPartsJoined checkPartsJoin(PairPart part, const Type *common, const Type *type) const;
  ResultDefinedType checkElementsKnown(const NewExpr *expr, const Type *key, const Type *element,
                                       bool isMap) const;
  ResultSourceShape checkSourceShape(const Type *type, const Type *given, const Type *wanted,
                                     SourceItems items, llvm::StringRef what) const;


  std::optional<const Type *> sourceElementDict(const NewExpr *expr, const Expr *arg,
                                                const Type *element, bool isMap);

  SemanticState &state;
  DictionaryRules &dictionaryRules;
};

}
