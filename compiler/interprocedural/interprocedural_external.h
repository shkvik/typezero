#pragma once

#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/library_ranges/library_ranges.h"

namespace tz {

struct Program;

class InterproceduralExternalFacade {
public:
  explicit InterproceduralExternalFacade(Program &checked);

  Diagnostics::ResultTaken checkReadsBeforeInit();
  void markUnreachableFunctions();
  Diagnostics::ResultTaken checkAsyncLocalStorages();
  Diagnostics::ResultTaken checkPoolObjects();
  Diagnostics::ResultTaken checkWideWrites();
  Diagnostics::ResultTaken checkOverflows();

private:
  Program &program;
  LibraryRanges library;
};

}
