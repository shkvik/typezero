#pragma once

#include "compiler/load/lexer/lexer.h"
#include "compiler/program/ast/ast.h"
#include <memory>

namespace tz {

class TokenCursor;
class StmtParser;
class VarParser;
class PatternParser;
class ExprParser;
class CallParser;
class PrimaryParser;
class TemplateParser;
class FunctionParser;
class ClassParser;
class DeclParser;
class ModuleParser;
class TypeParser;
class ObjectTypeParser;
class FunctionTypeParser;

class Parser {
public:
  Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags, bool library = false);
  SourceFile parseSourceFile();
  FunctionDecl *parseFunctionCopy();
  FunctionDecl *parseFunctionExprCopy();
  FunctionDecl *parseMethodCopy();
  ClassDecl *parseClassCopy();

private:
  struct Snapshot {
    Lexer::State lexer, peekState;
    Token tok, peeked;
    const char *prevEnd;
    bool hasPeeked, recovering;
  };
  struct Pattern;
  using ParamPatterns = std::vector<std::pair<llvm::StringRef, std::unique_ptr<Pattern>>>;
  struct Pattern {
    struct Element {
      llvm::SMLoc loc;
      bool hole = false;
      bool rest = false;
      llvm::StringRef name;
      Expr *target = nullptr;
      std::unique_ptr<Pattern> nested;
      Expr *defaultValue = nullptr;
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
  const char *prevEnd_ = nullptr;
  Token peeked_;
  bool hasPeeked_ = false;
  Lexer::State peekState_;
  bool recovering_ = false;
  bool noIn_ = false;
  bool noConditional_ = false;
  bool disallowConditional_ = false;
  bool inGenerator_ = false;
  bool library_ = false;
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
  CallParser &calls() const { return *calls_; }
  PrimaryParser &primaries() const { return *primaries_; }
  TemplateParser &templates() const { return *templates_; }
  FunctionParser &functions() const { return *functions_; }
  ClassParser &classes() const { return *classes_; }
  DeclParser &decls() const { return *decls_; }
  ModuleParser &modules() const { return *modules_; }
  TypeParser &types() const { return *types_; }
  ObjectTypeParser &objectTypes() const { return *objectTypes_; }
  FunctionTypeParser &functionTypes() const { return *functionTypes_; }

private:
  friend class TokenCursor;
  friend class StmtParser;
  friend class VarParser;
  friend class PatternParser;
  friend class ExprParser;
  friend class CallParser;
  friend class PrimaryParser;
  friend class TemplateParser;
  friend class FunctionParser;
  friend class ClassParser;
  friend class DeclParser;
  friend class ModuleParser;
  friend class TypeParser;
  friend class ObjectTypeParser;
  friend class FunctionTypeParser;
  std::unique_ptr<TokenCursor> cursor_;
  std::unique_ptr<StmtParser> stmts_;
  std::unique_ptr<VarParser> vars_;
  std::unique_ptr<PatternParser> patterns_;
  std::unique_ptr<ExprParser> exprs_;
  std::unique_ptr<CallParser> calls_;
  std::unique_ptr<PrimaryParser> primaries_;
  std::unique_ptr<TemplateParser> templates_;
  std::unique_ptr<FunctionParser> functions_;
  std::unique_ptr<ClassParser> classes_;
  std::unique_ptr<DeclParser> decls_;
  std::unique_ptr<ModuleParser> modules_;
  std::unique_ptr<TypeParser> types_;
  std::unique_ptr<ObjectTypeParser> objectTypes_;
  std::unique_ptr<FunctionTypeParser> functionTypes_;
};

}
