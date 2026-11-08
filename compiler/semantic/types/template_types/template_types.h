#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/template_types/template_types_errors.h"

namespace tz {

class TemplateTypes {
public:
  explicit TemplateTypes(SemanticState &owner);

  const Type *resolveTemplate(const TypeRef &ref);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  const Type *evalStringCase(TypeOp op, const Type *type);
  bool matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                     InferMatch *inference);

private:
  using ResultHoleAccepted = Result<const Type *, TemplateTypesHoleTypeError>;
  using ResultCountAllowed = Result<size_t, TemplateTypesStringCountError>;

  static ResultHoleAccepted holeType(const Type *member);
  static ResultCountAllowed stringCount(size_t count);
  static std::u16string utf16(llvm::StringRef text);
  static std::vector<const Type *> distributed(const Type *type);
  static bool templateHole(const Type *type);
  static std::u16string upperCase(char16_t c);
  static std::u16string lowerCase(char16_t c);
  static std::u16string changeCase(TypeOp op, const std::u16string &text);

  SemanticState &state;
};

}
