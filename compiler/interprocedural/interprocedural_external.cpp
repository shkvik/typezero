#include "compiler/interprocedural/interprocedural_external.h"
#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker.h"
#include "compiler/interprocedural/load_order/load_order.h"
#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"
#include "compiler/interprocedural/pool_checker/pool_checker.h"
#include "compiler/interprocedural/unused/unused.h"
#include "compiler/interprocedural/wide_writes/wide_writes.h"
#include "compiler/program/program/program.h"

namespace tz {

InterproceduralExternalFacade::InterproceduralExternalFacade(Program &checked, bool checksOverflow)
    : program(checked),
      library(LibraryRanges::collect(checked.file, checked.sourceMgr)),
      overflow(checksOverflow) {}

InterproceduralExternalFacade::ResultProgramChecked InterproceduralExternalFacade::run() {
  ItemList<Diagnostic> found;

  auto earlyReads = this->checkReadsBeforeInit();
  if (earlyReads.isErr()) {
    found.append(earlyReads.error());
  }

  this->markUnreachableFunctions();

  auto storages = this->checkAsyncLocalStorages();
  if (storages.isErr()) {
    found.append(storages.error());
  }

  auto poolObjects = this->checkPoolObjects();
  if (poolObjects.isErr()) {
    found.append(poolObjects.error());
  }

  auto wideWrites = this->checkWideWrites();
  if (wideWrites.isErr()) {
    found.append(wideWrites.error());
  }

  if (this->overflow) {
    auto overflows = this->checkOverflows();
    if (overflows.isErr()) {
      found.append(overflows.error());
    }
  }
  if (!found.empty()) {
    return err(found);
  }

  return ok();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkReadsBeforeInit() {
  LoadOrder order(this->program.sourceMgr);

  return order.checkLoadOrder(this->program.file);
}

void InterproceduralExternalFacade::markUnreachableFunctions() {
  Unused::markUnusedFunctions(this->program.file);
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkAsyncLocalStorages() {
  AsyncLocalStorageChecker storages(this->program, this->library);

  return storages.run();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkPoolObjects() {
  PoolChecker pools(this->program, this->library);

  return pools.run();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkWideWrites() {
  WideWrites writes(this->program, this->library);

  return writes.checkWidenings();
}

Diagnostics::ResultTaken InterproceduralExternalFacade::checkOverflows() {
  OverflowCheck overflows(this->program, this->library);

  return overflows.run();
}

}
