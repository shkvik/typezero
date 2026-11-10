#include "compiler/interprocedural/interprocedural_external.h"
#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker.h"
#include "compiler/interprocedural/load_order/load_order.h"
#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"
#include "compiler/interprocedural/pool_checker/pool_checker.h"
#include "compiler/interprocedural/unused/unused.h"
#include "compiler/interprocedural/wide_writes/wide_writes.h"
#include "compiler/program/program/program.h"

namespace tz {

InterproceduralExternalFacade::InterproceduralExternalFacade(Program &checked)
    : program(checked),
      library(LibraryRanges::collect(checked.file, checked.sourceMgr)) {}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkReadsBeforeInit() {
  LoadOrder order(this->program.diags);
  order.checkLoadOrder(this->program.file);

  return this->program.diags.take();
}

void InterproceduralExternalFacade::markUnreachableFunctions() {
  Unused::markUnusedFunctions(this->program.file);
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkAsyncLocalStorages() {
  AsyncLocalStorageChecker storages(this->program, this->library);
  storages.run();

  return this->program.diags.take();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkPoolObjects() {
  PoolChecker pools(this->program, this->library);
  pools.run();

  return this->program.diags.take();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkWideWrites() {
  WideWrites writes(this->program, this->library);
  writes.checkWidenings();

  return this->program.diags.take();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkOverflows() {
  checkIntOverflow(this->program.file, this->program.diags);

  return this->program.diags.take();
}

}
