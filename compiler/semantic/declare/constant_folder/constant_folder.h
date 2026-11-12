#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/constant_folder/constant_folder_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/APInt.h"

namespace tz {

class SemanticParts;
class Scopes;

class ConstantFolder {
public:
  using ResultDivisorNotZero = Result<const Expr *, ConstantFolderCheckDivisorError>;

  ConstantFolder(SemanticState &owner, SemanticParts &all);

  void foldUnary(UnaryExpr *unary);
  void foldBinary(BinaryExpr *binary);
  std::optional<int64_t> intConstant(const Expr *expr) const;
  void noteUnsignedVar(const VarSymbol *variable, const Expr *init);
  std::optional<uint64_t> unsignedConstant(const Expr *expr) const;
  ResultDivisorNotZero checkDivisor(BinaryOp op, const Expr *divisor) const;
  Expr *globalConstant(GlobalName object, llvm::StringRef name, llvm::SMLoc loc);
  std::vector<EnumDecl *> enumsNeeded(const Expr *init, const EnumDecl *self) const;
  std::optional<ConstantValue> evalConstant(const Expr *expr, const EnumDecl *self, size_t defined);

private:
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

  using ResultShiftCountFits = Result<const Type *, ConstantFolderShiftCountError>;

  ResultShiftCountFits shiftCount(const Type *shifted, int64_t count) const;

  IntFold exactFold(int64_t value) const;
  void foldUnsigned64(BinaryExpr *binary);
  int32_t jsToInt32(double number) const;
  bool isUnsigned64(const Type *type) const;
  ConstantValue numberValue(double number) const;
  ConstantValue textValue(std::u16string text) const;
  EnumDecl *enumNamedBy(const Expr *expr) const;
  double power(double base, double exponent) const;
  std::string formatNumber(double value) const;
  uint64_t bitPattern(int64_t value, int width) const;
  ConstantValue memberValue(const EnumMember &member) const;
  bool isFitting(const Type *type, int64_t value) const;
  int64_t wrapTo(const Type *type, int64_t value) const;
  void foldBitNot(UnaryExpr *unary, int64_t value);
  void foldNegation(UnaryExpr *unary, int64_t value);
  std::string decimalText(const llvm::APInt &wide) const;
  IntPower intPower(int64_t base, int64_t exponent) const;
  std::optional<int64_t> plainIntConstant(const Expr *expr) const;
  NodeChildren<const Expr *> constantParts(const Expr *expr) const;
  void rememberFolded(BinaryExpr *binary, const IntFold &fold);
  std::optional<IntFold> foldPower(int64_t base, int64_t exponent) const;
  std::string wideText(int64_t left, int64_t right, BinaryOp op) const;
  std::optional<uint64_t> nonNegative(std::optional<int64_t> value) const;
  std::optional<int64_t> literalConstant(const IntLitExpr *literal) const;
  IntFold foldArithmetic(BinaryOp op, int64_t left, int64_t right) const;
  ConstantValue int32Result(BinaryOp op, int32_t left, int32_t right) const;
  void foldNegativeLiteral(UnaryExpr *unary, const IntLitExpr *literal);
  std::string constantText(std::optional<int64_t> exact, double value) const;
  IntFold divisionByMinusOne(BinaryOp op, const Type *type, int64_t left) const;
  std::optional<IntFold> foldIntOp(BinaryExpr *binary, int64_t left, int64_t right);
  std::optional<IntFold> foldShift(BinaryExpr *binary, int64_t value, int64_t count);
  IntFold foldBits(BinaryOp op, const Type *type, int64_t left, int64_t right) const;
  std::optional<uint64_t> unsignedResult(BinaryOp op, uint64_t left, uint64_t right) const;
  void reportUnsignedOverflow(const BinaryExpr *binary, uint64_t left, uint64_t right);
  std::optional<ConstantValue> numberResult(BinaryOp op, double left, double right) const;
  std::string overflowText(const Type *type, const std::string &value, int64_t wrapped) const;
  std::optional<int64_t> checkedProduct(std::optional<int64_t> left,
                                        std::optional<int64_t> right) const;
  llvm::APInt wideResult(BinaryOp op, const llvm::APInt &left, const llvm::APInt &right) const;
  std::optional<IntFold> foldDivision(BinaryOp op, const Type *type, int64_t left,
                                      int64_t right) const;
  std::optional<IntFold> foldShiftLeft(BinaryExpr *binary, int64_t value, int count,
                                       uint64_t pattern);
  std::optional<ConstantValue> unaryConstant(UnaryOp op,
                                             const std::optional<ConstantValue> &operand) const;
  std::optional<ConstantValue> joinedText(BinaryOp op, const ConstantValue &left,
                                          const ConstantValue &right) const;
  std::optional<ConstantValue> leafConstant(const Expr *expr, const EnumDecl *self,
                                            size_t defined) const;
  std::optional<ConstantValue> ownMemberConstant(const EnumDecl *self, size_t defined,
                                                 llvm::StringRef name) const;
  std::optional<ConstantValue> enumMemberConstant(const MemberExpr *member, const EnumDecl *self,
                                                  size_t defined) const;
  std::optional<ConstantValue> binaryConstant(BinaryOp op, const std::optional<ConstantValue> &left,
                                              const std::optional<ConstantValue> &right) const;
  std::optional<ConstantValue> nodeConstant(const Expr *expr, const EnumDecl *self, size_t defined,
                                            std::vector<std::optional<ConstantValue>> &answers);

  SemanticState &state;
  Scopes *scopes = nullptr;
};

}
