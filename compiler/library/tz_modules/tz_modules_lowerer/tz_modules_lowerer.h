#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class TzModulesLowerer {
public:
  explicit TzModulesLowerer(Lowering &lowering);

  void emitExternal(FunctionDecl *fn);
  void appendPrintHooks(llvm::ArrayRef<const ClassDecl *> chain,
                        std::vector<llvm::Constant *> &entries);
  llvm::Value *emitResultAdapter(CastExpr *cast);

private:
  Lowering &lowering_;
  std::map<std::pair<const Type *, const Type *>, llvm::Constant *> resultAdapters_;
};

}
