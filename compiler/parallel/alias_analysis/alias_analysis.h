#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

// Which references may be the same object, and which objects escape.
class AliasAnalysis {
public:
  explicit AliasAnalysis(Analysis &analysis);

  Ref refOf(const Expr *e);
  Ref receiverRef(const Expr *e);
  bool isAllocation(const Expr *e) const;
  bool isLocal(const VarSymbol *v);
  const Type *typeOfRef(Ref r) const;
  bool sameObject(Ref a, Ref b);
  bool typesMayAlias(const Type *a, const Type *b);
  bool objectTypesMayAlias(const Type *a, const Type *b);
  bool mayReach(Ref from, Ref to, bool innerOnly);
  const std::vector<const Type *> &inside(const Type *type);
  bool locsMayMeet(const Loc &a, const Loc &b);

private:
  Analysis &analysis_;
};

} // namespace tz::parallel
