#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/v2/walk/check_task_context/check_task.h"
#include "llvm/ADT/ArrayRef.h"
#include <utility>

namespace tz {

class SemanticState;

class GenericsConstraints {
public:
  explicit GenericsConstraints(SemanticState &owner);

  bool isSatisfying(const ConstraintCheck &check) const;


  FuncSymbol *findUnknownMethod(const ConstraintCheck &check) const;

private:
  struct MethodTypes {
    llvm::ArrayRef<std::pair<const FuncSymbol *, const Type *>> known;
    FuncSymbol *missing = nullptr;
  };

  bool isTextUnion(const Type *type) const;
  bool isMatchingField(const Field &have, const Field &wanted) const;
  bool evaluate(const ConstraintCheck &check, MethodTypes &types) const;
  bool hasMembersOf(const Type *arg, const Type *constraint, MethodTypes &types) const;
  bool hasObjectMembersOf(const Type *arg, const Type *constraint, MethodTypes &types) const;
  bool isFittingConstraint(const Type *arg, const Type *constraint, MethodTypes &types) const;
  bool hasMethodField(const ClassDecl *classDecl, const Field &field, MethodTypes &types) const;
  bool hasMatchingOverload(const ClassDecl *classDecl, const Overload &overload,
                           MethodTypes &types) const;
  bool hasMethodOfType(llvm::ArrayRef<FunctionDecl *> methods, const Type *signature,
                       MethodTypes &types) const;
  bool hasMatchingField(const Type *arg, const ClassDecl *classDecl, const Field &field,
                        MethodTypes &types) const;


  const Type *methodType(FuncSymbol *function, MethodTypes &types) const;

  SemanticState &state;
};

}
