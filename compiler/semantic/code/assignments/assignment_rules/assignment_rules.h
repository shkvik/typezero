#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/assignments/code_assignments_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class AssignmentRules {
public:
  using ResultIndexWithinArray = Result<const IndexExpr *, CodeAssignmentsRegistryIndexProvenError>;

  explicit AssignmentRules(SemanticState &owner);

  void startNullishByTarget(AssignExpr *assign);
  void startVariableNullish(AssignExpr *assign);
  void startAssignTarget(Expr **target, AssignForm form);


  Expr *copyChain(const Expr *expr);
  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);


  ResultIndexWithinArray checkIndexProven(const IndexExpr *index) const;

private:
  using ResultFieldChainCopied = Result<Expr *, CodeAssignmentsRegistryFieldChainError>;
  using ResultTargetIsNotThis = Result<const NameExpr *, CodeAssignmentsRegistryNotThisError>;

  void startVarTarget(NameExpr *name);
  void startFieldNullish(AssignExpr *assign);
  void startMemberTarget(Expr **target, MemberExpr *member, AssignForm form);


  Expr *copyRoot(const Expr *expr);
  std::string wrongTargetMessage(const Expr *target) const;
  ResultFieldChainCopied checkFieldChain(Expr *read) const;
  ResultTargetIsNotThis checkNotThis(const NameExpr *name) const;

  SemanticState &state;
};

}
