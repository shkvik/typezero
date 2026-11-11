#include "compiler/interprocedural/interprocedural_external.h"
#include "compiler/interprocedural/async_local_storage_checker/async_local_storage_checker.h"
#include "compiler/interprocedural/load_order/load_order.h"
#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"
#include "compiler/interprocedural/pool_checker/pool_checker.h"
#include "compiler/interprocedural/unused_functions/unused_functions.h"
#include "compiler/interprocedural/wide_writes/wide_writes.h"
#include "compiler/program/program/program.h"

namespace tz {

InterproceduralExternalFacade::InterproceduralExternalFacade(Program &checked, bool checksOverflow)
    : program(checked),
      library(LibraryRanges::collect(checked.file, checked.sourceMgr)),
      overflow(checksOverflow) {}

InterproceduralExternalFacade::ResultProgramChecked InterproceduralExternalFacade::run() {
  auto found = ItemList<Diagnostic>();

  auto loadOrder = LoadOrder(this->program.sourceMgr);
  auto earlyReads = loadOrder.checkLoadOrder(this->program.file);
  if (earlyReads.isErr()) {
    found.append(earlyReads.error());
  }

  auto unusedFunctions = UnusedFunctions(this->program.file);
  this->program.file.unusedFunctions = unusedFunctions.findUnused();

  auto asyncLocalStorageChecker = AsyncLocalStorageChecker(this->program, this->library);
  auto storages = asyncLocalStorageChecker.run();
  if (storages.isErr()) {
    found.append(storages.error());
  }

  auto poolChecker = PoolChecker(this->program, this->library);
  auto poolObjects = poolChecker.run();
  if (poolObjects.isErr()) {
    found.append(poolObjects.error());
  }

  auto wideWrites = WideWrites(this->program, this->library);
  auto widenings = wideWrites.checkWidenings();
  if (widenings.isErr()) {
    found.append(widenings.error());
  }

  if (this->overflow) {
    auto overflowCheck = OverflowCheck(this->program, this->library);
    auto overflows = overflowCheck.run();
    if (overflows.isErr()) {
      found.append(overflows.error());
    }
  }
  if (!found.empty()) {
    return err(found);
  }

  return ok();
}

}
