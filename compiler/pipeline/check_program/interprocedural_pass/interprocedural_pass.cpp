#include "compiler/pipeline/check_program/interprocedural_pass/interprocedural_pass.h"
#include "compiler/interprocedural/context_checker/context_checker.h"
#include "compiler/interprocedural/load_order/load_order.h"
#include "compiler/interprocedural/pool_checker/pool_checker.h"
#include "compiler/interprocedural/unused/unused.h"
#include "compiler/interprocedural/wide_writes/wide_writes.h"
#include "compiler/program/program/program.h"
#include "compiler/semantic/semantic_external.h"

namespace tz {

InterproceduralPass::InterproceduralPass(SemanticExternalFacade &owner, Program &checked)
    : engine(owner),
      program(checked) {}

void InterproceduralPass::checkLoadOrder() const {
  SourceFile &file = this->program.file;
  LoadOrder(this->engine.getDiagnostics()).checkLoadOrder(file);
  Unused::findUnused(file);
}

Errors InterproceduralPass::checkValueFlows() const {
  SourceFile &file = this->program.file;
  ContextChecker(this->program).run();
  PoolChecker(this->program).run();
  if (!this->engine.getDiagnostics().hasErrors()) {
    WideWrites(this->engine).checkWidenings(file);
  }

  return this->program.diags->take();
}

}
