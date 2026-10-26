#pragma once

#include "compiler/memory/cycle_finder/cycle_finder.h"
#include <utility>
#include <vector>

namespace tz {

class HeldMembers {
public:
  HeldMembers(MemoryPlan &plan, const CycleScanner &scan, CycleFinder &finder);

  void run();

private:
  bool isMemberNode(unsigned v) const;
  bool passesOn(unsigned v) const;
  void findHeld();
  bool isHeld(CycleGraph::Kind kind, const void *key) const;
  bool add(llvm::DenseSet<const Type *> &set, const Type *t);
  bool addType(const Type *t);

  MemoryPlan &plan_;
  const CycleScanner &scan_;
  CycleFinder &finder_;
  std::vector<unsigned> callbacks_;
  std::vector<std::pair<const Type *, std::vector<unsigned>>> promised_;
  std::vector<bool> held_;
};

}
