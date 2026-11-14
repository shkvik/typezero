#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/APInt.h"
#include <optional>
#include <string>

namespace tz {

class ArithmeticRules {
public:
  explicit ArithmeticRules(SemanticState &owner);

  void foldBinary(BinaryExpr *binary);
  void startWrapping(BinaryExpr *binary);


  bool isShiftOperator(BinaryOp op) const;
  bool isUnsigned64(const Type *type) const;


  std::string mixedNumbersText(const BinaryExpr *binary) const;
  std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) const;


  Expr *foldedUnary(UnaryExpr *unary);
  std::optional<uint64_t> unsignedConstant(const Expr *expr) const;

private:
  struct NumberConversion {
    const Type *target = nullptr;
    bool isLeftConverted = false;
  };
  struct IntFold {
    std::optional<int64_t> exact;
    int64_t wrapped;
    double value;
    std::string text;
  };
  struct IntPower {
    int64_t wrapped;
    std::optional<int64_t> exact;
  };

  using ResultShiftCountFits = Result<const Type *, CodeArithmeticRegistryShiftCountError>;

  void foldUnary(UnaryExpr *unary);
  void foldUnsigned64(BinaryExpr *binary);
  void foldBitNot(UnaryExpr *unary, int64_t value);
  void foldNegation(UnaryExpr *unary, int64_t value);
  void rememberFolded(BinaryExpr *binary, const IntFold &fold);
  void foldNegativeLiteral(UnaryExpr *unary, const IntLitExpr *literal);
  void reportUnsignedOverflow(const BinaryExpr *binary, uint64_t left, uint64_t right);


  bool isFitting(const Type *type, int64_t value) const;


  IntFold exactFold(int64_t value) const;
  int64_t wrapTo(const Type *type, int64_t value) const;
  IntFold foldArithmetic(BinaryOp op, int64_t left, int64_t right) const;
  IntFold divisionByMinusOne(BinaryOp op, const Type *type, int64_t left) const;
  IntFold foldBits(BinaryOp op, const Type *type, int64_t left, int64_t right) const;


  uint64_t bitPattern(int64_t value, int width) const;
  IntPower intPower(int64_t base, int64_t exponent) const;


  std::string formatNumber(double value) const;
  std::string decimalText(const llvm::APInt &wide) const;
  std::string wideText(int64_t left, int64_t right, BinaryOp op) const;
  std::string constantText(std::optional<int64_t> exact, double value) const;
  std::string convertedText(llvm::StringRef operand, const std::string &method) const;
  std::string overflowText(const Type *type, const std::string &value, int64_t wrapped) const;
  std::string mixedExample(const BinaryExpr *binary, const NumberConversion &conversion) const;
  llvm::APInt wideResult(BinaryOp op, const llvm::APInt &left, const llvm::APInt &right) const;


  std::optional<IntFold> foldPower(int64_t base, int64_t exponent) const;
  std::optional<IntFold> foldIntOp(BinaryExpr *binary, int64_t left, int64_t right);
  std::optional<IntFold> foldShift(BinaryExpr *binary, int64_t value, int64_t count);
  std::optional<IntFold> foldShiftLeft(BinaryExpr *binary, int64_t value, int count,
                                       uint64_t pattern);
  std::optional<IntFold> foldDivision(BinaryOp op, const Type *type, int64_t left,
                                      int64_t right) const;
  std::optional<int64_t> checkedProduct(std::optional<int64_t> left,
                                        std::optional<int64_t> right) const;


  std::optional<uint64_t> nonNegative(std::optional<int64_t> value) const;
  std::optional<uint64_t> unsignedResult(BinaryOp op, uint64_t left, uint64_t right) const;


  ResultShiftCountFits shiftCount(const Type *shifted, int64_t count) const;
  NumberConversion mixedConversion(const Type *leftType, const Type *rightType) const;

  SemanticState &state;
};

}
