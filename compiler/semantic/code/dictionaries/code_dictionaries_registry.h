#pragma once

#include "compiler/semantic/code/dictionaries/dictionary_access/dictionary_access.h"
#include "compiler/semantic/code/dictionaries/dictionary_construction/dictionary_construction.h"
#include "compiler/semantic/code/dictionaries/dictionary_rules/dictionary_rules.h"
#include "compiler/semantic/code/dictionaries/dictionary_sources/dictionary_sources.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeDictionariesRegistry {
public:
  explicit CodeDictionariesRegistry(SemanticState &owner);

  void checkDictIn(const CheckDictIn &task);
  void checkDictNew(const CheckDictNew &task);
  void nextPairPart(const NextPairPart &task);
  void afterPairPart(const AfterPairPart &task);
  void checkDictIndex(const CheckDictIndex &task);
  void finishRecordKey(const FinishRecordKey &task);
  void checkDictMember(const CheckDictMember &task);
  void checkDictTarget(const CheckDictTarget &task);
  void checkDictDelete(const CheckDictDelete &task);
  void resolveDictType(const ResolveDictType &task);
  void finishPairsDict(const FinishPairsDict &task);
  void afterDictSource(const AfterDictSource &task);
  void finishDictTarget(const FinishDictTarget &task);
  void finishDictDelete(const FinishDictDelete &task);
  void finishDictSource(const FinishDictSource &task);
  void checkRecordKeySlot(const CheckRecordKeySlot &task);
  void finishDictIndexRead(const FinishDictIndexRead &task);
  void finishNullishRecord(const FinishNullishRecord &task);
  void finishDictIndexError(const FinishDictIndexError &task);
  void finishDictTypeAnswer(const FinishDictTypeAnswer &task);
  void afterDictNewTypeArgs(const AfterDictNewTypeArgs &task);
  void afterNullishRecordKey(const AfterNullishRecordKey &task);
  void checkRecordNullishAssign(const CheckRecordNullishAssign &task);
  void afterNullishRecordObject(const AfterNullishRecordObject &task);

private:
  DictionaryRules dictionaryRules;
  DictionaryConstruction dictionaryConstruction;
  DictionaryAccess dictionaryAccess;
  DictionarySources dictionarySources;
};

}
