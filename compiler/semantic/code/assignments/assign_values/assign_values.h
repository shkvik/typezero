#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/assignments/assignment_rules/assignment_rules.h"
#include "compiler/semantic/code/assignments/code_assignments_registry_errors.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class AssignValues {
public:
  AssignValues(SemanticState &owner, AssignmentRules &sharedAssignmentRules);

  void lengthFacts(const LengthFacts &task);
  void compoundFacts(const CompoundFacts &task);
  void assignedField(const AssignedField &task);
  void assignedLength(const AssignedLength &task);
  void afterPlainValue(const AfterPlainValue &task);
  void afterAssignValue(const AfterAssignValue &task);
  void afterAssignTarget(const AfterAssignTarget &task);
  void afterInferredField(const AfterInferredField &task);
  void checkAssignDivisor(const CheckAssignDivisor &task);
  void afterStringCompound(const AfterStringCompound &task);
  void afterNumericCompound(const AfterNumericCompound &task);

private:
  using ResultDivisorNotZero = Result<const Expr *, CodeArithmeticRegistryCheckDivisorError>;
  using ResultLengthIsInBounds = Result<const Expr *, CodeAssignmentsRegistryLengthProvenError>;
  using ResultStringAssignable = Result<const Type *, CodeAssignmentsRegistryStringAssignError>;
  using ResultFieldTypeIsFound = Result<const Type *, CodeAssignmentsRegistryFieldTypeFromError>;
  using ResultCompoundOperands = Result<const Type *, CodeAssignmentsRegistryCompoundOperandsError>;
  using ResultElementWriteSafe =
      Result<const IndexExpr *, CodeAssignmentsRegistryElementWritableError>;

  void startPlainValue(AssignExpr *assign, const Type *type, const AssignPlace &place);
  void startCompoundValue(AssignExpr *assign, BinaryOp op, const Type *type,
                          const AssignPlace &place);


  bool isInitializerThis(const Expr *expr) const;
  bool reportCompoundRange(const AssignPlace &place);


  Expr *recordAssign(AssignExpr *assign);
  Expr *targetObject(const IndexExpr *index, const MemberExpr *member) const;


  std::string joinHint(const AssignExpr *assign) const;
  std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) const;


  ResultFieldTypeIsFound checkFieldTypeFrom(const MemberExpr *member, const Type *type) const;
  ResultLengthIsInBounds checkLengthProven(const MemberExpr *member, const Expr *value) const;
  ResultElementWriteSafe checkElementWritable(const IndexExpr *index, bool isInRange,
                                              AssignForm form) const;
  ResultStringAssignable checkStringAssign(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                           const AssignExpr *assign) const;
  ResultCompoundOperands checkCompoundOperands(BinaryOp op, llvm::StringRef spelling,
                                               const Type *type, const Expr *value,
                                               bool isSettled) const;


  ClassDecl *classOf(const Type *type) const;
  ResultDivisorNotZero checkDivisor(BinaryOp op, const Expr *divisor) const;

  SemanticState &state;
  AssignmentRules &assignmentRules;
};

}
