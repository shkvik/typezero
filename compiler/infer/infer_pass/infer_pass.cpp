#include "compiler/infer/infer_pass/infer_pass.h"
#include "compiler/check/checker/checker.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/program/program/program.h"

namespace tz {

Errors InferPass::run(Program &program) const {
  Checker &checker = *program.checker;
  auto cycleCallee = [&](FunctionDecl *fn, llvm::StringRef name) {
    return checker.importBinder().cycleCallee(fn, name);
  };
  checker.callCycles().findCycles(checker.declaredFunctions_, cycleCallee);

  return program.diags->take();
}

}
