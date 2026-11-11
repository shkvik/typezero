#pragma once

#include "compiler/item_list/item_list/item_list.h"
#include "compiler/program/diagnostic/diagnostic.h"
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
  Program &program;
  LibraryRanges library;
  bool overflow = false;
};

}
