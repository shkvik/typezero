#pragma once

#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <utility>
#include <vector>

namespace tz::parallel {

class Analysis;

using Escapes = llvm::DenseSet<const VarSymbol *>;

struct Pos {
  unsigned buffer = 0;
  unsigned offset = 0;
  bool operator<(const Pos &o) const;
};

struct AwaitInfo {
  FunctionId fn = kModuleFunction;
};

struct JoinInfo {
  FunctionId fn = kModuleFunction;
  Active segs;
};

struct Target {
  llvm::StringRef label;
  bool loop = false;
  bool block = false;
  Active breaks, continues;
};

struct OpenJoin {
  const AwaitExpr *await;
  Active active;
};

struct Entry {
  llvm::SMLoc first, last;
  bool open = false;
};

struct Partner {
  unsigned piece;
  std::vector<Loc> locs;
};

struct Decision {
  bool alone = true;
  std::vector<Partner> partners;
  llvm::SMLoc from, to;
  std::vector<llvm::SMLoc> ats;
  std::vector<WaitPlace> places;
  std::vector<llvm::SMLoc> waits;
  std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> inside;
};

}
