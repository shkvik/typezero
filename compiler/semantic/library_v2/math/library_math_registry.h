#pragma once

#include "compiler/program/library/math/bit_function_table/bit_function_table.h"
#include "compiler/program/library/math/math_function_table/math_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/math/library_math_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class LibraryMathRegistry {
public:
  explicit LibraryMathRegistry(SemanticState &owner);

  void bitsArgSlot(const BitsArgSlot &task);
  void checkBitCall(const CheckBitCall &task);
  void nextInt32Arg(const NextInt32Arg &task);
  void checkMathCall(const CheckMathCall &task);
  void afterMathArgs(const AfterMathArgs &task);
  void afterBitCount(const AfterBitCount &task);
  void finishBitsArg(const FinishBitsArg &task);
  void afterInt32Arg(const AfterInt32Arg &task);
  void nextMinMaxArg(const NextMinMaxArg &task);
  void numericArgSlot(const NumericArgSlot &task);
  void afterNumericArg(const AfterNumericArg &task);
  void finishPlainBits(const FinishPlainBits &task);
  void nextWideBitsArg(const NextWideBitsArg &task);
  void afterWrapFromArg(const AfterWrapFromArg &task);
  void afterMinMaxValue(const AfterMinMaxValue &task);
  void afterWideBitsArg(const AfterWideBitsArg &task);
  void afterPlainBitsArg(const AfterPlainBitsArg &task);
  void afterMinMaxSpread(const AfterMinMaxSpread &task);

private:
  using ResultWrapBitsWidth = Result<unsigned, LibraryMathRegistryWrapBitsWidthError>;
  using ResultRoundedDouble = Result<const Type *, LibraryMathRegistryRoundedDoubleError>;

  void checkFloatResult(CallExpr *call);
  void checkSharedResult(CallExpr *call);
  void finishMinMaxSpread(const MinMaxDraft &draft);
  void takeMinMaxType(MinMaxDraft draft, const Type *type);
  void checkRounding(CallExpr *call, MemberExpr *member);
  void reportMathMember(CallExpr *call, MemberExpr *member);
  void reportSharedMismatch(const Expr *arg, const Type *target);
  void startInt32Math(CallExpr *call, MemberExpr *member, Builtin builtin);
  void checkWrapFrom(CallExpr *call, MemberExpr *member, MemberExpr *inner);
  void checkMathResult(CallExpr *call, MemberExpr *member, const MathFunctionInfo &function);


  bool isSpreadAt(const std::vector<bool> &spreads, size_t index) const;


  const Type *mathArgHint(MathFunctionForm form, const Type *hint) const;
  const Type *literalsType(llvm::ArrayRef<Expr *> args, const std::vector<bool> &spreads) const;
  const Type *commonArgType(llvm::ArrayRef<Expr *> args, const std::vector<bool> &spreads) const;
  const Type *adaptSharedArgs(std::vector<Expr *> &args, const std::vector<bool> &spreads, const Type *common);


  Expr *spreadIteratorValues(Expr *arg);
  std::string bitsHint(const Expr *arg, const Type *unsignedType) const;
  ResultWrapBitsWidth checkWrapBitsWidth(llvm::StringRef name) const;
  ResultRoundedDouble checkRoundedDouble(llvm::StringRef name, const Type *type) const;

  SemanticState &state;
};

}
