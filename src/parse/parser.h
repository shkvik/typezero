#pragma once

#include "ast/ast.h"
#include "lex/lexer.h"

namespace tz {

// Recursive-descent parser for the supported subset of TypeScript.
//
// After a syntax error the parser skips to the next statement and suppresses
// further errors until then, so one mistake yields one message.
//
// Destructuring never reaches the AST: the parser expands it into plain
// declarations and assignments through hidden variables (see declarePattern
// and assignPattern).
class Parser {
public:
  // `library`: the code of a built-in module of tz written in tz
  // (Module::builtin), which may declare intrinsics and accessors (see
  // FunctionDecl::external).
  Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags, bool library = false);

  SourceFile parseSourceFile();
  // A function declaration, with the lexer at its `function`: Sema parses
  // FunctionDecl::source again to get a fresh copy.
  FunctionDecl *parseFunctionCopy() { return parseFunction(/*expression=*/false); }
  // The same for an arrow or a function expression.
  FunctionDecl *parseFunctionExprCopy();
  // The same for a method of a class or of an object literal, with the lexer
  // at its name.
  FunctionDecl *parseMethodCopy();
  // The same for a generic class (ClassDecl::source), with the lexer at its
  // `class`.
  ClassDecl *parseClassCopy();

private:
  // Tokens
  void advance();
  // The token after the current one.
  const Token &peek();
  bool accept(Tok kind);
  bool expect(Tok kind, llvm::StringRef context = {});
  void expectSemicolon();
  // A syntax error: the parser is lost until the next statement.
  void error(llvm::SMLoc loc, const llvm::Twine &message);
  // An error that leaves the parser on track, e.g. an unsupported feature.
  void report(llvm::SMLoc loc, const llvm::Twine &message);
  void synchronize();
  // After an error inside `{ ... }`: skips past the `}` that closes it.
  void skipToClosingBrace();

  // Looking ahead: the parser tries a construct with diagnostics off, then
  // goes back to where it started.
  struct Snapshot {
    Lexer::State lexer, peekState;
    Token tok, peeked;
    const char *prevEnd;
    bool hasPeeked, recovering;
  };
  Snapshot snapshot() const;
  void rewind(Snapshot snapshot);

  // Statements. `import` and `export` are allowed only at the top level.
  Stmt *parseStatement(bool topLevel = false);
  // Imports and exports go to imports_ and exports_; what stays among the
  // statements is the declaration after `export`, or an empty statement.
  Stmt *parseImport(bool topLevel);
  Stmt *parseExport(bool topLevel);
  // `{ a, b as c }`, with the lexer at `{`.
  void parseModuleNames(std::vector<ModuleName> &names, bool import);
  // `from "./x"`, or just the path without `from`; false after reporting an
  // error.
  bool parseFromPath(std::string &path, llvm::SMLoc &loc, bool from = true);
  // Records the names a declaration after `export` declares.
  void exportDeclared(const Stmt *stmt);
  // `namespace X { }`, `declare module "x" { }`, `declare const x: T`:
  // reports `message` and skips the declaration.
  Stmt *skipDeclaration(llvm::SMLoc loc, const llvm::Twine &message);
  BlockStmt *parseBlock();
  Stmt *parseFunctionDecl(bool isAsync = false);
  // A function, or a declaration without a body, such as an overload
  // signature, which parseFunctionDecl rejects. A function expression may
  // have no name.
  FunctionDecl *parseFunction(bool expression, bool isAsync = false);
  // `async function` here, on one line.
  bool asyncFunctionAhead();
  // At `async`: an async arrow or function expression, or null if `async`
  // is just a name here (nothing is consumed then).
  Expr *tryAsyncFunction();
  // `(a: int, b = 1, ...rest: int[], { x, y }: Point)`. A parameter written as
  // a pattern gets a hidden name; the declarations that destructure it go to
  // `prologue`, for the start of the body (see addPrologue).
  struct Pattern;
  using ParamPatterns = std::vector<std::pair<llvm::StringRef, std::unique_ptr<Pattern>>>;
  void parseParams(FunctionDecl *fn, ParamPatterns *patterns = nullptr);
  void addPrologue(FunctionDecl *fn, ParamPatterns &patterns);
  // Whether the `(` here starts the parameters of an arrow function.
  bool arrowAhead();
  // `x => body` or `(params): T => body`; a generic arrow `<T>(x: T) => x`
  // has its type parameters already parsed.
  FunctionExpr *parseArrow(std::vector<TypeParamDecl> typeParams = {}, const char *start = nullptr);
  // `<T, U extends number = int>`
  std::vector<TypeParamDecl> parseTypeParams();
  VarDeclStmt *parseVarDecl();
  void parseDeclarators(VarDeclStmt *decl);
  bool parseBinding(VarDeclarator &d);
  Stmt *parseIf();
  Stmt *parseWhile();
  Stmt *parseDoWhile();
  Stmt *parseFor();
  Stmt *parseSwitch();
  Stmt *parseReturn();
  // `throw value` and `try { } catch (e) { } finally { }` (docs/errors.md).
  Stmt *parseThrow();
  Stmt *parseTry();
  Stmt *parseBreakOrContinue();
  Stmt *parseTypeAlias();
  Stmt *parseInterface();
  Stmt *parseEnum();
  // Classes. A member's modifiers (`static`, `private`...) come before its
  // name; a parameter's before its name in a constructor.
  // With `expression`, a class expression (docs/decorators.md, section 5),
  // whose name may be left out.
  ClassDecl *parseClass(bool expression = false);
  void parseClassMember(ClassDecl *cls);
  // Decorators (docs/decorators.md): `@Name`, `@a.b`, `@Name(args)` or
  // `@(expression)`, as many as there are.
  std::vector<Decorator> parseDecorators();
  Expr *parseDecoratorExpr();
  // Reports decorators where they cannot be: before anything but a class, a
  // member of one or a parameter.
  void rejectDecorators(llvm::ArrayRef<Decorator> decorators);
  bool memberModifierAhead();
  // After an error in a member: skips to the end of it.
  void skipClassMember();
  // The rest of a method after its name: type parameters, parameters, the
  // return type and the body. `start` is where the name starts.
  void parseMethodRest(FunctionDecl *fn, const char *start);
  // `<T, U extends X>` after a name, which tz does not support here.
  void skipTypeParams();
  // `[Symbol.iterator]` is ahead, a member name in brackets.
  bool symbolKeyAhead();
  // `[K in ...` is ahead, in an object type: a mapped type.
  bool mappedTypeAhead();
  TypeRef parseMappedType(TypeRef &object, int readonlyChange);

