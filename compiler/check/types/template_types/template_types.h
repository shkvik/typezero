#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TemplateTypes {
public:
  explicit TemplateTypes(Checker &owner);

  const Type *resolveTemplate(const TypeRef &ref);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  const Type *evalStringCase(TypeOp op, const Type *type);
  bool matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                     Checker::InferMatch *inference);

private:
  static std::string utf8(const std::u16string &text);
  static std::u16string utf16(llvm::StringRef text);
  static std::vector<const Type *> distributed(const Type *type);
  static bool templateHole(const Type *type);
  static std::u16string upperCase(char16_t c);
  static std::u16string lowerCase(char16_t c);
  static std::u16string changeCase(TypeOp op, const std::u16string &text);

  Checker &checker;
  llvm::SMLoc templateLoc;
};

}
