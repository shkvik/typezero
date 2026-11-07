#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class RegexChecker {
public:
  explicit RegexChecker(CheckState &owner);

  Expr *checkRegexLit(RegexLitExpr *lit);
  static const RegexLitExpr *knownRegex(const Expr *expr);
  const Type *matchType(const RegexLitExpr *regex);
  void noteRegexVariable(VarSymbol *var, const Expr *init, bool isConst,
                         llvm::ArrayRef<Stmt *> block, size_t index);
  Expr *checkRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRegexMember(MemberExpr *member);
  Expr *checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkRegExpStatic(CallExpr *call, MemberExpr *member);
  Expr *checkRegExpStaticMember(MemberExpr *member);
  Expr *regexToString(Expr *regex);

private:
  static std::string utf8(const std::u16string &text);
  static std::u16string units(const uint32_t *program, uint32_t start, uint32_t length);
  static bool isFlag(llvm::StringRef name, uint32_t &flag);

  CheckState &state;
};

}
