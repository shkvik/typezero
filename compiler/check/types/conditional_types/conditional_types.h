#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConditionalTypes {
public:
  explicit ConditionalTypes(Checker &checker);

  const Type *resolveConditional(const TypeRef &ref);
  const Type *deferConditional(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool fitsConditional(const Type *from, const Type *to, unsigned depth = 0);
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  const Type *resolveIntersection(const TypeRef &ref);
  const Type *resolveTypeArgument(const TypeRef &ref);
  const Type *signatureForUtility(const TypeRef &ref);

private:
  Checker &checker_;
  std::map<const TypeRef *, Checker::ConditionalInfo> conditionals_;
  llvm::DenseMap<const Type *, const Checker::ConditionalInfo *> conditionalTags_;
};

}
