#include "compiler/lower/lower_pass/lower_pass.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/optimize/target/target.h"
#include "compiler/program/program/program.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

LowerPass::LowerPass(llvm::LLVMContext &context, const Target &target, llvm::StringRef moduleName,
                     const FeatureRegistry &features)
    : context_(context), target_(target), moduleName_(moduleName.str()), features_(features) {}

Result<Lowered, Errors> LowerPass::run(Program &program, const ParallelPlan &plan) const {
  llvm::DataLayout layout = target_.machine().createDataLayout();
  CodeGen codegen(context_, moduleName_, layout);
  codegen.setSourceMgr(&program.sourceMgr);
  codegen.setParallelPlan(&plan);
  codegen.setFeatures(&features_);
  codegen.setFeatures(&features_);
  std::unique_ptr<llvm::Module> module = codegen.moduleBuilder().generate(program.file);
  std::string problems;
  llvm::raw_string_ostream out(problems);
  if (llvm::verifyModule(*module, &out)) {
    Errors errors;
    errors.raw(problems);
    errors.tool("internal error: the generated LLVM IR is invalid");
    return err(errors);
  }
  module->setTargetTriple(target_.triple());

  return ok(Lowered{std::move(module), codegen.parallel()});
}

} // namespace tz
