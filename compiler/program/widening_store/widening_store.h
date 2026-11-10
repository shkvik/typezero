#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <utility>

namespace tz {

class WideningStore {
public:
  void rememberWideningCast(const CastExpr *cast);
  bool isWideningCast(const CastExpr *cast) const;
  bool hasWideningCasts() const;
  void markFieldsNarrowed();
  bool hasNarrowedFields() const;
  void rememberWidening(const Type *from, const Type *to, bool narrows);
  bool hasKnownWidening(const Type *from, const Type *to) const;
  bool isNarrowingWidening(const Type *from, const Type *to) const;

private:
  llvm::DenseSet<const CastExpr *> wideningCasts;
  bool fieldsNarrowed = false;
  std::map<std::pair<const Type *, const Type *>, bool> narrowingWidenings;
};

}
