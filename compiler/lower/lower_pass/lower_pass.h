#pragma once

#include "compiler/lower/lowered/lowered.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include <string>
#include <vector>

namespace tz {

class LowererRegistry;

class Target;
struct MemoryPlan;
struct ParallelPlan;
struct Program;

class LowerPass {
public:
  LowerPass(llvm::LLVMContext &context, const Target &target, llvm::StringRef moduleName,
            const LowererRegistry &lowerers, DebugLevel debug, bool optimize);

  Result<Lowered, std::vector<Diagnostic>> run(Program &program, const ParallelPlan &plan,
                                               const MemoryPlan &memory) const;

private:
  llvm::LLVMContext &context;
  const Target &target;
  std::string moduleName;
  const LowererRegistry &lowerers;
  DebugLevel debug;
  bool optimize;
};

}
