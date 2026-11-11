#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/assign_checker/assign_checker_errors.h"
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
    const VarSymbol *var;
    bool compound;
    bool reportedRange;
    bool definite;
  };

  void walkTargetAlias(AssignExpr *assign);
  Expr *reportImportedTarget(AssignExpr *assign);
  Expr *checkLogicalAssignValue(AssignExpr *assign);
  Expr *checkNullishAssign(AssignExpr *assign);
  Expr *checkRecordFieldNullish(AssignExpr *assign);
  Expr *checkVariableNullish(AssignExpr *assign);
  bool unfoldsNarrowedCompound(AssignExpr *assign, BinaryOp op);
  Expr *checkPlainAssign(AssignExpr *assign, BinaryOp op, bool compound);
  bool reportsCompoundRange(const AssignPlace &place);
  void walkInferredField(AssignExpr *assign, AssignPlace &place);
  void walkPlainValue(AssignExpr *assign, const Type *type, AssignPlace &place);
  void walkCompoundValue(AssignExpr *assign, BinaryOp op, const Type *type,
                         const AssignPlace &place);
  void walkStringCompound(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                          const Type *type, const VarSymbol *var);
  void walkNumericCompound(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                           const Type *type);
  void walkCompoundOperands(AssignExpr *assign, BinaryOp op, const std::string &spelling,
                            const Type *type, bool shift);
  void walkDivisor(AssignExpr *assign, BinaryOp op);
  void walkCompoundFacts(AssignExpr *assign, BinaryOp op, const VarSymbol *var);
  void walkAssignedElement(AssignExpr *assign, const AssignPlace &place);
  void walkAssignedLength(AssignExpr *assign, const AssignPlace &place);
  void walkLengthValue(const MemberExpr *member, const Expr *value);
  void walkAssignedField(const AssignPlace &place);
  Expr *copyRoot(const Expr *e);
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
  ConstantFolder *constantFolder = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
