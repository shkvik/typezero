#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Which types may form cycles and which values a group holds (docs/memory.md).
class MemoryAnalysis {
public:
  explicit MemoryAnalysis(CodeGen &codegen);

  // Cycles (codegen_cycles.cpp, docs/objects.md section 11). Over the whole
  // program, the graph of "a slot of this type may hold an object of that
  // one" (fields, elements, entries, captures, boxes, subtypes) tells which
  // types may form a cycle; their objects are made as members of groups
  // (runtime/core/group/group.c). References that may point at a member are
  // counted through the group, and stores into a member join groups; everything
  // else is counted as before.
  void analyzeCycles(SourceFile &file);
  // Layer 1: the allocations whose objects are never stored (StoreScanner).
  void findUnstored(SourceFile &file);
  // A reference of this type may point at a member; a value of this type
  // may hold such a reference.
  bool refMayBeMember(const Type *type);
  bool mayHoldMember(const Type *type);

private:
  CodeGen &codegen_;
  // Representations of cyclic array types, which generic code may see.
  llvm::DenseSet<const Type *> cyclicArrayReps_;
  llvm::DenseMap<const Type *, bool> refMember_;
  bool fnMayBeMember_ = false;                     // a function value's environment may be a member
};

} // namespace tz
