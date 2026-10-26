#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class FunctionBuilder {
public:
  explicit FunctionBuilder(Lowering &lowering);

  void declareFunction(FunctionDecl *fn);
  void emitFunction(FunctionDecl *fn);
  void finishFunction();
  void closeBody();
  void beginFunction(llvm::Function *function, FunctionDecl *fn);
  void emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args);
  void emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args);
  void setFunctionAttributes(llvm::Function *fn);

private:
  Lowering &lowering_;
};

}
