#include "compiler/semantic/code/dictionaries/code_dictionaries_registry.h"

namespace tz {

CodeDictionariesRegistry::CodeDictionariesRegistry(SemanticState &owner)
    : dictionaryRules(owner),
      dictionaryConstruction(owner, this->dictionaryRules),
      dictionaryAccess(owner),
      dictionarySources(owner, this->dictionaryRules) {}

void CodeDictionariesRegistry::checkRecordKeySlot(const CheckRecordKeySlot &task) {
  return this->dictionaryAccess.checkRecordKeySlot(task);
}

void CodeDictionariesRegistry::finishRecordKey(const FinishRecordKey &task) {
  return this->dictionaryAccess.finishRecordKey(task);
}

void CodeDictionariesRegistry::checkDictMember(const CheckDictMember &task) {
  return this->dictionaryAccess.checkDictMember(task);
}

void CodeDictionariesRegistry::checkDictIndex(const CheckDictIndex &task) {
  return this->dictionaryAccess.checkDictIndex(task);
}

void CodeDictionariesRegistry::finishDictIndexRead(const FinishDictIndexRead &task) {
  return this->dictionaryAccess.finishDictIndexRead(task);
}

void CodeDictionariesRegistry::finishDictIndexError(const FinishDictIndexError &task) {
  return this->dictionaryAccess.finishDictIndexError(task);
}

void CodeDictionariesRegistry::checkDictTarget(const CheckDictTarget &task) {
  return this->dictionaryAccess.checkDictTarget(task);
}

void CodeDictionariesRegistry::finishDictTarget(const FinishDictTarget &task) {
  return this->dictionaryAccess.finishDictTarget(task);
}

void CodeDictionariesRegistry::checkRecordNullishAssign(const CheckRecordNullishAssign &task) {
  return this->dictionaryAccess.checkRecordNullishAssign(task);
}

void CodeDictionariesRegistry::afterNullishRecordObject(const AfterNullishRecordObject &task) {
  return this->dictionaryAccess.afterNullishRecordObject(task);
}

void CodeDictionariesRegistry::afterNullishRecordKey(const AfterNullishRecordKey &task) {
  return this->dictionaryAccess.afterNullishRecordKey(task);
}

void CodeDictionariesRegistry::finishNullishRecord(const FinishNullishRecord &task) {
  return this->dictionaryAccess.finishNullishRecord(task);
}

void CodeDictionariesRegistry::checkDictDelete(const CheckDictDelete &task) {
  return this->dictionaryAccess.checkDictDelete(task);
}

void CodeDictionariesRegistry::finishDictDelete(const FinishDictDelete &task) {
  return this->dictionaryAccess.finishDictDelete(task);
}

void CodeDictionariesRegistry::checkDictIn(const CheckDictIn &task) {
  return this->dictionaryAccess.checkDictIn(task);
}

void CodeDictionariesRegistry::resolveDictType(const ResolveDictType &task) {
  return this->dictionaryConstruction.resolveDictType(task);
}

void CodeDictionariesRegistry::finishDictTypeAnswer(const FinishDictTypeAnswer &task) {
  return this->dictionaryConstruction.finishDictTypeAnswer(task);
}

void CodeDictionariesRegistry::checkDictNew(const CheckDictNew &task) {
  return this->dictionaryConstruction.checkDictNew(task);
}

void CodeDictionariesRegistry::afterDictNewTypeArgs(const AfterDictNewTypeArgs &task) {
  return this->dictionaryConstruction.afterDictNewTypeArgs(task);
}

void CodeDictionariesRegistry::nextPairPart(const NextPairPart &task) {
  return this->dictionarySources.nextPairPart(task);
}

void CodeDictionariesRegistry::afterPairPart(const AfterPairPart &task) {
  return this->dictionarySources.afterPairPart(task);
}

void CodeDictionariesRegistry::finishPairsDict(const FinishPairsDict &task) {
  return this->dictionarySources.finishPairsDict(task);
}

void CodeDictionariesRegistry::afterDictSource(const AfterDictSource &task) {
  return this->dictionarySources.afterDictSource(task);
}

void CodeDictionariesRegistry::finishDictSource(const FinishDictSource &task) {
  return this->dictionarySources.finishDictSource(task);
}

}
