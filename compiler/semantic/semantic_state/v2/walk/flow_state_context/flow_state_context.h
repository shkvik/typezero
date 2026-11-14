#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/Support/SMLoc.h"
#include <utility>
#include <vector>

namespace tz {

struct ErrorWatch {
  std::vector<EmitterKey> listened;
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> pending;
};

class FlowStateContext {
public:
  void enterErrorWatch();
  void forgetSetFields();
  void leaveErrorWatch();
  void markBeforeSuper();
  void markSuperCalled();
  void markUnreachable();
  void replaceFlow(Flow flow);
  void addSetField(llvm::StringRef name);
  void defineField(const FieldPath &path);
  void listenForErrors(const EmitterKey &key);
  void markDefined(const VarSymbol *variable);
  void forgetFieldsNamed(llvm::StringRef name);
  void markOkResult(const VarSymbol *variable);
  void forgetDefined(const VarSymbol *variable);
  void markErrResult(const VarSymbol *variable);
  void forgetResultState(const VarSymbol *variable);
  void addNarrowed(const VarSymbol *variable, const Type *type);
  void addPendingErrors(const EmitterKey &key, llvm::SMLoc loc);


  bool isBeforeSuper() const;
  bool isUnreachable() const;
  bool hasNarrowedFields() const;
  bool isFieldSet(llvm::StringRef name) const;
  bool isDefined(const VarSymbol *variable) const;
  bool isFieldDefined(const FieldPath &path) const;
  bool isOkResult(const VarSymbol *variable) const;
  bool isErrResult(const VarSymbol *variable) const;


  const Type *findNarrowed(const VarSymbol *variable) const;
  const Type *findVariableType(const VarSymbol *variable) const;
  const Type *findNarrowedField(const FieldPath &path) const;


  Flow takeFlow();
  Facts &getFacts();
  const Flow &getFlow() const;
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> takePendingErrors();

private:
  Flow flow;
  std::vector<ErrorWatch> outerErrorWatches;
};

}
