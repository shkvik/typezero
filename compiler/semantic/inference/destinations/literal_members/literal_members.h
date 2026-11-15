#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include <optional>

namespace tz {

class SemanticState;

class LiteralMembers {
public:
  explicit LiteralMembers(SemanticState &owner);

  const Type *literalMember(const ObjectLitExpr *literal, const Type *target) const;

private:
  bool isLiteralSet(const Type *type) const;
  bool isPlainObjectMember(const Type *member) const;
  bool hasObjectSpread(const ObjectLitExpr *literal) const;
  bool isTagConvertible(const Type *tag, const Type *fieldType) const;
  bool hasProperty(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool hasFittingNames(const ObjectLitExpr *literal, const Type *member) const;


  const Type *enumTag(const Expr *expr) const;
  const Type *knownLiteralSet(const Expr *value) const;
  const Type *propertyTag(const ObjectLitExpr::Property &property, const Field *field) const;


  std::optional<int> memberScore(const ObjectLitExpr *literal, const Type *member) const;
  std::optional<int> candidateScore(const ObjectLitExpr *literal, const Type *member) const;
  std::optional<int> propertyScore(const ObjectLitExpr::Property &property,
                                   const Type *member) const;


  const VarSymbol *namedVariable(const Expr *expr) const;

  SemanticState &state;
};

}
