#pragma once

#include "compiler/memory/cycle_scanner/cycle_scanner.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include <map>
#include <vector>

namespace tz {

// Which types, environments and boxes may form cycles of references (docs/objects.md, section
// 11): over the whole program, the graph of "a slot of this may hold that" (fields, elements,
// entries, captures, boxes, subtypes), whose strongly connected components with an edge that may
// be set late are the cycles.
class CycleFinder {
public:
  CycleFinder(MemoryPlan &plan, const CycleScanner &scan, const SourceFile &file);

  // A round: the cyclic types, environments, boxes and function types of the plan made again,
  // from a new graph without the references that the plan says need no count.
  void run();
  // The object types a reference of `type` may point at: itself, the types declared to be it,
  // and those code sees as it (views).
  const std::vector<const Type *> &runtimeTypesOf(const Type *type);
  // The nodes a value of `type` may refer to.
  void refers(const Type *type, std::vector<unsigned> &out);
  unsigned fnNodeOf(const Type *type);
  // The node of `key`, or -1.
  int nodeOf(CycleGraph::Kind kind, const void *key) const;

  // The type a generic copy sees a value of `type` as (Checker::representation).
  static const Type *representationOf(const Type *type);
  // The type of the value of a variable: its own, or that of the function it names.
  static const Type *valueTypeOf(const VarSymbol *var);

  CycleGraph graph;

private:
  void buildGraph();
  // Whether each node is in a cyclic component.
  std::vector<bool> cyclicNodes() const;
  void markCyclic(const std::vector<bool> &cyclic);
  void markFunctions();
  void edges(unsigned from, const Type *type, bool late);
  void captureEdges(unsigned from, const VarSymbol *var);
  // An array or dictionary type is added to when it, or how generic code sees it, is.
  bool isMutated(const Type *type) const;
  static const Type *containerRepresentation(const Type *type);

  MemoryPlan &plan_;
  const CycleScanner &scan_;
  const SourceFile &file_;
  std::vector<const Type *> objects_, dicts_;
  std::map<const Type *, std::vector<const Type *>> runtimeTypes_;
  const Type *anyFunction_ = nullptr;
  unsigned anyNode_ = 0, unknownNode_ = 0;
};

} // namespace tz
