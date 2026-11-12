#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/template_types/template_types_errors.h"

namespace tz {

class SemanticParts;
class TypeResolver;
class TypeOperators;
class ExtendsRelation;

class TemplateTypes {
public:
  TemplateTypes(SemanticState &owner, SemanticParts &all);

  const Type *resolveTemplate(const TypeRef &ref);
  const Type *evalStringCase(TypeOp op, const Type *type);
  const Type *evalTemplate(llvm::ArrayRef<const Type *> operands);
  bool isMatchingTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                          InferMatch *inference);

private:
  struct TemplatePart {
    std::vector<std::u16string> texts;
    std::vector<const Type *> holes;
  };

  using ResultHoleAccepted = Result<const Type *, TemplateTypesHoleTypeError>;
  using ResultCountAllowed = Result<size_t, TemplateTypesStringCountError>;

  ResultHoleAccepted checkHole(const Type *hole) const;
  ResultCountAllowed checkStringCount(size_t count) const;

  bool isTemplateHole(const Type *type) const;
  const Type *resolveHole(const TypeRef &arg);
  std::u16string upperCase(char16_t unit) const;
  std::u16string lowerCase(char16_t unit) const;
  const Type *numberLiteral(double value) const;
  void reportTooManyStrings(llvm::StringRef message);
  const Type *firstNonHole(const Type *hole) const;
  const Type *partType(const TemplatePart &part) const;
  const Type *memberInCase(TypeOp op, const Type *member) const;
  const Type *patternInCase(TypeOp op, const Type *pattern) const;
  void appendPattern(TemplatePart &part, const Type *pattern) const;
  bool isNumberHole(const Type *hole, const InferMatch *inference) const;
  std::u16string changeCase(TypeOp op, const std::u16string &text) const;
  std::u16string unitInCase(TypeOp op, char16_t unit, bool isFirst) const;
  bool isPieceFittingHole(const std::u16string &piece, const Type *hole, InferMatch *inference);
  std::optional<TemplatePart> partWithMember(const TemplatePart &part, const Type *member) const;
  const Type *pieceType(const std::u16string &piece, const Type *hole,
                        const InferMatch *inference) const;
  std::optional<std::vector<TemplatePart>> grownParts(const std::vector<TemplatePart> &parts,
                                                      llvm::ArrayRef<const Type *> members,
                                                      const std::u16string &after);

  SemanticState &state;
  TypeResolver *typeResolver = nullptr;
  TypeOperators *typeOperators = nullptr;
  ExtendsRelation *extendsRelation = nullptr;
};

}
