#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ModuleBuilder {
public:
  explicit ModuleBuilder(Lowering &owner);

  std::unique_ptr<llvm::Module> generate(SourceFile &file);
  llvm::Type *lower(const Type *type);
  void declareGlobals(SourceFile &file);
  void emitMain(SourceFile &file);

private:
  Lowering &lowering;
  llvm::Instruction *mainInit = nullptr;
  std::vector<const VarSymbol *> managedGlobals;
};

}
