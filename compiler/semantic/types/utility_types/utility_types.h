#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/utility_types/utility_types_errors.h"

namespace tz {

class SemanticParts;
class Scopes;
class TypeParams;
class ExprChecker;
class TypeDeclarer;
class TypeOperators;
class ReturnInferrer;
class ConditionalTypes;
class ClassValueChecker;

class UtilityTypes {
public:
  using ResultUtilityApplied = Result<const Type *, UtilityTypesResolveUtilityTypeError>;
  using ResultGenericIndexed = Result<Expr *, UtilityTypesCheckGenericIndexError>;

  UtilityTypes(SemanticState &owner, SemanticParts &all);

  bool wantsLiteral(const Type *param) const;
  const Type *functionValueType(FuncSymbol *fn);
  ResultGenericIndexed checkGenericIndex(IndexExpr *index);
  ResultUtilityApplied resolveUtilityType(const TypeRef &ref);
  const Type *importedValueType(Module *from, llvm::StringRef name);

private:
  struct Utility {
    llvm::StringLiteral name;
    TypeOp op;
    int count;
  };

  static const Utility *utilityNamed(llvm::StringRef name);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeParams *typeParams = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeOperators *typeOperators = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
