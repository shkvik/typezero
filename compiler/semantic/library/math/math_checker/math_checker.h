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
class ConstantFolder;
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

  static ResultWrapBitsWidth wrapBitsWidth(llvm::StringRef name);
  static ResultRoundedDouble roundedDouble(llvm::StringRef name, const Type *type);
  Expr *reportMathMember(CallExpr *call, MemberExpr *member);
  bool mathArgsNumeric(CallExpr *call, MathFunctionForm form, const Type *hint);
  static const Type *mathArgHint(MathFunctionForm form, const Type *hint);
  Expr *checkMathResult(CallExpr *call, MemberExpr *member, const MathFunctionInfo &fn);
  Expr *checkRounding(CallExpr *call, MemberExpr *member);
  Expr *checkSharedResult(CallExpr *call);
  Expr *checkInt32Math(CallExpr *call, MemberExpr *member, Builtin builtin);
  Expr *checkArgsLoosely(CallExpr *call);
  Expr *checkWrapFrom(CallExpr *call, MemberExpr *member, MemberExpr *inner);
  Expr *checkWideBits(CallExpr *call, const BitFunctionInfo &fn, const std::string &what);
  Expr *checkPlainBits(CallExpr *call, const BitFunctionInfo &fn, const std::string &what);
  bool bitCountFits(CallExpr *call, const std::string &what);

  struct SpreadValues {
    const Type *common;
    bool someValue;
    bool numeric;
  };

  bool minMaxArgFits(CallExpr *call, size_t i, const Type *hint, SpreadValues &values);
  const Type *spreadElementType(CallExpr *call, size_t i, const Type *hint, SpreadValues &values);
  Expr *spreadIteratorValues(Expr *arg);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
