#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types_v2/mappings/types_mappings_registry_errors.h"

namespace tz {

class TypesMappingsRegistry {
public:
  explicit TypesMappingsRegistry(SemanticState &owner);

  void evalMapped(const EvalMapped &task);
  void startMapped(const StartMapped &task);
  void nextMappedKey(const NextMappedKey &task);
  void afterMappedKeys(const AfterMappedKeys &task);
  void afterMappedParts(const AfterMappedParts &task);
  void afterMappedValue(const AfterMappedValue &task);
  void afterMappedRename(const AfterMappedRename &task);
  void afterMappedSource(const AfterMappedSource &task);

private:
  void addMappedFields(MappedDraft draft);
  void startMappedParts(const TypeRef &ref, const Type *keys, const Type *source);
  void mergeMappedField(std::vector<Field> &fields, const std::string &name, const Type *type,
                        bool isReadonly) const;


  bool isOpenMapped(const MappedOperands &mapped) const;
  bool isMappedReadonly(const MappedOperands &mapped, llvm::StringRef name) const;


  const Type *widerKeys(const Type *keys) const;
  const Type *widerSource(const Type *keys) const;
  const Type *nonStringKey(const Type *keys) const;
  const Type *nonStringRename(const Type *rename) const;
  const Type *mappedFieldType(const MappedOperands &mapped, const Type *value) const;


  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;

  SemanticState &state;
};

}
