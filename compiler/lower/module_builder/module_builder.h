#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The module: declarations, globals, functions and main, in that order.
class ModuleBuilder {
public:
  explicit ModuleBuilder(CodeGen &codegen);

  std::unique_ptr<llvm::Module> generate(SourceFile &file);
  llvm::Type *lower(const Type *type);
  void declareGlobals(SourceFile &file);
  void emitMain(SourceFile &file);

private:
  CodeGen &codegen_;
  llvm::Instruction *mainInit_ = nullptr;
  std::vector<const VarSymbol *> managedGlobals_;
};

} // namespace tz
