#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types_v2/text_patterns/types_text_patterns_registry_errors.h"

namespace tz {

class TypesTextPatternsRegistry {
public:
  explicit TypesTextPatternsRegistry(SemanticState &owner);

  void finishTemplate();
  void evalTemplate(const EvalTemplate &task);
  void startTemplate(const StartTemplate &task);
  void evalStringCase(const EvalStringCase &task);
  void afterTemplateHole(const AfterTemplateHole &task);

private:
  struct TemplatePart {
    std::vector<std::u16string> texts;
    std::vector<const Type *> holes;
  };
  using ResultCountAllowed = Result<size_t, TypesTextPatternsRegistryStringCountError>;
  using ResultHoleAccepted = Result<const Type *, TypesTextPatternsRegistryHoleTypeError>;

  void nextTemplateHole(TemplateDraft draft);
  void reportTooManyStrings(llvm::StringRef message);
  void finishTemplateHoles(const TemplateDraft &draft);
  void appendPattern(TemplatePart &part, const Type *pattern) const;


  bool isTemplateHole(const Type *type) const;


  const Type *firstNonHole(const Type *hole) const;
  const Type *partType(const TemplatePart &part) const;
  const Type *memberInCase(TypeOp op, const Type *member) const;
  const Type *patternInCase(TypeOp op, const Type *pattern) const;


  std::u16string changeCase(TypeOp op, const std::u16string &text) const;
  std::u16string unitInCase(TypeOp op, char16_t unit, bool isFirst) const;


  ResultHoleAccepted checkHole(const Type *hole) const;
  ResultCountAllowed checkStringCount(size_t count) const;
  std::optional<TemplatePart> partWithMember(const TemplatePart &part, const Type *member) const;
  std::optional<std::vector<TemplatePart>> grownParts(const std::vector<TemplatePart> &parts,
                                                      llvm::ArrayRef<const Type *> members,
                                                      const std::u16string &after);

  SemanticState &state;
};

}
