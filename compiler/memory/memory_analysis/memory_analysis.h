#pragma once

#include "compiler/memory/memory_plan/memory_plan.h"
#include "compiler/memory/owner_scanner/owner_scanner.h"
#include <vector>

namespace tz {

class CycleScanner;

class MemoryAnalysis {
public:
  explicit MemoryAnalysis(MemoryPlan &plan);

  void analyzeCycles(SourceFile &file);
  void findUnstored(SourceFile &file);

private:
  void scanProgram(SourceFile &file, CycleScanner &scan);
  std::vector<OwnerScanner::BackRef> findBackRefs(SourceFile &file, OwnerScanner &owners);
  void findEternals(SourceFile &file);

  MemoryPlan &plan_;
};

}
