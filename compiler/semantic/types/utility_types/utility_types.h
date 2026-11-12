#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
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
  using ResultUtilityAllowed = Result<void, UtilityTypesResolveUtilityTypeError>;

  UtilityTypes(SemanticState &owner, SemanticParts &all);

  bool isExpectingLiteral(const Type *param) const;
  const Type *functionValueType(FuncSymbol *function);
  ResultGenericIndexed checkGenericIndex(IndexExpr *index);
  ResultUtilityApplied resolveUtilityType(const TypeRef &ref);
  const Type *importedValueType(Module *from, llvm::StringRef name);

private:
  bool isGenericKey(const Type *key) const;
  bool isGenericKeyName(const NameExpr *keyName) const;
  const Type *classDeclType(ClassDecl *classDecl);
  const Type *functionDeclType(FunctionDecl *functionDecl);
  const Type *firstNonString(const Type *operand) const;
  bool hasDeclarator(const VarDeclStmt *varDecl, llvm::StringRef name) const;
  const Type *varDeclType(const VarDeclStmt *varDecl, llvm::StringRef name) const;
  const VarDeclarator *declaratorNamed(const VarDeclStmt *varDecl, llvm::StringRef name) const;
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  const UtilityTypeName *findUtility(llvm::StringRef name) const;
  ResultUtilityAllowed checkTextOperand(const TypeRef &ref, const Type *operand) const;
  ResultUtilityAllowed checkFunctionOperand(const TypeRef &ref, const Type *operand) const;
  std::vector<const Type *> utilityOperands(const TypeRef &ref, const UtilityTypeName &utility);
  ResultUtilityAllowed checkArgumentCount(const TypeRef &ref, const UtilityTypeName &utility) const;
  ResultUtilityAllowed checkOperand(const TypeRef &ref, const UtilityTypeName &utility,
                                    const Type *operand) const;

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
