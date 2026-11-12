#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/assign_checker/assign_checker_errors.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_types.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class FieldFacts;
class VarChecker;
class DictChecker;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class BytesChecker;
class ClassChecker;
class TruthChecker;
class BinaryChecker;
class ConcatChecker;
class TargetChecker;
class TypeRelations;
class ClassDeclarer;
class ConstantFolder;
class DecimalChecker;
class GenericChecker;
class ProcessChecker;
class TopLevelChecker;
class BuiltinModuleChecker;

class AssignChecker {
public:
  AssignChecker(SemanticState &owner, SemanticParts &all);

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

  struct AssignPlace {
    IndexExpr *index;
    MemberExpr *member;
    NameExpr *name;
    const VarSymbol *variable;
    AssignForm form;
    bool hasReportedRange;
    bool isDefinite;
  };

  Expr *copyChain(const Expr *expr);
  Expr *copyRoot(const Expr *expr);
  ResultNameIsAssignable checkNotImported(const NameExpr *name);
  ResultTargetIsVariable checkVariableTarget(const Expr *target) const;
  ResultVariableOptional checkNullishOptional(const NameExpr *name, const Type *type) const;
  ResultIndexWithinArray checkIndexProven(const IndexExpr *index) const;
  ResultFieldTypeIsFound checkFieldTypeFrom(const MemberExpr *member, const Type *type) const;
  ResultStringAssignable checkStringAssign(BinaryOp op, llvm::StringRef spelling, const Type *type,
                                           const AssignExpr *assign) const;
  std::string joinHint(const AssignExpr *assign) const;
  ResultCompoundOperands checkCompoundOperands(BinaryOp op, llvm::StringRef spelling,
                                               const Type *type, const Expr *value,
                                               bool isSettled) const;
  ResultElementWriteSafe checkElementWritable(const IndexExpr *index, bool isInRange,
                                              AssignForm form) const;
  ResultLengthIsInBounds checkLengthProven(const MemberExpr *member, const Expr *value) const;
  void walkTargetAlias(AssignExpr *assign);
  Expr *reportImportedTarget(AssignExpr *assign);
  Expr *checkLogicalAssignValue(AssignExpr *assign);
  Expr *checkNullishAssign(AssignExpr *assign);
  Expr *checkRecordFieldNullish(AssignExpr *assign);
  bool isRecordHolder(const Expr *object) const;
  Expr *checkVariableNullish(AssignExpr *assign);
  void walkNullishValue(AssignExpr *assign, const Type *type);
  bool unfoldNarrowedCompound(AssignExpr *assign, BinaryOp op);
  Expr *checkPlainAssign(AssignExpr *assign, BinaryOp op, AssignForm form);
  bool reportCompoundRange(const AssignPlace &place);
  void walkInferredField(AssignExpr *assign, AssignPlace &place);
  void walkPlainValue(AssignExpr *assign, const Type *type, AssignPlace &place);
  void walkCompoundValue(AssignExpr *assign, BinaryOp op, const Type *type,
                         const AssignPlace &place);
  void walkStringCompound(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                          const Type *type, const VarSymbol *variable);
  void walkNumericCompound(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                           const Type *type);
  void walkCompoundOperands(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                            const Type *type, bool isShift);
  void walkDivisor(AssignExpr *assign, BinaryOp op);
  void walkCompoundFacts(AssignExpr *assign, BinaryOp op, const VarSymbol *variable);
  void walkAssignedElement(AssignExpr *assign, const AssignPlace &place);
  void walkAssignedLength(AssignExpr *assign, const AssignPlace &place);
  void walkLengthValue(const MemberExpr *member, const Expr *value);
  void walkFilledLength(const MemberExpr *member, const Expr *value);
  void walkAssignedField(const AssignPlace &place);
  ResultLogicalTargetSet checkLogicalAssign(AssignExpr *assign);
  Expr *logicalRead(const Expr *target);
  Expr *indexRead(const IndexExpr *index);
  ResultFieldChainCopied checkFieldChain(Expr *read) const;
  ResultFieldMayBeAbsent checkFieldOptional(const MemberExpr *member, const Type *type) const;
  Expr *checkFieldLogicalAssign(AssignExpr *assign);
  const Type *assignedValueType(const Expr *value) const;
  void defineFieldOf(const MemberExpr *member);
  ResultIncrementedValue checkUpdatable(const UpdateExpr *update, const Type *type) const;
  void walkUpdatedName(const NameExpr *name, const UpdateExpr *update);
  void walkUpdatedElement(IndexExpr *index);

  SemanticState &state;
  Scopes *scopes = nullptr;
  FieldFacts *fieldFacts = nullptr;
  VarChecker *varChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BytesChecker *bytesChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TargetChecker *targetChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ConstantFolder *constantFolder = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
