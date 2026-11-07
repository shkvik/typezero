#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/assign_checker/assign_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class AssignChecker {
public:
  explicit AssignChecker(CheckState &owner);

  Expr *checkAssign(AssignExpr *assign);
  Expr *checkUpdate(UpdateExpr *update);

private:
  using ResultFieldChainCopied = Result<Expr *, AssignCheckerFieldChainError>;
  using ResultIncrementedValue = Result<const Type *, AssignCheckerUpdatableError>;
  using ResultStringAssignable = Result<const Type *, AssignCheckerStringAssignError>;
  using ResultLengthIsInBounds = Result<const Expr *, AssignCheckerLengthProvenError>;
  using ResultLogicalTargetSet = Result<Expr *, AssignCheckerCheckLogicalAssignError>;
  using ResultFieldTypeIsFound = Result<const Type *, AssignCheckerFieldTypeFromError>;
  using ResultFieldMayBeAbsent = Result<const Type *, AssignCheckerFieldOptionalError>;
  using ResultTargetIsVariable = Result<const Expr *, AssignCheckerVariableTargetError>;
  using ResultNameIsAssignable = Result<const NameExpr *, AssignCheckerNotImportedError>;
  using ResultVariableOptional = Result<const Type *, AssignCheckerNullishOptionalError>;
  using ResultIndexWithinArray = Result<const IndexExpr *, AssignCheckerIndexProvenError>;
  using ResultCompoundOperands = Result<const Type *, AssignCheckerCompoundOperandsError>;
  using ResultElementWriteSafe = Result<const IndexExpr *, AssignCheckerElementWritableError>;

  Expr *copyChain(const Expr *e);
  Expr *checkFieldLogicalAssign(AssignExpr *assign);
  static ResultFieldChainCopied fieldChain(Expr *read);
  ResultNameIsAssignable notImported(const NameExpr *name);
  ResultLogicalTargetSet checkLogicalAssign(AssignExpr *assign);
  static ResultTargetIsVariable variableTarget(const Expr *target);
  ResultIndexWithinArray indexProven(const IndexExpr *index) const;
  static ResultIncrementedValue updatable(const UpdateExpr *update, const Type *type);
  static ResultVariableOptional nullishOptional(const NameExpr *name, const Type *type);
  ResultLengthIsInBounds lengthProven(const MemberExpr *member, const Expr *value) const;
  static ResultFieldMayBeAbsent fieldOptional(const MemberExpr *member, const Type *type);
  static ResultFieldTypeIsFound fieldTypeFrom(const MemberExpr *member, const Type *type);
  ResultElementWriteSafe elementWritable(const IndexExpr *index, bool inRange, bool compound) const;
  static std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source,
                                                                  llvm::SMLoc op, size_t length);
  static ResultStringAssignable stringAssign(BinaryOp op, llvm::StringRef spelling,
                                             const Type *type, const AssignExpr *assign);
  static ResultCompoundOperands compoundOperands(BinaryOp op, llvm::StringRef spelling,
                                                 const Type *type, const Expr *value, bool settled);

  CheckState &state;
};

}
