#include "compiler/lower/lower_pass/lower_pass.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/optimize/target/target.h"
#include "compiler/program/module/module.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

LowerPass::LowerPass(llvm::LLVMContext &context, const Target &target, llvm::StringRef moduleName,
                     const LowererRegistry &lowerers, DebugLevel debug, bool optimize)
    : context(context),
      target(target),
      moduleName(moduleName.str()),
      lowerers(lowerers),
      debug(debug),
      optimize(optimize) {}

Result<Lowered, Errors> LowerPass::run(Program &program, const ParallelPlan &plan,
                                       const MemoryPlan &memory) const {
  llvm::DataLayout layout = this->target.machine().createDataLayout();
  Lowering lowering(this->context, this->moduleName, layout);
  lowering.setSourceMgr(&program.sourceMgr);
  lowering.setParallelPlan(&plan);
  lowering.setMemoryPlan(&memory);
  lowering.setLowerers(&this->lowerers);
  lowering.setContexts(llvm::any_of(program.file.modules, [](const Module *m) {
    return m->builtin && m->name == "node:async_hooks";
  }));
  lowering.setDebug(this->debug, this->optimize, this->target.triple().isOSWindows());
  std::unique_ptr<llvm::Module> module = lowering.moduleBuilder().generate(program.file);
  std::string problems = lowering.mismatch();
  llvm::raw_string_ostream out(problems);
  if (!problems.empty() || llvm::verifyModule(*module, &out)) {
    for (llvm::Function &function : *module) {
      if (!function.isDeclaration() && llvm::verifyFunction(function)) {
        out << "in " << function.getName() << "\n";
      }
    }
    Errors errors;
    errors.raw(problems);
    errors.tool("internal error: the generated LLVM IR is invalid");
    return err(errors);
  }
  module->setTargetTriple(this->target.triple());

  return ok(Lowered{std::move(module), lowering.parallel()});
}
}
