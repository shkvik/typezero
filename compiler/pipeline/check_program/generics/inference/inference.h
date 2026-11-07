#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

struct Inference {
  Inference(llvm::ArrayRef<const Type *> params, const AliasStore &aliases,
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
  const AliasStore &origins;
  std::function<const Type *(const Type *)> elementOf;
  std::function<const Type *(const Type *)> asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;

private:
  static const Type *paramsAsList(const Type *fn, size_t from);
  static const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);
};

}
