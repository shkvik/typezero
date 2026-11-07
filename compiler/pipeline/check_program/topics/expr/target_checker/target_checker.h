#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/target_checker/target_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class TargetChecker {
public:
  explicit TargetChecker(CheckState &owner);

  VarSymbol *checkVarTarget(NameExpr *name);
  const Type *checkAssignTarget(Expr *&target, bool compound);

private:
  using ResultDefinedDictType = Result<const Type *, TargetCheckerDictDefinedError>;
  using ResultNoBytesProperty = Result<const Type *, TargetCheckerBytesMemberError>;
  using ResultTargetIsNotThis = Result<const NameExpr *, TargetCheckerNotThisError>;
  using ResultIndexIsNotBytes = Result<const Type *, TargetCheckerIndexNotBytesError>;
  using ResultObjectIsDefined = Result<const Type *, TargetCheckerObjectDefinedError>;
  using ResultIndexIsWritable = Result<const Type *, TargetCheckerIndexWritableError>;
  using ResultFieldIsWritable = Result<const Type *, TargetCheckerMemberWritableError>;
  using ResultVariableMutable = Result<const VarSymbol *, TargetCheckerVariableMutableError>;

  static ResultTargetIsNotThis notThis(const NameExpr *name);
  static ResultIndexIsNotBytes indexNotBytes(const Type *type);
  static ResultObjectIsDefined objectDefined(const Type *type);
  static ResultIndexIsWritable indexWritable(const Type *type);
  static ResultDefinedDictType dictDefined(const Type *type, const MemberExpr *member);
  static ResultNoBytesProperty bytesMember(const Type *type, const MemberExpr *member);
  static ResultVariableMutable variableMutable(const NameExpr *name, const VarSymbol *var);
  static ResultFieldIsWritable memberWritable(const Type *type, const MemberExpr *member,
                                              bool compound);

  CheckState &state;
};

}
