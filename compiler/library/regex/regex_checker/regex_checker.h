#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Regular expressions: literals and the methods of RegExp (docs/regex.md).
class RegexChecker {
public:
  explicit RegexChecker(Sema &sema);

  // Regular expressions (sema_regex.cpp). A literal is compiled here, so its
  // syntax errors are compile errors, and its groups and flags are known;
  // so are those of a variable that holds a literal all along. A regex from
  // anywhere else (a parameter, a field, `new RegExp(s)`) is of a kind known
  // only when the program runs.
  Expr *checkRegexLit(RegexLitExpr *lit);
  // The literal `expr` surely holds, or null.
  static const RegexLitExpr *knownRegex(const Expr *expr);
  // The type of a match of `regex`; null for a regex not known.
  const Type *matchType(const RegexLitExpr *regex);
  // Notes a variable that holds the literal of its initializer all along;
  // `block` and `index` are where it is declared.
  void noteRegexVariable(VarSymbol *var, const Expr *init, bool isConst,
                         llvm::ArrayRef<Stmt *> block, size_t index);
  // re.test(s), re.exec(s), re.toString() and the properties of a regex.
  Expr *checkRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRegexMember(MemberExpr *member);
  // `new RegExp(pattern, flags)` and `RegExp(...)`; RegExp.escape(s), and
  // the static properties of RegExp, which are errors.
  Expr *checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkRegExpStatic(CallExpr *call, MemberExpr *member);
  Expr *checkRegExpStaticMember(MemberExpr *member);
  // String(re) and `${re}`: "/source/flags".
  Expr *regexToString(Expr *regex);

private:
  Sema &sema_;
};

} // namespace tz
