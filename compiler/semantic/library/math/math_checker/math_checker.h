#pragma once

#include "compiler/program/library/math/bit_function_table/bit_function_table.h"
#include "compiler/program/library/math/math_function_table/math_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/math/math_checker/math_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class DictChecker;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class BinaryChecker;
class TypeRelations;
class GenericChecker;
class ArgumentChecker;
class IteratorMethods;
class GeneratorChecker;

class MathChecker {
public:
  MathChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMinMaxSpread(CallExpr *call, const Type *hint);
  Expr *checkBitCall(CallExpr *call, MemberExpr *member);
  Expr *bitsArg(Expr *arg, unsigned bits, const Type *literal, const std::string &what);
  const Type *sharedType(std::vector<Expr *> &args, const std::vector<bool> &spreads,
                         const Type *common = nullptr);
  Expr *numericArg(Expr *arg, const Type *hint = nullptr);

private:
  using ResultWrapBitsWidth = Result<unsigned, MathCheckerWrapBitsWidthError>;
  using ResultRoundedDouble = Result<const Type *, MathCheckerRoundedDoubleError>;

  struct SpreadValues {
    const Type *common;
    bool hasSomeValue;
    bool isNumeric;
  };

  ResultWrapBitsWidth checkWrapBitsWidth(llvm::StringRef name) const;
  ResultRoundedDouble checkRoundedDouble(llvm::StringRef name, const Type *type) const;
  Expr *reportMathMember(CallExpr *call, MemberExpr *member);
  bool prepareMathArgs(CallExpr *call, MathFunctionForm form, const Type *hint);
  const Type *mathArgHint(MathFunctionForm form, const Type *hint) const;
  Expr *checkMathResult(CallExpr *call, MemberExpr *member, const MathFunctionInfo &function);
  Expr *checkFloatResult(CallExpr *call);
  Expr *checkRounding(CallExpr *call, MemberExpr *member);
  Expr *checkSharedResult(CallExpr *call);
  Expr *checkInt32Math(CallExpr *call, MemberExpr *member, Builtin builtin);
  bool isSpreadAt(const std::vector<bool> &spreads, size_t index) const;
  const Type *commonArgType(llvm::ArrayRef<Expr *> args, const std::vector<bool> &spreads) const;
  const Type *literalsType(llvm::ArrayRef<Expr *> args, const std::vector<bool> &spreads) const;
  bool adaptSharedArg(Expr *&arg, const Type *common);
  Expr *checkedBitsArg(Expr *arg, const Type *target);
  std::string bitsHint(const Expr *arg, const Type *unsignedType);
  Expr *checkWrapFrom(CallExpr *call, MemberExpr *member, MemberExpr *inner);
  Expr *checkWideBits(CallExpr *call, const BitFunctionInfo &function, const std::string &what);
  Expr *checkPlainBits(CallExpr *call, const BitFunctionInfo &function, const std::string &what);
  bool convertBitCount(CallExpr *call, const std::string &what);
  bool takeMinMaxArg(CallExpr *call, size_t index, const Type *hint, SpreadValues &values);
  const Type *minMaxArgType(CallExpr *call, size_t index, const Type *hint, SpreadValues &values);
  const Type *spreadElementType(CallExpr *call, size_t index, const Type *hint,
                                SpreadValues &values);
  Expr *spreadIteratorValues(Expr *arg);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
