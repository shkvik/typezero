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

struct MemoryPlan {
  bool isCyclic(const Type *type) const { return cyclicTypes.contains(type); }
  bool isCyclicEnv(const void *key) const { return cyclicEnvs.contains(key); }
  bool isCyclicBox(const Type *type) const { return cyclicBoxes.contains(type); }
  bool isWeakTarget(const Type *type) const { return weakTargets.contains(type); }
  bool makesMember(const Expr *site, const Type *type) const {
    return isCyclic(type) && !unstoredSites.contains(site);
  }
  bool isEternal(const VarSymbol *var) const { return eternals.contains(var); }
  bool refMayBeMember(const Type *type) const;
  bool mayHoldMember(const Type *type) const;
  bool promiseIsMember(const Type *value) const;

  bool hasCycles = false;
  bool anyCyclicObject = false;
  bool fnMayBeMember = false;
  llvm::DenseSet<const Type *> cyclicTypes, cyclicBoxes, weakTargets;
  llvm::DenseSet<const void *> cyclicEnvs;
  llvm::DenseSet<const VarSymbol *> eternals;
  llvm::DenseSet<const Type *> cyclicArrayReps;
  llvm::DenseSet<const Expr *> unstoredSites;
  std::set<std::pair<const void *, const VarSymbol *>> uncounted;
  std::set<std::pair<const Type *, std::string>> ownedFields, backRefFields;
  std::vector<const Type *> subtypes;
  mutable llvm::DenseMap<const Type *, bool> refMember;
};

}
