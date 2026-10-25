#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Template literal types and the intrinsic string types.
class TemplateTypes {
public:
  explicit TemplateTypes(Sema &sema);

  // `` `on${K}` ``, and `A & B` (only `K & string` and `T & {}`).
  const Type *resolveTemplate(const TypeRef &ref);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  const Type *evalStringCase(TypeOp op, const Type *type);
  bool matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                     Sema::InferMatch *inference);

private:
  Sema &sema_;
  // The template literal type being resolved, for an error of what it makes.
  llvm::SMLoc templateLoc_;
};

} // namespace tz
