#pragma once

#include "compiler/memory/cycle_finder/cycle_finder.h"
#include <utility>
#include <vector>

namespace tz {

// What may hold a member, and may be held by one, is one too: an object, array, dictionary,
// environment or box outside the groups that a member held, and that held a reference into a
// group, would count from outside it a reference that the group may own through it (an X that
// holds a T both in a field and in an array of them), and the group could then never be freed
// though nothing outside reaches it. One that no member holds stays outside: what it holds is
// held from outside indeed, and the groups stay as small as the cycles make them.
class HeldMembers {
public:
  HeldMembers(MemoryPlan &plan, const CycleScanner &scan, CycleFinder &finder);

  // Adds them to the plan, until there are no more.
  void run();

private:
  bool isMemberNode(unsigned v) const;
  // A function value, an object of any type or an unknown is the reference to what it holds:
  // what holds it holds that.
  bool passesOn(unsigned v) const;
  // The nodes a member holds.
  void findHeld();
  bool isHeld(CycleGraph::Kind kind, const void *key) const;
  // A type made a member; whether it was not one.
  bool add(llvm::DenseSet<const Type *> &set, const Type *t);
  // Whether an object, array or dictionary type joins the members.
  bool addType(const Type *t);

  MemoryPlan &plan_;
  const CycleScanner &scan_;
  CycleFinder &finder_;
  // The callbacks of reactions and what the promises of each value hold, found before the nodes
  // are counted (refers may make nodes).
  std::vector<unsigned> callbacks_;
  std::vector<std::pair<const Type *, std::vector<unsigned>>> promised_;
  std::vector<bool> held_;
};

} // namespace tz
