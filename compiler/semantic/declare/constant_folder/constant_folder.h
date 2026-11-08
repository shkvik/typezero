#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/constant_folder/constant_folder_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ConstantFolder {
public:
  using ResultDivisorNotZero = Result<const Expr *, ConstantFolderCheckDivisorError>;

  explicit ConstantFolder(SemanticState &owner);

  void foldUnary(UnaryExpr *unary, bool wraps);
  void foldBinary(BinaryExpr *bin, bool wraps);
  std::optional<int64_t> intConstant(const Expr *expr) const;
  void noteUnsignedVar(const VarSymbol *var, const Expr *init);
  std::optional<uint64_t> unsignedConstant(const Expr *expr) const;
  ResultDivisorNotZero checkDivisor(BinaryOp op, const Expr *divisor) const;
  Expr *globalConstant(llvm::StringRef object, llvm::StringRef name, llvm::SMLoc loc);
  std::optional<ConstantValue> evalConstant(const Expr *expr, EnumDecl *self, size_t defined);

private:
  using ResultShiftCountFits = Result<const Type *, ConstantFolderShiftCountError>;

  static constexpr int64_t maxExact = int64_t(1) << 53;

  static int32_t jsToInt32(double x);
  static double power(double x, double y);
  static std::string formatNumber(double value);
  static bool fitsIn(const Type *type, int64_t value);
  static int64_t wrapTo(const Type *type, int64_t value);
  static std::string wideText(int64_t a, int64_t b, BinaryOp op);
  static bool exactEnough(const Type *type, int64_t value, bool wraps);
  static ResultShiftCountFits shiftCount(const Type *shifted, int64_t count);
  static std::string constantText(std::optional<int64_t> exact, double value);
  static std::pair<int64_t, std::optional<int64_t>> intPower(int64_t base, int64_t exponent);
  static std::string overflowText(const Type *type, const std::string &value, int64_t wrapped,
                                  bool wraps);

  SemanticState &state;
};

}
