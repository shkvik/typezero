#pragma once

#include "compiler/load/lexer/lexer.h"
#include "compiler/program/ast/ast.h"
#include <memory>

namespace tz {

// Recursive-descent parser for the supported subset of TypeScript.
//
// After a syntax error the parser skips to the next statement and suppresses
// further errors until then, so one mistake yields one message.
//
// Destructuring never reaches the AST: the parser expands it into plain
// declarations and assignments through hidden variables (see declarePattern
// and assignPattern).
class TokenCursor;
class StmtParser;
class VarParser;
class PatternParser;
class ExprParser;
class TemplateParser;
class FunctionParser;
class ClassParser;
class DeclParser;
class ModuleParser;
class TypeParser;

class Parser {
public:
  // `library`: the code of a built-in module of tz written in tz
  // (Module::builtin), which may declare intrinsics and accessors (see
  // FunctionDecl::external).
  Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags, bool library = false);
  SourceFile parseSourceFile();
  // A function declaration, with the lexer at its `function`: Sema parses
  // FunctionDecl::source again to get a fresh copy.
  FunctionDecl *parseFunctionCopy();
  // The same for an arrow or a function expression.
  FunctionDecl *parseFunctionExprCopy();
  // The same for a method of a class or of an object literal, with the lexer
  // at its name.
  FunctionDecl *parseMethodCopy();
  // The same for a generic class (ClassDecl::source), with the lexer at its
  // `class`.
  ClassDecl *parseClassCopy();

private:
  // Looking ahead: the parser tries a construct with diagnostics off, then
  // goes back to where it started.
  struct Snapshot {
    Lexer::State lexer, peekState;
    Token tok, peeked;
    const char *prevEnd;
    bool hasPeeked, recovering;
  };
  // `(a: int, b = 1, ...rest: int[], { x, y }: Point)`. A parameter written as
  // a pattern gets a hidden name; the declarations that destructure it go to
  // `prologue`, for the start of the body (see addPrologue).
  struct Pattern;
  using ParamPatterns = std::vector<std::pair<llvm::StringRef, std::unique_ptr<Pattern>>>;
  // Destructuring: `[a, , b = 1, [c], ...rest]`, or with `object`
  // `{ a, b: c = 1, d: [e], [key]: f, ...rest }`.
  struct Pattern {
    struct Element {
      llvm::SMLoc loc;
      bool hole = false;
      bool rest = false;               // `...rest`, the last element
      llvm::StringRef name;            // a declaration's variable
      Expr *target = nullptr;          // what an assignment assigns to
      std::unique_ptr<Pattern> nested; // or a nested pattern
      Expr *defaultValue = nullptr;    // `= value`, taken for undefined
      // Object patterns: the field the element takes, by name or by a key in
      // brackets.
      llvm::StringRef key;
      Expr *keyExpr = nullptr;
    };
    llvm::SMLoc loc;
    bool object = false;
    std::vector<Element> elements;

    bool declares(llvm::StringRef name) const;
  };
  Lexer &lexer_;
  AstContext &ctx_;
  Diagnostics &diags_;
  Token tok_;
  const char *prevEnd_ = nullptr; // the end of the token before tok_
  Token peeked_;
  bool hasPeeked_ = false;
  // Where the lexer was before it read peeked_, for a regex literal that
  // starts at tok_ (see parsePrimary).
  Lexer::State peekState_;
  bool recovering_ = false;
  // In the initializer of a `for` header `in` is not an operator.
  bool noIn_ = false;
  // The next parseType parses no conditional type: it is the `extends`
  // clause of one, where `A extends B ? C : D` needs parentheses, as in
  // TypeScript. disallowConditional_ holds while that type is parsed, but
  // not inside the brackets it may have.
  bool noConditional_ = false;
  bool disallowConditional_ = false;
  // In the body of a `function*`, where `yield` is an operator.
  bool inGenerator_ = false;
  bool library_ = false;
  // See SourceFile::fieldNames and stringConstants.
  llvm::StringSet<> fieldNames_;
  std::vector<ImportDecl> imports_;
  std::vector<ExportDecl> exports_;
  llvm::StringMap<std::vector<std::string>> stringConstants_;

public:
  ~Parser();

  TokenCursor &cursor() const { return *cursor_; }
  StmtParser &stmts() const { return *stmts_; }
  VarParser &vars() const { return *vars_; }
  PatternParser &patterns() const { return *patterns_; }
  ExprParser &exprs() const { return *exprs_; }
  TemplateParser &templates() const { return *templates_; }
  FunctionParser &functions() const { return *functions_; }
  ClassParser &classes() const { return *classes_; }
  DeclParser &decls() const { return *decls_; }
  ModuleParser &modules() const { return *modules_; }
  TypeParser &types() const { return *types_; }

private:
  friend class TokenCursor;
  friend class StmtParser;
  friend class VarParser;
  friend class PatternParser;
  friend class ExprParser;
  friend class TemplateParser;
  friend class FunctionParser;
  friend class ClassParser;
  friend class DeclParser;
  friend class ModuleParser;
  friend class TypeParser;
  std::unique_ptr<TokenCursor> cursor_;
  std::unique_ptr<StmtParser> stmts_;
  std::unique_ptr<VarParser> vars_;
  std::unique_ptr<PatternParser> patterns_;
  std::unique_ptr<ExprParser> exprs_;
  std::unique_ptr<TemplateParser> templates_;
  std::unique_ptr<FunctionParser> functions_;
  std::unique_ptr<ClassParser> classes_;
  std::unique_ptr<DeclParser> decls_;
  std::unique_ptr<ModuleParser> modules_;
  std::unique_ptr<TypeParser> types_;
};

} // namespace tz
