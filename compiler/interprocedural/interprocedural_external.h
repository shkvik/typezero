#pragma once

#include "compiler/item_list/item_list/item_list.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/library_ranges/library_ranges.h"
#include "compiler/result/result/result.h"

namespace tz {

struct Program;

class InterproceduralExternalFacade {
public:
  using ResultProgramChecked = Result<void, ItemList<Diagnostic>>;

  InterproceduralExternalFacade(Program &checked, bool checksOverflow);

  ResultProgramChecked run();

private:
  Diagnostics::ResultTaken checkReadsBeforeInit();
  void markUnreachableFunctions();
  Diagnostics::ResultTaken checkAsyncLocalStorages();
  Diagnostics::ResultTaken checkPoolObjects();
  Diagnostics::ResultTaken checkWideWrites();
  Diagnostics::ResultTaken checkOverflows();

  Program &program;
  LibraryRanges library;
  bool overflow = false;
};

}
