#pragma once

#include "compiler/pipeline/check_program/facts/facts.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

struct FieldPath {
  const VarSymbol *var = nullptr;
  llvm::SmallVector<llvm::StringRef, 2> names;
};

bool operator==(const FieldPath &left, const FieldPath &right);

struct EmitterKey {
  FieldPath path;
  std::string text;
  const Expr *root = nullptr;
};

bool operator==(const EmitterKey &left, const EmitterKey &right);

struct Flow {
  Facts facts;
  llvm::SmallPtrSet<const VarSymbol *, 4> defined;
  llvm::SmallPtrSet<const VarSymbol *, 2> undefinedVars;
  std::vector<FieldPath> definedFields;
  std::vector<std::pair<FieldPath, const Type *>> narrowedFields;
  std::vector<std::pair<const VarSymbol *, const Type *>> narrowed;
  llvm::SmallPtrSet<const VarSymbol *, 2> okResults, errResults;
  std::vector<llvm::StringRef> setFields;
  std::vector<EmitterKey> errorListened;
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> errorPending;
  bool beforeSuper = false;
  bool unreachable = false;
};

struct LoopScan {
  struct Change {
    unsigned count = 0;
    int64_t delta = 0;
    bool nested = false;
  };
  llvm::StringMap<Change> changes;
  bool callsFunctions = false;
  bool growsArrays = false;
  bool shrinksArrays = false;
  llvm::StringSet<> fieldWrites;
  bool releasesElements = false;
  bool breaks = false;
  bool awaits = false;
  unsigned functionDepth = 0;
  unsigned switches = 0;
  llvm::SmallVector<llvm::StringRef, 2> innerLabels;
};

enum class TruthUse { Condition, Not, Or, And, OrAssign, AndAssign };

struct NarrowTarget {
  const VarSymbol *var = nullptr;
  std::optional<FieldPath> path;
  const Type *declared = nullptr;
};

struct PredicateSide {
  bool unreachable = false;
  std::vector<std::pair<int, const Type *>> narrowed;
  std::vector<int> defined;
  std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>> fields;
  struct NarrowedField {
    int param;
    llvm::SmallVector<llvm::StringRef, 2> names;
    const Type *type;
  };
  std::vector<NarrowedField> narrowedFields;
  std::vector<Facts::Fact> facts;
};

struct Predicate {
  PredicateSide whenTrue, whenFalse;
  std::vector<const VarSymbol *> params;
  const VarSymbol *self = nullptr;
};

struct PredicateBuild {
  const FuncSymbol *fn;
  const FunctionDecl *decl;
  Flow whenTrue, whenFalse;
};

struct JumpTarget {
  enum class Kind { Loop, Switch, Block };
  Kind kind;
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Flow breaks;
};

struct BlockNames {
  size_t functions;
  std::vector<llvm::StringRef> names;
};

}
