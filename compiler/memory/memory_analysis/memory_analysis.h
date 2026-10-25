#pragma once

#include "compiler/memory/memory_plan/memory_plan.h"
#include "compiler/memory/owner_scanner/owner_scanner.h"
#include <vector>

namespace tz {

class CycleScanner;

// Which types may form cycles and which values a group holds (docs/memory.md), into a plan.
class MemoryAnalysis {
public:
  explicit MemoryAnalysis(MemoryPlan &plan);

  // Over the whole program, the graph of "a slot of this type may hold an object of that one"
  // (fields, elements, entries, captures, boxes, subtypes) tells which types may form a cycle;
  // their objects are made as members of groups (runtime/core/group/group.c). References that
  // may point at a member are counted through the group, and stores into a member join groups;
  // everything else is counted as before.
  void analyzeCycles(SourceFile &file);
  // Layer 1: the allocations whose objects are never stored (StoreScanner).
  void findUnstored(SourceFile &file);

private:
  // What the analysis needs of the code of the program.
  void scanProgram(SourceFile &file, CycleScanner &scan);
  // Layer 2: the references of functions in owned objects to their owners that need no count.
  std::vector<OwnerScanner::BackRef> findBackRefs(SourceFile &file, OwnerScanner &owners);
  // The globals that hold their values for good: their groups are eternal.
  void findEternals(SourceFile &file);

  MemoryPlan &plan_;
};

} // namespace tz
