#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TemplateTypes {
public:
  explicit TemplateTypes(Checker &checker);

  const Type *resolveTemplate(const TypeRef &ref);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  const Type *evalStringCase(TypeOp op, const Type *type);
  bool matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                     Checker::InferMatch *inference);

private:
  Checker &checker_;
  llvm::SMLoc templateLoc_;
};

}