  // Destructuring: `[a, , b = 1, [c], ...rest]`, or with `object`
  // `{ a, b: c = 1, d: [e], [key]: f, ...rest }`.
  struct Pattern {
    struct Element {
      llvm::SMLoc loc;
      bool hole = false;
      bool rest = false;                // `...rest`, the last element
      llvm::StringRef name;             // a declaration's variable
      Expr *target = nullptr;           // what an assignment assigns to
      std::unique_ptr<Pattern> nested;  // or a nested pattern
      Expr *defaultValue = nullptr;     // `= value`, taken for undefined
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
  // At `[` or `{`.
  std::unique_ptr<Pattern> parseBindingPattern();
  std::unique_ptr<Pattern> parseObjectPattern();
  // The pattern an array or object literal on the left of `=` stands for.
  std::unique_ptr<Pattern> patternFromLiteral(Expr *lit);
  // Declares the variables of `pattern`, taking their values from `value`,
  // which a hidden variable holds.
  void declarePattern(VarDeclStmt *decl, const Pattern &pattern, Expr *value,
                      std::optional<TypeRef> typeAnn);
  // The same from the array or object in the variable `base`.
  void declareElements(VarDeclStmt *decl, const Pattern &pattern, llvm::StringRef base);
  // `[a, b] = value` or `({ a, b } = value)` as a statement.
  Stmt *assignPattern(llvm::SMLoc loc, const Pattern &pattern, Expr *value);
  // The value of element `i` of the array in `base`: `base[i]`, with the
  // default value, or `base.slice(i)` for the rest; of an object pattern,
  // `base.key` or `base[key]`, or for the rest a copy of the object without
  // the other fields.
  Expr *elementOf(llvm::StringRef base, const Pattern &pattern, size_t i,
                  std::vector<const IndexExpr *> &keyReads);
  VarDeclarator hiddenDeclarator(llvm::SMLoc loc, Expr *init);
  // The source text from `start` to the end of the last token, for messages.
  llvm::StringRef textFrom(const char *start) const;

  // Types
  TypeRef parseType();
  TypeRef parseUnionType();
  TypeRef parseIntersectionType();
  // A return type, which may be a type predicate: `x is T`, `asserts x is T`.
  TypeRef parseReturnType();
  TypeRef parseArrayType();
  TypeRef parsePrimaryType();
  // `(a: int, b?: string) => T`, with the lexer at `(`.
  bool functionTypeAhead();
  TypeRef parseFunctionType();
  // `(a: int, b?: string)` of a function type or a method in a type, as a
  // Function TypeRef without its result yet; false after a syntax error.
  bool parseSignatureParams(TypeRef &ref);
  TypeRef parseObjectType();
  // `[string, int]`, with the lexer at `[`.
  TypeRef parseTupleType();
  // The name of a field in an object type or literal: an identifier, a
  // keyword or a string literal.
  bool parseFieldName(llvm::StringRef &name);
  std::vector<TypeRef> parseTypeArgs();
  // `f<int>(x)` and `f<int>` in an expression: type arguments if the `<`
  // here starts some that a call or the end of the expression follows, as
  // TypeScript decides; otherwise `<` is less-than, and nothing is consumed.
  // With `callOnly`, only a call may follow (`JSON.parse<User>(text)`).
  bool tryTypeArgs(std::vector<TypeRef> &args, bool callOnly = false);

  // Expressions, from lowest to highest precedence. parseSequence admits the
  // comma operator; parseExpr, used where a comma separates (arguments, array
  // elements, declarators), does not.
  Expr *parseSequence();
  Expr *parseExpr();
  // At `yield`, inside a generator.
  Expr *parseYield();
  Expr *parseConditional();
  Expr *parseBinary(int minPrec);
  Expr *parseUnary();
  Expr *parsePostfix();
  Expr *parseCallOrMember();
  Expr *parsePrimary();
  // A template literal; `raw` keeps the texts as written, for String.raw.
  TemplateExpr *parseTemplate(bool raw = false);
  Expr *parseArrayLiteral();
  Expr *parseObjectLiteral();
  Expr *parseNew();
  Expr *parseIntLiteral();
  Expr *parseFloatLiteral();

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
  // Records `const name = "text"` (or a constant named so, declared before
  // it) for stringConstants.
  void recordConstant(llvm::StringRef name, const Expr *init);
};

} // namespace tz
