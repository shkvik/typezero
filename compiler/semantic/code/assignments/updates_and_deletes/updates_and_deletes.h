#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/assignments/assignment_rules/assignment_rules.h"
#include "compiler/semantic/code/assignments/code_assignments_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class UpdatesAndDeletes {
public:
  UpdatesAndDeletes(SemanticState &owner, AssignmentRules &sharedAssignmentRules);

  void checkUpdate(const CheckUpdate &task);
  void checkDeleteValue(const CheckDeleteValue &task);
  void afterOtherDelete(const AfterOtherDelete &task);
  void afterUpdateTarget(const AfterUpdateTarget &task);
  void afterMemberDelete(const AfterMemberDelete &task);
  void afterIndexDeleteKey(const AfterIndexDeleteKey &task);
  void afterIndexDeleteObject(const AfterIndexDeleteObject &task);

private:
  using ResultDeleteIsPossible = Result<Expr *, CodeAssignmentsRegistryCheckDeleteError>;
  using ResultIncrementedValue = Result<const Type *, CodeAssignmentsRegistryUpdatableError>;

  bool isIdentifierChar(char letter) const;


  const Field *ownField(const Type *object, llvm::StringRef name) const;
  std::string fieldText(const Expr *object, llvm::StringRef field) const;
  const Field *keyedField(const Type *type, const IndexExpr *index) const;


  ResultDeleteIsPossible checkFieldDelete(const Expr *object, const Field *field) const;
  ResultDeleteIsPossible checkDeletedField(const Expr *object, const Field *field) const;
  ResultIncrementedValue checkUpdatable(const UpdateExpr *update, const Type *type) const;
  ResultDeleteIsPossible checkKeyDelete(UnaryExpr *unary, const IndexExpr *index,
                                        const std::string &array, const std::string &position);


  std::optional<std::string> chainText(const Expr *expr) const;
  std::optional<std::string> simpleText(const Expr *expr) const;

  SemanticState &state;
  AssignmentRules &assignmentRules;
};

}
