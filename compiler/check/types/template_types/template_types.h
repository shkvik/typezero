#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Template literal types and the intrinsic string types.
class TemplateTypes {
public:
  explicit TemplateTypes(Checker &checker);

  // `` `on${K}` ``, and `A & B` (only `K & string` and `T & {}`).
  const Type *resolveTemplate(const TypeRef &ref);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  const Type *evalStringCase(TypeOp op, const Type *type);
  bool matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                     Checker::InferMatch *inference);

private:
  Checker &checker_;
  // The template literal type being resolved, for an error of what it makes.
  llvm::SMLoc templateLoc_;
};

} // namespace tz
