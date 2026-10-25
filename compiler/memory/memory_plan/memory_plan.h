#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz {

// What MemoryPass found of the memory of the program (docs/objects.md, section 11): which types
// may form cycles, so that their objects are made as members of groups, and what is proved of
// the others; what lowering asks of it.
struct MemoryPlan {
  // Objects of this object, array or dictionary type are made as members.
  bool isCyclic(const Type *type) const { return cyclicTypes.contains(type); }
  // The same for the environments of closures (by the key of their layout) and the boxes of
  // variables of a type.
  bool isCyclicEnv(const void *key) const { return cyclicEnvs.contains(key); }
  bool isCyclicBox(const Type *type) const { return cyclicBoxes.contains(type); }
  // Objects of this type may have weak references (WeakRef<T> of it or of a type up its chain),
  // which their drop forgets.
  bool isWeakTarget(const Type *type) const { return weakTargets.contains(type); }
  // Whether the allocation `site` of `type` makes a member: the type may form cycles, and what
  // it makes may be stored somewhere (layer 1, see StoreScanner).
  bool makesMember(const Expr *site, const Type *type) const {
    return isCyclic(type) && !unstoredSites.contains(site);
  }
  // A global that is never assigned again holds its value for good: the groups in it are
  // eternal (tz_group_pin_value), and at the end of a program checked for leaks it lets them go
  // (tz_group_end_value).
  bool isEternal(const VarSymbol *var) const { return eternals.contains(var); }
  // A reference of this type may point at a member; a value of this type may hold such a
  // reference.
  bool refMayBeMember(const Type *type) const;
  bool mayHoldMember(const Type *type) const;
  // A promise of `value` is a member of a group: what it settles with may be one (its value, or
  // its error when an Error may be), see runtime/core/async/async.c.
  bool promiseIsMember(const Type *value) const;

  bool hasCycles = false;       // some type, environment or box is cyclic
  bool anyCyclicObject = false; // some object or dictionary type is
  bool fnMayBeMember = false;   // a function value's environment may be a member
  llvm::DenseSet<const Type *> cyclicTypes, cyclicBoxes, weakTargets;
  llvm::DenseSet<const void *> cyclicEnvs;
  llvm::DenseSet<const VarSymbol *> eternals;
  // Representations of cyclic array types, which generic code may see.
  llvm::DenseSet<const Type *> cyclicArrayReps;
  // Layer 1: the allocations whose objects are never stored.
  llvm::DenseSet<const Expr *> unstoredSites;
  // Layer 2: the captures of closures (by the key of their environment) that are not counted,
  // as the closure lives in an object its owner made and holds alone; and those fields of the
  // owners, by type and name.
  std::set<std::pair<const void *, const VarSymbol *>> uncounted;
  std::set<std::pair<const Type *, std::string>> ownedFields, backRefFields;
  std::vector<const Type *> subtypes; // see SourceFile::subtypes
  mutable llvm::DenseMap<const Type *, bool> refMember;
};

} // namespace tz
