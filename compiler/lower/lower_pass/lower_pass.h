#pragma once

#include "compiler/lower/lowered/lowered.h"
#include "compiler/program/errors/errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include <string>

namespace tz {

class FeatureRegistry;

class Target;
struct MemoryPlan;
struct ParallelPlan;
struct Program;

class LowerPass {
public:
  LowerPass(llvm::LLVMContext &context, const Target &target, llvm::StringRef moduleName,
            const FeatureRegistry &features);

  Result<Lowered, Errors> run(Program &program, const ParallelPlan &plan,
                              const MemoryPlan &memory) const;

private:
  llvm::LLVMContext &context_;
  const Target &target_;
  std::string moduleName_;
  const FeatureRegistry &features_;
};

}
