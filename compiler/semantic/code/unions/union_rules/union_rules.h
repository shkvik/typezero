#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/code/unions/code_unions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class UnionRules {
public:
  using ResultMemberAccessible = Result<const ClassDecl *, CodeClassesRegistryCheckAccessError>;

  explicit UnionRules(SemanticState &owner);

  bool hasDataField(const ClassDecl *classDecl, llvm::StringRef name) const;


  ClassDecl *classOf(const Type *type) const;
  ResultMemberAccessible checkAccess(const ClassDecl *classDecl, Access access,
                                     llvm::StringRef name) const;

private:
  bool isDerivedFrom(const ClassDecl *from, const ClassDecl *base) const;
  bool isSameClass(const ClassDecl *first, const ClassDecl *second) const;

  SemanticState &state;
};

}
