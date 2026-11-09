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
    : program(checked),
      loadOrder(std::make_unique<LoadOrder>(*checked.diags)),
      wideWrites(std::make_unique<WideWrites>(owner, *checked.diags)),
      poolChecker(std::make_unique<PoolChecker>(checked)),
      contextChecker(std::make_unique<ContextChecker>(checked)) {}

InterproceduralPass::~InterproceduralPass() = default;

void InterproceduralPass::checkLoadOrder() {
  SourceFile &file = this->program.file;
  this->loadOrder->checkLoadOrder(file);
  Unused::findUnused(file);
}

Errors InterproceduralPass::checkValueFlows() {
  SourceFile &file = this->program.file;
  this->contextChecker->run();
  this->poolChecker->run();
  if (!this->program.diags->hasErrors()) {
    this->wideWrites->checkWidenings(file);
  }

  return this->program.diags->take();
}

}
