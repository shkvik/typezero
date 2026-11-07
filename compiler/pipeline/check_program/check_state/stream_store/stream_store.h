#pragma once

#include "compiler/pipeline/check_program/check_state/flow_state/flow_state_types.h"
#include "compiler/program/types/types.h"
#include "llvm/Support/SMLoc.h"
#include <vector>

namespace tz {

struct StreamChunks {
  const Type *readable = nullptr;
  const Type *writable = nullptr;
};

struct StreamView {
  FieldPath path;
  llvm::SMLoc from;
  const Type *type = nullptr;
};

class StreamStore {
public:
  void rememberStreamView(const FieldPath &path, llvm::SMLoc from, const Type *view);
  bool hasStreamViews() const;
  const Type *findStreamView(const FieldPath &path, llvm::SMLoc loc) const;
  void rememberProcessStdin(VarSymbol *var);
  VarSymbol *getProcessStdin() const;

private:
  std::vector<StreamView> streamViews;
  VarSymbol *processStdin = nullptr;
};

}
