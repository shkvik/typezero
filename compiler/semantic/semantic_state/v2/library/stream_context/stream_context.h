#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
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
  void rememberHookParams(const FunctionDecl *functionDecl, std::vector<const Type *> params);


  bool hasStreamViews() const;


  VarSymbol *getProcessStdin() const;
  const Type *findStreamView(const FieldPath &path, llvm::SMLoc loc) const;
  std::optional<std::vector<const Type *>> findHookParams(const FunctionDecl *functionDecl) const;

private:
  VarSymbol *processStdin = nullptr;
  std::vector<StreamView> streamViews;
  llvm::DenseMap<const FunctionDecl *, std::vector<const Type *>> hookParams;
};

}
