#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// A function: its declaration, attributes, parameters and body.
class FunctionBuilder {
public:
  explicit FunctionBuilder(Lowering &lowering);

  void declareFunction(FunctionDecl *fn);
  void emitFunction(FunctionDecl *fn);
  void finishFunction();
  // Returns where control reaches the end of the body.
  void closeBody();
  // Starts emitting the body of `fn` (null for main) into `function`.
  void beginFunction(llvm::Function *function, FunctionDecl *fn);
  void emitParamsAndBody(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args);
  // The parameters alone, as locals of the current scope.
  void emitParams(FunctionDecl *fn, llvm::ArrayRef<llvm::Value *> args);
  void setFunctionAttributes(llvm::Function *fn);

private:
  Lowering &lowering_;
};

} // namespace tz
