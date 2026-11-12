#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class VarChecker;
class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class ObjectChecker;
class TypeRelations;
class ProcessChecker;
class ArgumentChecker;

class RegexChecker {
public:
  RegexChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkRegexLit(RegexLitExpr *literal);
  const RegexLitExpr *knownRegex(const Expr *expr) const;
  const Type *matchType(const RegexLitExpr *regex);
  void noteRegexVariable(VarSymbol *variable, const Expr *init, bool isConst,
                         llvm::ArrayRef<Stmt *> block, size_t index);
  Expr *checkRegexMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRegexMember(MemberExpr *member);
  Expr *checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args);
  Expr *checkRegExpStatic(CallExpr *call, MemberExpr *member);
  Expr *checkRegExpStaticMember(MemberExpr *member);
  Expr *regexToString(Expr *regex);

private:
  using ResultParsedFlags = Result<uint32_t, RegexCheckerParsedFlagsError>;

  bool assignsName(llvm::ArrayRef<const Stmt *> stmts, llvm::StringRef name) const;
  ResultParsedFlags parsedFlags(const std::u16string &flags) const;
  std::u16string units(const uint32_t *program, uint32_t start, uint32_t length) const;
  bool isFlag(llvm::StringRef name, uint32_t &flag) const;

  SemanticState &state;
  Prelude *prelude = nullptr;
  VarChecker *varChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ObjectChecker *objectChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
