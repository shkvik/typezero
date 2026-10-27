#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params);

const Type *paramsAsList(const Type *fn, size_t from);

const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);

struct Inference {
  using Origins =
      llvm::DenseMap<const Type *, std::pair<TypeAliasDecl *, std::vector<const Type *>>>;
  Inference(llvm::ArrayRef<const Type *> params, const Origins &origins,
            std::function<const Type *(const Type *)> elementOf);

  void from(const Type *pattern, const Type *actual);

  void fromHeld(const Type *pattern, const Type *actual);

  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound);

  void retry(const std::vector<const Type *> &bound);

  llvm::ArrayRef<const Type *> params;
  std::vector<std::vector<const Type *>> candidates, weakCandidates, literalCandidates;
  std::vector<bool> undefinedSeen;
  std::set<std::pair<const Type *, const Type *>> visited;
  bool weak = false;
  bool intLiteral = false;
  const Origins &origins;
  std::function<const Type *(const Type *)> elementOf;
  std::function<const Type *(const Type *)> asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

}
