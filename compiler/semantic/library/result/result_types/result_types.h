#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/result/result_types/result_types_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

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
  std::optional<TypeName> findResultType(llvm::StringRef name) const;
  ResultImportSource importSource(const ImportDecl &importDecl) const;
  const Type *resolveResultType(const TypeRef &ref, TypeName result);
  const Type *unifyHalf(const Type *a, const Type *b) const;
  static UnifyShortcut halfShortcut(const Type *a, const Type *b);
  void neverOperands(BinaryExpr *binary);

private:
  using ResultTypeArgsPair = Result<const TypeRef *, ResultTypesResultTypeArgsError>;

  ResultTypeArgsPair resultTypeArgs(const TypeRef &ref) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
