#pragma once

#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/Support/SMLoc.h"
#include <vector>

namespace tz {

struct StreamView {
  FieldPath path;
  llvm::SMLoc from;
  const Type *type = nullptr;
};

class StreamContext {
public:
  void rememberProcessStdin(VarSymbol *variable);
  void rememberStreamView(const FieldPath &path, llvm::SMLoc from, const Type *view);


  bool hasStreamViews() const;


  VarSymbol *getProcessStdin() const;
  const Type *findStreamView(const FieldPath &path, llvm::SMLoc loc) const;

private:
  VarSymbol *processStdin = nullptr;
  std::vector<StreamView> streamViews;
};

}
