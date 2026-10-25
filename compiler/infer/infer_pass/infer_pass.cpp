#include "compiler/infer/infer_pass/infer_pass.h"
#include "compiler/check/sema/sema.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

Errors InferPass::run(Program &program) const {
  Sema &sema = *program.checker;
  sema.callCycles().findCycles(sema.declaredFunctions_,
             [&](FunctionDecl *fn, llvm::StringRef name) { return sema.importBinder().cycleCallee(fn, name); });

  return program.diags->take();
}

} // namespace tz
