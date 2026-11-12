#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace tz {

struct ErrorWatch {
  std::vector<EmitterKey> listened;
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> pending;
};

class FlowState {
public:
  const Flow &getFlow() const;
  void replaceFlow(Flow flow);
  Flow takeFlow();
  Facts &getFacts();
  void markUnreachable();
  bool isUnreachable() const;
  void markBeforeSuper();
  void markSuperCalled();
  bool isBeforeSuper() const;
  void addSetField(llvm::StringRef name);
  void forgetSetFields();
  bool isFieldSet(llvm::StringRef name) const;
  void markDefined(const VarSymbol *variable);
  void forgetDefined(const VarSymbol *variable);
  bool isDefined(const VarSymbol *variable) const;
  void addNarrowed(const VarSymbol *variable, const Type *type);
  const Type *findNarrowed(const VarSymbol *variable) const;
  void markOkResult(const VarSymbol *variable);
  void markErrResult(const VarSymbol *variable);
  void forgetResultState(const VarSymbol *variable);
  bool isOkResult(const VarSymbol *variable) const;
  bool isErrResult(const VarSymbol *variable) const;
  void defineField(const FieldPath &path);
  bool isFieldDefined(const FieldPath &path) const;
  void forgetFieldsNamed(llvm::StringRef name);
  bool hasNarrowedFields() const;
  const Type *findNarrowedField(const FieldPath &path) const;
  void enterErrorWatch();
  void leaveErrorWatch();
  void listenForErrors(const EmitterKey &key);
  void addPendingErrors(const EmitterKey &key, llvm::SMLoc loc);
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> takePendingErrors();
  void markFieldAssigned(llvm::StringRef name);
  bool isFieldAssigned(llvm::StringRef name) const;
  void rememberFixedLength(const VarSymbol *variable, int64_t length);
  std::optional<int64_t> findFixedLength(const VarSymbol *variable) const;

private:
  Flow flow;
  std::vector<ErrorWatch> outerErrorWatches;
  llvm::StringSet<> assignedFields;
  llvm::DenseMap<const VarSymbol *, int64_t> fixedLengths;
};

}
