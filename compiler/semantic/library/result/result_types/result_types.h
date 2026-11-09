#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/result/result_types/result_types_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class TypeDeclarer;
class TypeResolver;
class TypeRelations;

class ResultTypes {
public:
  using ResultImportSource = Result<const ImportDecl *, ResultTypesImportSourceError>;

  ResultTypes(SemanticState &owner, SemanticParts &all);

  std::optional<GlobalName> resultName(llvm::StringRef name) const;
  llvm::StringRef resultTypeName(llvm::StringRef name) const;
  static ResultImportSource importSource(const ImportDecl &decl);
  const Type *resolveResultType(const TypeRef &ref, llvm::StringRef name);
  static const Type *unifyHalf(const Type *a, const Type *b);
  void neverOperands(BinaryExpr *bin);

private:
  using ResultTypeArgsPair = Result<const TypeRef *, ResultTypesResultTypeArgsError>;

  static ResultTypeArgsPair resultTypeArgs(const TypeRef &ref);

  static llvm::StringRef findName(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef name);

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
