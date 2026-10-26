#pragma once

#include "compiler/memory/cycle_scanner/cycle_scanner.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include <map>
#include <vector>

namespace tz {

class CycleFinder {
public:
  CycleFinder(MemoryPlan &plan, const CycleScanner &scan, const SourceFile &file);

  void run();
  const std::vector<const Type *> &runtimeTypesOf(const Type *type);
  void refers(const Type *type, std::vector<unsigned> &out);
  unsigned fnNodeOf(const Type *type);
  int nodeOf(CycleGraph::Kind kind, const void *key) const;

  static const Type *representationOf(const Type *type);
  static const Type *valueTypeOf(const VarSymbol *var);

  CycleGraph graph;

private:
  void buildGraph();
  std::vector<bool> cyclicNodes() const;
  void markCyclic(const std::vector<bool> &cyclic);
  void markFunctions();
  void edges(unsigned from, const Type *type, bool late);
  void captureEdges(unsigned from, const VarSymbol *var);
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

}
