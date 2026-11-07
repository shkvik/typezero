#include "compiler/pipeline/check_program/infer/infer_pass/infer_pass.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/declare/import_binder/import_binder.h"
#include "compiler/pipeline/check_program/infer/call_cycles/call_cycles.h"

namespace tz {

InferPass::InferPass(CheckState &owner)
    : state(owner) {}

Errors InferPass::run() const {
  auto cycleCallee = [&](FunctionDecl *fn, llvm::StringRef name) {
    return ImportBinder(this->state).cycleCallee(fn, name);
  };
  std::vector<FunctionDecl *> declared = this->state.getReturnInference().getDeclaredFunctions();
  CallCycles(this->state).findCycles(declared, cycleCallee);

  return this->state.getDiagnostics().take();
}

}
