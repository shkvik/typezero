#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ModuleBuilder {
public:
  explicit ModuleBuilder(Lowering &lowering);

  std::unique_ptr<llvm::Module> generate(SourceFile &file);
  llvm::Type *lower(const Type *type);
  void declareGlobals(SourceFile &file);
  void emitMain(SourceFile &file);

private:
  Lowering &lowering_;
  llvm::Instruction *mainInit_ = nullptr;
  std::vector<const VarSymbol *> managedGlobals_;
};

}
