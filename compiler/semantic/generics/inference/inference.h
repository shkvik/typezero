#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

class Inference {
public:
  using TypeOf = std::function<const Type *(const Type *)>;

  Inference(llvm::ArrayRef<const Type *> typeParams, const AliasStore &aliases,
            TypeOf elementOfType);

  bool hasDeferred() const;
  void setAsyncOf(TypeOf asyncOfType);
  bool hasSeenUndefined(size_t k) const;
  void from(const Type *pattern, const Type *actual);
  void retry(const std::vector<const Type *> &bound);
  void fromHeld(const Type *pattern, const Type *actual);
  void adoptCandidates(size_t k, const Inference &other);
  void markArgument(bool weakArgument, bool intLiteralArgument);
  const std::vector<const Type *> &getCandidates(size_t k) const;
  const std::vector<const Type *> &getWeakCandidates(size_t k) const;
  const std::vector<const Type *> &getLiteralCandidates(size_t k) const;
  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound);

private:
  static const Type *paramsAsList(const Type *fn, size_t from);
  static const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);

  llvm::ArrayRef<const Type *> params;
  std::vector<std::vector<const Type *>> candidates, weakCandidates, literalCandidates;
  std::vector<bool> undefinedSeen;
  std::set<std::pair<const Type *, const Type *>> visited;
  bool weak = false;
  bool intLiteral = false;
  const AliasStore &origins;
  TypeOf elementOf;
  TypeOf asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

}
