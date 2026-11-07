#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/concat_checker/concat_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ConcatChecker {
public:
  explicit ConcatChecker(Checker &owner);

  Expr *toStringOperand(Expr *expr);
  static std::string mixedConcatText(const BinaryExpr *root, const BinaryExpr *mixed);

private:
  using Parts = std::vector<std::pair<const Expr *, llvm::StringRef>>;
  using ResultConvertsToText = Result<const Expr *, ConcatCheckerConvertibleError>;

  static const Expr *unconverted(const Expr *expr);
  static ResultConvertsToText convertible(const Expr *expr);
  static void concatParts(const BinaryExpr *bin, Parts &parts);
  static std::string concatTemplate(const Parts &parts, llvm::StringRef &only);
  static std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source,
                                                                  llvm::SMLoc op, size_t length);

  Checker &checker;
};

}
