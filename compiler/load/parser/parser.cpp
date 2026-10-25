#include "compiler/load/parser/parser.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const char *opSpelling(UnaryOp op) {
  switch (op) {
  case UnaryOp::Neg: return "-";
  case UnaryOp::Plus: return "+";
  case UnaryOp::Not: return "!";
  case UnaryOp::BitNot: return "~";
  case UnaryOp::TypeOf: return "typeof";
  case UnaryOp::Void: return "void";
  case UnaryOp::Delete: return "delete";
  case UnaryOp::NonNull: return "!";
  }
  return "?";
}

const char *opSpelling(BinaryOp op) {
  switch (op) {
  case BinaryOp::Add: return "+";
  case BinaryOp::Sub: return "-";
  case BinaryOp::Mul: return "*";
  case BinaryOp::Div: return "/";
  case BinaryOp::Rem: return "%";
  case BinaryOp::Pow: return "**";
  case BinaryOp::BitAnd: return "&";
  case BinaryOp::BitOr: return "|";
  case BinaryOp::BitXor: return "^";
  case BinaryOp::Shl: return "<<";
  case BinaryOp::Shr: return ">>";
  case BinaryOp::UShr: return ">>>";
  case BinaryOp::Lt: return "<";
  case BinaryOp::Le: return "<=";
  case BinaryOp::Gt: return ">";
  case BinaryOp::Ge: return ">=";
  case BinaryOp::Eq: return "==";
  case BinaryOp::Ne: return "!=";
  case BinaryOp::LogAnd: return "&&";
  case BinaryOp::LogOr: return "||";
  case BinaryOp::Nullish: return "??";
  case BinaryOp::In: return "in";
  case BinaryOp::InstanceOf: return "instanceof";
  }
  return "?";
}

namespace {

// Binding strength of binary operators, following JavaScript. `as` and
// `satisfies` bind like the relational operators; they are parsed only to
// report that the language has neither.
constexpr int kRelationalPrec = 7;
constexpr int kPowPrec = 11; // `**`, the only right-associative one

int binaryPrec(Tok kind) {
  switch (kind) {
  case Tok::PipePipe:
  case Tok::QuestionQuestion: return 1;
  case Tok::AmpAmp: return 2;
  case Tok::Pipe: return 3;
  case Tok::Caret: return 4;
  case Tok::Amp: return 5;
  case Tok::EqEq:
  case Tok::EqEqEq:
  case Tok::BangEq:
  case Tok::BangEqEq: return 6;
  case Tok::Less:
  case Tok::LessEq:
  case Tok::Greater:
  case Tok::GreaterEq:
  case Tok::KwIn: return kRelationalPrec;
  case Tok::Shl:
  case Tok::Shr:
  case Tok::UShr: return 8;
  case Tok::Plus:
  case Tok::Minus: return 9;
  case Tok::Star:
  case Tok::Slash:
  case Tok::Percent: return 10;
  case Tok::StarStar: return kPowPrec;
  default: return -1;
  }
}

BinaryOp binaryOpFor(Tok kind) {
  switch (kind) {
  case Tok::PipePipe: return BinaryOp::LogOr;
  case Tok::QuestionQuestion: return BinaryOp::Nullish;
  case Tok::AmpAmp: return BinaryOp::LogAnd;
  case Tok::Pipe: return BinaryOp::BitOr;
  case Tok::Caret: return BinaryOp::BitXor;
  case Tok::Amp: return BinaryOp::BitAnd;
  case Tok::EqEq:
  case Tok::EqEqEq: return BinaryOp::Eq;
  case Tok::BangEq:
  case Tok::BangEqEq: return BinaryOp::Ne;
  case Tok::Less: return BinaryOp::Lt;
  case Tok::LessEq: return BinaryOp::Le;
  case Tok::Greater: return BinaryOp::Gt;
  case Tok::GreaterEq: return BinaryOp::Ge;
  case Tok::KwIn: return BinaryOp::In;
  case Tok::Shl: return BinaryOp::Shl;
  case Tok::Shr: return BinaryOp::Shr;
  case Tok::UShr: return BinaryOp::UShr;
  case Tok::Plus: return BinaryOp::Add;
  case Tok::Minus: return BinaryOp::Sub;
  case Tok::Star: return BinaryOp::Mul;
  case Tok::StarStar: return BinaryOp::Pow;
  case Tok::Slash: return BinaryOp::Div;
  default: return BinaryOp::Rem;
  }
}

// Returns true for assignment operators; `op` is set for compound ones.
bool assignmentOp(Tok kind, std::optional<BinaryOp> &op) {
  switch (kind) {
  case Tok::Assign: op.reset(); return true;
  case Tok::PlusAssign: op = BinaryOp::Add; return true;
  case Tok::MinusAssign: op = BinaryOp::Sub; return true;
  case Tok::StarAssign: op = BinaryOp::Mul; return true;
  case Tok::StarStarAssign: op = BinaryOp::Pow; return true;
  case Tok::SlashAssign: op = BinaryOp::Div; return true;
  case Tok::PercentAssign: op = BinaryOp::Rem; return true;
  case Tok::AmpAssign: op = BinaryOp::BitAnd; return true;
  case Tok::PipeAssign: op = BinaryOp::BitOr; return true;
  case Tok::CaretAssign: op = BinaryOp::BitXor; return true;
  case Tok::ShlAssign: op = BinaryOp::Shl; return true;
  case Tok::ShrAssign: op = BinaryOp::Shr; return true;
  case Tok::UShrAssign: op = BinaryOp::UShr; return true;
  case Tok::QuestionQuestionAssign: op = BinaryOp::Nullish; return true;
  case Tok::AmpAmpAssign: op = BinaryOp::LogAnd; return true;
  case Tok::PipePipeAssign: op = BinaryOp::LogOr; return true;
  default: return false;
  }
}

bool startsStatement(Tok kind) {
  switch (kind) {
  case Tok::KwLet:
  case Tok::KwConst:
  case Tok::KwVar:
  case Tok::KwFunction:
  case Tok::KwIf:
  case Tok::KwWhile:
  case Tok::KwDo:
  case Tok::KwFor:
  case Tok::KwSwitch:
  case Tok::KwCase:
  case Tok::KwDefault:
  case Tok::KwEnum:
  case Tok::KwReturn:
  case Tok::KwBreak:
  case Tok::KwContinue:
  case Tok::KwDebugger:
    return true;
  default:
    return false;
  }
}

} // namespace

Parser::Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags, bool library)
    : lexer_(lexer), ctx_(ctx), diags_(diags), library_(library) {
  advance();
}

//===----------------------------------------------------------------------===//
// Tokens and error recovery
//===----------------------------------------------------------------------===//

void Parser::advance() {
  prevEnd_ = tok_.text.end();
  if (hasPeeked_) {
    tok_ = peeked_;
    hasPeeked_ = false;
    return;
  }
  tok_ = lexer_.next();
}

const Token &Parser::peek() {
  if (!hasPeeked_) {
    peekState_ = lexer_.save();
    peeked_ = lexer_.next();
    hasPeeked_ = true;
  }
  return peeked_;
}

bool Parser::accept(Tok kind) {
  if (!tok_.is(kind))
    return false;
  advance();
  return true;
}

bool Parser::expect(Tok kind, llvm::StringRef context) {
  if (accept(kind))
    return true;
  std::string message = std::string("expected ") + tokSpelling(kind);
  if (!context.empty())
    message += " " + context.str();
  error(tok_.loc(), message + ", found " + tokSpelling(tok_.kind));
  return false;
}

// A statement ends with `;`, or, by automatic semicolon insertion, before `}`,
// at the end of the file, or at a line break.
void Parser::expectSemicolon() {
  if (accept(Tok::Semi))
    return;
  if (tok_.is(Tok::RBrace) || tok_.is(Tok::Eof) || tok_.newlineBefore)
    return;
  error(tok_.loc(), llvm::Twine("expected ';', found ") + tokSpelling(tok_.kind));
}

void Parser::error(llvm::SMLoc loc, const llvm::Twine &message) {
  if (recovering_)
    return;
  recovering_ = true;
  diags_.error(loc, message);
}

void Parser::report(llvm::SMLoc loc, const llvm::Twine &message) {
  if (!recovering_)
    diags_.error(loc, message);
}

Parser::Snapshot Parser::snapshot() const {
  return {lexer_.save(), peekState_, tok_, peeked_, prevEnd_, hasPeeked_, recovering_};
}

void Parser::rewind(Snapshot s) {
  lexer_.restore(std::move(s.lexer));
  peekState_ = std::move(s.peekState);
  tok_ = s.tok;
  peeked_ = s.peeked;
  prevEnd_ = s.prevEnd;
  hasPeeked_ = s.hasPeeked;
  recovering_ = s.recovering;
}

// Skips to a likely statement boundary after a syntax error.
void Parser::synchronize() {
  recovering_ = false;
  for (;;) {
    if (tok_.is(Tok::Eof) || tok_.is(Tok::RBrace))
      return;
    if (tok_.is(Tok::Semi)) {
      advance();
      return;
    }
    bool declaration =
        tok_.is(Tok::Identifier) &&
        (tok_.text == "type" || tok_.text == "interface" || tok_.text == "namespace" ||
         tok_.text == "module" || tok_.text == "declare" || tok_.text == "class") &&
        peek().is(Tok::Identifier) && !peek().newlineBefore;
    declaration |= tok_.is(Tok::Identifier) && (tok_.text == "import" || tok_.text == "export" ||
                                                 tok_.text == "throw" || tok_.text == "try");
    if (tok_.newlineBefore && (startsStatement(tok_.kind) || declaration))
      return;
    advance();
  }
}

void Parser::skipToClosingBrace() {
  for (unsigned depth = 0; !tok_.is(Tok::Eof); advance()) {
    if (tok_.is(Tok::LBrace)) {
      ++depth;
    } else if (tok_.is(Tok::RBrace)) {
      if (depth == 0) {
        advance();
        return;
      }
      --depth;
    }
  }
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

SourceFile Parser::parseSourceFile() {
  SourceFile file;
  while (!tok_.is(Tok::Eof)) {
    if (tok_.is(Tok::RBrace)) {
      error(tok_.loc(), "unexpected '}'");
      advance();
      recovering_ = false;
      continue;
    }
    file.items.push_back(parseStatement(/*topLevel=*/true));
    if (recovering_)
      synchronize();
  }
  file.fieldNames = std::move(fieldNames_);
  file.imports = std::move(imports_);
  file.exports = std::move(exports_);
  file.stringConstants = std::move(stringConstants_);
  return file;
}

Stmt *Parser::parseStatement(bool topLevel) {
  llvm::SMLoc loc = tok_.loc();
  switch (tok_.kind) {
  case Tok::At: {
    // `@Table("users") class User`, also before `export`.
    std::vector<Decorator> decorators = parseDecorators();
    Stmt *stmt = parseStatement(topLevel);
    if (auto *cls = dyn_cast<ClassDecl>(stmt))
      cls->decorators.insert(cls->decorators.begin(), decorators.begin(), decorators.end());
    else
      rejectDecorators(decorators);
    return stmt;
  }
  case Tok::LBrace:
    return parseBlock();
  case Tok::KwFunction:
    return parseFunctionDecl();
  case Tok::KwVar:
    report(loc, "'var' is not supported; use 'let' or 'const'");
    [[fallthrough]];
  case Tok::KwLet:
  case Tok::KwConst: {
    if (tok_.is(Tok::KwConst) && peek().is(Tok::KwEnum)) {
      advance(); // `const enum` is an enum like any other
      return parseEnum();
    }
    VarDeclStmt *decl = parseVarDecl();
    expectSemicolon();
    return decl;
  }
  case Tok::KwIf:
    return parseIf();
  case Tok::KwWhile:
    return parseWhile();
  case Tok::KwDo:
    return parseDoWhile();
  case Tok::KwFor:
    return parseFor();
  case Tok::KwSwitch:
    return parseSwitch();
  case Tok::KwEnum:
    return parseEnum();
  case Tok::KwReturn:
    return parseReturn();
  case Tok::KwBreak:
  case Tok::KwContinue:
    return parseBreakOrContinue();
  case Tok::KwDebugger:
    advance();
    expectSemicolon();
    return ctx_.make<DebuggerStmt>(loc);
  case Tok::Semi:
    advance();
    return ctx_.make<EmptyStmt>(loc);
  case Tok::Identifier:
    // `import(...)` and `import.meta` are expressions (see parsePrimary).
    if (tok_.text == "import" && !peek().is(Tok::LParen) && !peek().is(Tok::Dot))
      return parseImport(topLevel);
    if (tok_.text == "export")
      return parseExport(topLevel);
    // TypeScript's namespaces and declarations without code. A name, a
    // keyword or a string on the same line tells them from a variable.
    if ((tok_.text == "namespace" || tok_.text == "module" || tok_.text == "declare") &&
        (peek().is(Tok::Identifier) || isKeyword(peek().kind) || peek().is(Tok::StringLiteral)) &&
        !peek().newlineBefore) {
      if (tok_.text != "declare")
        return skipDeclaration(loc, "'" + tok_.text +
                                        "' is not supported: it is TypeScript's old way to group "
                                        "code; put the code in a file of its own and import it "
                                        "by name");
      llvm::StringRef what = peek().text;
      if (what == "module" || what == "global" || what == "namespace")
        return skipDeclaration(loc, "'declare " + what +
                                        "' is not supported: every file is a module of the "
                                        "program; import what it needs by name");
      return skipDeclaration(loc, "'declare' is not supported: the compiler needs the code of "
                                  "every declaration; write the declaration itself, or import it");
    }
    if (peek().is(Tok::Colon)) {
      llvm::StringRef label = tok_.text;
      advance();
      advance(); // ':'
      return ctx_.make<LabeledStmt>(loc, label, parseStatement());
    }
    // `type` declares an alias only when a name follows on the same line;
    // otherwise it is an ordinary name. The same for `interface`.
    if (tok_.text == "type" && peek().is(Tok::Identifier) && !peek().newlineBefore)
      return parseTypeAlias();
    if (tok_.text == "interface" && peek().is(Tok::Identifier) && !peek().newlineBefore)
      return parseInterface();
    if (tok_.text == "class" && peek().is(Tok::Identifier) && !peek().newlineBefore)
      return parseClass();
    // `throw` and `try` are reserved words: never a name.
    if (tok_.text == "throw")
      return parseThrow();
    if (tok_.text == "try" && peek().is(Tok::LBrace))
      return parseTry();
    if ((tok_.text == "catch" || tok_.text == "finally") &&
        (peek().is(Tok::LBrace) || peek().is(Tok::LParen))) {
      report(loc, "'" + tok_.text + "' without 'try': it goes right after the block of a 'try'");
      advance();
      if (accept(Tok::LParen))
        while (!tok_.is(Tok::Eof) && !accept(Tok::RParen))
          advance();
      parseBlock();
      return ctx_.make<EmptyStmt>(loc);
    }
    if (tok_.text == "abstract" && peek().is(Tok::Identifier) && peek().text == "class" &&
        !peek().newlineBefore) {
      advance();
      ClassDecl *cls = parseClass();
      cls->isAbstract = true;
      return cls;
    }
    // `async function f() {}`
    if (asyncFunctionAhead()) {
      advance();
      return parseFunctionDecl(/*isAsync=*/true);
    }
    [[fallthrough]];
  default: {
    Expr *expr = parseSequence();
    // `[a, b] = [b, a]` and `({ a, b } = o)`
    if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op)
      if (isa<ArrayLitExpr>(assign->target) || isa<ObjectLitExpr>(assign->target)) {
        Stmt *stmt = assignPattern(loc, *patternFromLiteral(assign->target), assign->value);
        expectSemicolon();
        return stmt;
      }
    expectSemicolon();
    return ctx_.make<ExprStmt>(loc, expr);
  }
  }
}

//===----------------------------------------------------------------------===//
// Imports and exports
//===----------------------------------------------------------------------===//

// `import { a, b as c } from "./x"`. The other forms of JavaScript and
// TypeScript are errors with a hint (docs/modules.md).
Stmt *Parser::parseImport(bool topLevel) {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'import'
  if (!topLevel)
    report(loc, "'import' is allowed only at the top level of a file");
  ImportDecl decl;
  decl.loc = loc;
  if (tok_.is(Tok::StringLiteral)) {
    // `import "./x"`, for what the file does when it loads.
    std::string path;
    parseFromPath(path, decl.pathLoc, /*from=*/false);
    report(loc, "an import must name what it takes: importing a file only to run it is not "
                "supported; import the names you need, e.g. 'import { a } from \"" + path + "\"'");
    expectSemicolon();
    return ctx_.make<EmptyStmt>(loc);
  }
  if (tok_.is(Tok::Identifier) && tok_.text == "type" && !peek().is(Tok::Comma) &&
      !(peek().is(Tok::Identifier) && peek().text == "from")) {
    report(tok_.loc(), "'import type' is not needed: types are imported like everything else; drop "
                       "'type'");
    advance();
  }
  // The forms that do not name the exports they take: reported once the path
  // is known, for the hint.
  enum class Form { Named, Namespace, Default } form = Form::Named;
  llvm::SMLoc formLoc = tok_.loc();
  llvm::StringRef defaultName;
  if (tok_.is(Tok::Star)) {
    // `import * as geo from "./geometry"`
    form = Form::Namespace;
    advance();
    if (expect(Tok::KwAs, "after 'import *'")) {
      if (tok_.is(Tok::Identifier))
        defaultName = tok_.text;
      expect(Tok::Identifier, "after 'import * as'");
    }
  } else if (tok_.is(Tok::Identifier) && !(tok_.text == "from" && peek().is(Tok::StringLiteral))) {
    // `import square from "./math"`, maybe followed by `, { ... }`.
    form = Form::Default;
    defaultName = tok_.text;
    advance();
    if (tok_.is(Tok::Assign)) {
      error(formLoc, "'import " + defaultName + " = ...' is not supported; import by name, e.g. "
                                                "'import { a } from \"./x\"'");
      return ctx_.make<EmptyStmt>(loc);
    }
    if (accept(Tok::Comma) && !tok_.is(Tok::LBrace))
      error(tok_.loc(), llvm::Twine("expected '{' after ',', found ") + tokSpelling(tok_.kind));
  }
  if (tok_.is(Tok::LBrace))
    parseModuleNames(decl.names, /*import=*/true);
  else if (form == Form::Named)
    error(tok_.loc(), llvm::Twine("expected '{' and the names to import, found ") +
                          tokSpelling(tok_.kind));
  if (recovering_ || !parseFromPath(decl.path, decl.pathLoc))
    return ctx_.make<EmptyStmt>(loc);
  if (tok_.is(Tok::Identifier) && (tok_.text == "with" || tok_.text == "assert") &&
      !tok_.newlineBefore) {
    report(tok_.loc(), "import attributes are not supported; data files will be embedded with "
                       "embedJson and embedText");
    advance();
    if (tok_.is(Tok::LBrace)) {
      advance();
      skipToClosingBrace();
    }
  }
  expectSemicolon();
  if (form != Form::Named && isBuiltinModule(decl.path) && !defaultName.empty()) {
    // Sema reports it with the names the file uses: `fs.readFileSync`.
    decl.whole = defaultName;
    decl.wholeLoc = formLoc;
    decl.wholeIsDefault = form == Form::Default;
    imports_.push_back(std::move(decl));
    return ctx_.make<EmptyStmt>(loc);
  }
  if (form == Form::Namespace)
    report(formLoc, "'import * as' is not supported; list the names you need, e.g. 'import { a, "
                    "b } from \"" + decl.path + "\"'");
  else if (form == Form::Default)
    report(formLoc, "default imports are not supported: there is no 'export default'; import by "
                    "name, e.g. 'import { " + defaultName + " } from \"" + decl.path + "\"'");
  else if (decl.names.empty()) // `import {} from "./x"`, to run the file
    report(loc, "an import must name what it takes: importing a file only to run it is not "
                "supported; import the names you need, e.g. 'import { a } from \"" +
                    decl.path + "\"'");
  // The names in braces are imported even next to a form that is not.
  if (!decl.names.empty())
    imports_.push_back(std::move(decl));
  return ctx_.make<EmptyStmt>(loc);
}

// `export` before a declaration, `export { a, b as c }`, `export { a } from
// "./x"` and `export * from "./x"`.
Stmt *Parser::parseExport(bool topLevel) {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'export'
  if (!topLevel)
    report(loc, "'export' is allowed only at the top level");
  if (tok_.is(Tok::At)) {
    // `export @Table("users") class User`
    std::vector<Decorator> decorators = parseDecorators();
    Stmt *stmt = parseStatement(topLevel);
    if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      if (topLevel)
        exportDeclared(cls);
      cls->decorators.insert(cls->decorators.begin(), decorators.begin(), decorators.end());
    } else {
      rejectDecorators(decorators);
    }
    return stmt;
  }
  const Token &next = peek();
  bool declaration =
      tok_.is(Tok::KwFunction) || tok_.is(Tok::KwConst) || tok_.is(Tok::KwLet) ||
      tok_.is(Tok::KwVar) || tok_.is(Tok::KwEnum) || asyncFunctionAhead() ||
      (tok_.is(Tok::Identifier) &&
       (tok_.text == "type" || tok_.text == "interface" || tok_.text == "class" ||
        (tok_.text == "abstract" && next.text == "class")) &&
       next.is(Tok::Identifier) && !next.newlineBefore);
  if (declaration) {
    Stmt *stmt = parseStatement(topLevel);
    if (topLevel)
      exportDeclared(stmt);
    return stmt;
  }
  if (tok_.is(Tok::KwDefault)) {
    report(loc, "'export default' is not supported: everything is exported by name; export a "
                "declaration, e.g. 'export function f() {...}', and import it with 'import { f } "
                "from ...'");
    advance();
    // What follows is still checked.
    if (tok_.is(Tok::KwFunction) && peek().is(Tok::Identifier))
      return parseStatement(topLevel);
    llvm::SMLoc exprLoc = tok_.loc();
    Expr *expr = tok_.is(Tok::KwFunction) ? ctx_.make<FunctionExpr>(exprLoc, parseFunction(true))
                                          : parseExpr();
    expectSemicolon();
    return ctx_.make<ExprStmt>(exprLoc, expr);
  }
  if (tok_.is(Tok::Assign)) {
    report(loc, "'export =' is not supported; export by name, e.g. 'export function f() {...}'");
    advance();
    parseExpr();
    expectSemicolon();
    return ctx_.make<EmptyStmt>(loc);
  }
  ExportDecl decl;
  decl.loc = loc;
  if (tok_.is(Tok::Star)) {
    advance();
    if (tok_.is(Tok::KwAs)) {
      // `export * as geo from "./geometry"`
      llvm::SMLoc asLoc = tok_.loc();
      advance();
      expect(Tok::Identifier, "after 'export * as'");
      if (!recovering_ && parseFromPath(decl.path, decl.pathLoc)) {
        report(asLoc, "'export * as' is not supported; re-export the names, e.g. 'export { a, b } "
                      "from \"" + decl.path + "\"', or all of them with 'export * from \"" +
                          decl.path + "\"'");
        expectSemicolon();
      }
      return ctx_.make<EmptyStmt>(loc);
    }
    decl.all = true;
    if (parseFromPath(decl.path, decl.pathLoc)) {
      expectSemicolon();
      exports_.push_back(std::move(decl));
    }
    return ctx_.make<EmptyStmt>(loc);
  }
  if (tok_.is(Tok::Identifier) && tok_.text == "type" && next.is(Tok::LBrace)) {
    report(tok_.loc(), "'export type' is not needed: types are exported like everything else; drop "
                       "'type'");
    advance();
  }
  if (!tok_.is(Tok::LBrace)) {
    error(tok_.loc(), llvm::Twine("expected a declaration or '{' after 'export', found ") +
                          tokSpelling(tok_.kind));
    return ctx_.make<EmptyStmt>(loc);
  }
  parseModuleNames(decl.names, /*import=*/false);
  if (!recovering_ && tok_.is(Tok::Identifier) && tok_.text == "from" &&
      !parseFromPath(decl.path, decl.pathLoc))
    return ctx_.make<EmptyStmt>(loc);
  expectSemicolon();
  if (!recovering_)
    exports_.push_back(std::move(decl));
  return ctx_.make<EmptyStmt>(loc);
}

void Parser::parseModuleNames(std::vector<ModuleName> &names, bool import) {
  advance(); // '{'
  const char *keyword = import ? "import" : "export";
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    // `{ type T }`
    if (tok_.is(Tok::Identifier) && tok_.text == "type" && peek().is(Tok::Identifier) &&
        peek().text != "as") {
      report(tok_.loc(), llvm::Twine("'type' is not needed in an ") + keyword +
                             ": types are " + keyword + "ed like everything else; drop it");
      advance();
    }
    ModuleName name;
    name.loc = tok_.loc();
    if (tok_.is(Tok::KwDefault)) {
      error(tok_.loc(),
            "there is no 'export default'; everything is exported and imported by name");
      return;
    }
    if (!tok_.is(Tok::Identifier)) {
      error(tok_.loc(),
            llvm::Twine("expected a name to ") + keyword + ", found " + tokSpelling(tok_.kind));
      return;
    }
    name.name = tok_.text;
    advance();
    name.as = name.name;
    name.asLoc = name.loc;
    if (accept(Tok::KwAs)) {
      name.asLoc = tok_.loc();
      if (tok_.is(Tok::KwDefault)) {
        error(tok_.loc(),
              "there is no 'export default'; everything is exported and imported by name");
        return;
      }
      if (!tok_.is(Tok::Identifier)) {
        error(tok_.loc(),
              llvm::Twine("expected a name after 'as', found ") + tokSpelling(tok_.kind));
        return;
      }
      name.as = tok_.text;
      advance();
    }
    names.push_back(name);
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RBrace, import ? "to close the names to import" : "to close the names to export");
}

bool Parser::parseFromPath(std::string &path, llvm::SMLoc &loc, bool from) {
  if (from && !(tok_.is(Tok::Identifier) && tok_.text == "from")) {
    error(tok_.loc(), llvm::Twine("expected 'from' and the path of the file, e.g. 'from "
                                  "\"./math\"', found ") +
                          tokSpelling(tok_.kind));
    return false;
  }
  if (from)
    advance();
  loc = tok_.loc();
  if (!tok_.is(Tok::StringLiteral)) {
    error(tok_.loc(),
          llvm::Twine("expected the path of the file as a string, e.g. '\"./math\"', found ") +
              tokSpelling(tok_.kind));
    return false;
  }
  std::u16string text;
  decodeStringLiteral(tok_, text, diags_);
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      path);
  advance();
  return true;
}

void Parser::exportDeclared(const Stmt *stmt) {
  ExportDecl decl;
  decl.loc = stmt->loc();
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc) {
    decl.names.push_back({name, loc, name, loc});
  };
  if (auto *fn = dyn_cast<FunctionDecl>(stmt))
    add(fn->name, fn->loc());
  else if (auto *alias = dyn_cast<TypeAliasDecl>(stmt))
    add(alias->name, alias->loc());
  else if (auto *e = dyn_cast<EnumDecl>(stmt))
    add(e->name, e->loc());
  else if (auto *cls = dyn_cast<ClassDecl>(stmt))
    add(cls->name, cls->loc());
  else if (auto *var = dyn_cast<VarDeclStmt>(stmt))
    for (const VarDeclarator &d : var->decls)
      if (!d.name.starts_with("%")) // not a hidden variable of destructuring
        add(d.name, d.loc);
  if (!decl.names.empty())
    exports_.push_back(std::move(decl));
}

Stmt *Parser::skipDeclaration(llvm::SMLoc loc, const llvm::Twine &message) {
  report(loc, message);
  advance();
  // To the end of the statement: past a body in braces, a `;`, or the end of
  // the line.
  while (!tok_.is(Tok::Eof) && !tok_.is(Tok::RBrace)) {
    if (tok_.is(Tok::LBrace)) {
      advance();
      skipToClosingBrace();
      break;
    }
    if (accept(Tok::Semi))
      break;
    advance();
    if (tok_.newlineBefore)
      break;
  }
  return ctx_.make<EmptyStmt>(loc);
}

Stmt *Parser::parseBreakOrContinue() {
  llvm::SMLoc loc = tok_.loc();
  bool isBreak = tok_.is(Tok::KwBreak);
  advance();
  llvm::StringRef label;
  if (tok_.is(Tok::Identifier) && !tok_.newlineBefore) {
    label = tok_.text;
    advance();
  }
  expectSemicolon();
  if (isBreak)
    return ctx_.make<BreakStmt>(loc, label);
  return ctx_.make<ContinueStmt>(loc, label);
}

// `type Name = T`, `type Name<T> = ...`
Stmt *Parser::parseTypeAlias() {
  advance(); // 'type'
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef name = tok_.text;
  advance();
  std::vector<TypeParamDecl> typeParams;
  if (tok_.is(Tok::Less))
    typeParams = parseTypeParams();
  expect(Tok::Assign, "after the name of the type");
  TypeRef type = parseType();
  expectSemicolon();
  auto *alias = ctx_.make<TypeAliasDecl>(loc, name, std::move(type));
  alias->typeParams = std::move(typeParams);
  return alias;
}

// `interface Name { fields }`: the name of an object type, as with `type`.
Stmt *Parser::parseInterface() {
  advance(); // 'interface'
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef name = tok_.text;
  advance();
  std::vector<TypeParamDecl> typeParams;
  if (tok_.is(Tok::Less))
    typeParams = parseTypeParams();
  std::vector<TypeRef> bases;
  if (tok_.is(Tok::Identifier) && tok_.text == "extends") {
    advance();
    do
      bases.push_back(parseType());
    while (accept(Tok::Comma));
  }
  if (!tok_.is(Tok::LBrace)) {
    error(tok_.loc(), llvm::Twine("expected '{' to start the fields of the interface, found ") +
                          tokSpelling(tok_.kind));
    return ctx_.make<EmptyStmt>(loc);
  }
  auto *decl = ctx_.make<TypeAliasDecl>(loc, name, parseObjectType());
  decl->bases = std::move(bases);
  decl->typeParams = std::move(typeParams);
  accept(Tok::Semi);
  return decl;
}

// `enum Name { A, B = value, ... }`
Stmt *Parser::parseEnum() {
  advance(); // 'enum'
  llvm::SMLoc loc = tok_.loc();
  if (!tok_.is(Tok::Identifier)) {
    error(tok_.loc(), "expected the name of the enum");
    return ctx_.make<EmptyStmt>(loc);
  }
  auto *decl = ctx_.make<EnumDecl>(loc, tok_.text);
  advance();
  expect(Tok::LBrace, "to start the members of the enum");
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    EnumMember member;
    member.loc = tok_.loc();
    if (!tok_.is(Tok::Identifier)) {
      error(tok_.loc(), llvm::Twine("expected the name of an enum member, found ") +
                            tokSpelling(tok_.kind));
      break;
    }
    member.name = tok_.text;
    advance();
    if (accept(Tok::Assign)) {
      member.init = parseExpr();
      recordConstant((decl->name + "." + member.name).str(), member.init);
    }
    decl->members.push_back(member);
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RBrace, "to close the enum");
  return decl;
}

//===----------------------------------------------------------------------===//
// Classes
//===----------------------------------------------------------------------===//

// `class Name<T> extends Base implements A, B { members }`. The text of a
// generic class is kept, for Sema to parse a copy of it for each list of type
// arguments.
ClassDecl *Parser::parseClass(bool expression) {
  const char *start = tok_.text.data();
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'class'
  // A class expression may have no name: `class extends Base { ... }`.
  bool named = tok_.is(Tok::Identifier) &&
               !(expression && (tok_.text == "extends" || tok_.text == "implements"));
  auto *cls = ctx_.make<ClassDecl>(named ? tok_.loc() : loc, named ? tok_.text : llvm::StringRef());
  cls->expression = expression;
  if (named)
    advance(); // the name
  if (tok_.is(Tok::Less))
    cls->typeParams = parseTypeParams();
  if (tok_.is(Tok::Identifier) && tok_.text == "extends") {
    advance();
    // `extends Base` and `extends Box<int>` name a class; `extends
    // Mixin(Base)` and `extends ns.Base` are values (see ClassDecl::baseExpr),
    // and so is a plain name in a class expression, usually a parameter.
    const Token &next = peek();
    bool value = !tok_.is(Tok::Identifier) || next.is(Tok::LParen) || next.is(Tok::Dot) ||
                 next.is(Tok::LBracket);
    if (value) {
      cls->baseExpr = parseCallOrMember();
    } else {
      llvm::SMLoc baseLoc = tok_.loc();
      llvm::StringRef baseName = tok_.text;
      cls->base = parsePrimaryType();
      if (expression && cls->base->kind == TypeRef::Kind::Name)
        cls->baseExpr = ctx_.make<NameExpr>(baseLoc, baseName);
    }
  }
  if (tok_.is(Tok::Identifier) && tok_.text == "implements") {
    advance();
    do
      cls->interfaces.push_back(parsePrimaryType());
    while (accept(Tok::Comma));
  }
  if (!expect(Tok::LBrace, "to start the members of the class"))
    return cls;
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    parseClassMember(cls);
    if (recovering_) {
      skipClassMember();
      recovering_ = false;
    }
  }
  expect(Tok::RBrace, "to close the class");
  if (!cls->typeParams.empty())
    cls->source = textFrom(start);
  return cls;
}

ClassDecl *Parser::parseClassCopy() { return parseClass(); }

bool Parser::memberModifierAhead() {
  static const char *const modifiers[] = {"public",   "private",  "protected", "readonly",
                                          "static",   "override", "abstract",  "declare",
                                          "accessor", "async"};
  if (!tok_.is(Tok::Identifier) || !llvm::is_contained(modifiers, tok_.text))
    return false;
  // A member named like a modifier ends right after its name.
  const Token &next = peek();
  if (next.newlineBefore && !next.is(Tok::LBrace))
    return false;
  switch (next.kind) {
  case Tok::LParen:
  case Tok::Colon:
  case Tok::Assign:
  case Tok::Semi:
  case Tok::Question:
  case Tok::Bang:
  case Tok::Less:
  case Tok::RBrace:
  case Tok::Eof:
    return false;
  default:
    return true;
  }
}

void Parser::skipClassMember() {
  bool moved = false;
  while (!tok_.is(Tok::Eof) && !tok_.is(Tok::RBrace)) {
    if (moved && tok_.newlineBefore)
      return;
    if (accept(Tok::Semi))
      return;
    if (tok_.is(Tok::LBrace)) {
      advance();
      skipToClosingBrace();
    } else {
      advance();
    }
    moved = true;
  }
}

std::vector<Decorator> Parser::parseDecorators() {
  std::vector<Decorator> decorators;
  while (tok_.is(Tok::At)) {
    llvm::SMLoc loc = tok_.loc();
    advance();
    if (Expr *expr = parseDecoratorExpr())
      decorators.push_back({loc, expr});
    if (recovering_)
      break;
  }
  return decorators;
}

// As TypeScript has them: a name or a chain of member names, which a call
// may end, or any expression in parentheses.
Expr *Parser::parseDecoratorExpr() {
  if (accept(Tok::LParen)) {
    Expr *expr = parseSequence();
    expect(Tok::RParen, "to close the decorator");
    return expr;
  }
  if (!tok_.is(Tok::Identifier)) {
    error(tok_.loc(), llvm::Twine("expected the name of a decorator after '@', found ") +
                          tokSpelling(tok_.kind));
    return nullptr;
  }
  Expr *expr = ctx_.make<NameExpr>(tok_.loc(), tok_.text);
  advance();
  while (tok_.is(Tok::Dot)) {
    advance();
    if (!tok_.is(Tok::Identifier) && !isKeyword(tok_.kind)) {
      error(tok_.loc(), "expected a property name after '.'");
      return expr;
    }
    expr = ctx_.make<MemberExpr>(tok_.loc(), expr, tok_.text);
    advance();
  }
  if (!tok_.is(Tok::LParen))
    return expr;
  llvm::SMLoc loc = expr->loc();
  advance();
  llvm::SaveAndRestore<bool> allowIn(noIn_, false);
  std::vector<Expr *> args;
  std::vector<bool> spread;
  while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
    bool spreads = accept(Tok::Ellipsis);
    if (spreads || !spread.empty()) {
      spread.resize(args.size(), false);
      spread.push_back(spreads);
    }
    args.push_back(parseExpr());
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RParen, "to close the argument list");
  auto *call = ctx_.make<CallExpr>(loc, expr, std::move(args));
  call->spread = std::move(spread);
  return call;
}

void Parser::rejectDecorators(llvm::ArrayRef<Decorator> decorators) {
  if (!decorators.empty())
    report(decorators[0].loc, "a decorator can decorate only a class, a member of a class or a "
                              "parameter of a method or a constructor");
}

void Parser::skipTypeParams() {
  int depth = 0;
  do {
    if (tok_.is(Tok::Less))
      ++depth;
    else if (tok_.is(Tok::Greater))
      --depth;
    else if (tok_.is(Tok::Shr))
      depth -= 2;
    else if (tok_.is(Tok::Eof) || tok_.is(Tok::LBrace) || tok_.is(Tok::LParen))
      return;
    advance();
  } while (depth > 0);
}

// A field, a method or a constructor, with its modifiers.
void Parser::parseClassMember(ClassDecl *cls) {
  if (accept(Tok::Semi))
    return;
  std::vector<Decorator> decorators = parseDecorators();
  if (recovering_)
    return;
  llvm::SMLoc loc = tok_.loc();
  bool isStatic = false, readonly = false, isAbstract = false, isOverride = false;
  bool isAsync = false;
  bool accessor = false;
  Access access = Access::Public;
  while (memberModifierAhead()) {
    llvm::StringRef word = tok_.text;
    llvm::SMLoc at = tok_.loc();
    advance();
    if (word == "static") {
      if (tok_.is(Tok::LBrace)) {
        report(at, "static blocks are not supported; give each static field its value in its "
                   "declaration");
        advance();
        skipToClosingBrace();
        return;
      }
      isStatic = true;
    } else if (word == "public") {
      access = Access::Public;
    } else if (word == "private") {
      access = Access::Private;
    } else if (word == "protected") {
      access = Access::Protected;
    } else if (word == "readonly") {
      readonly = true;
    } else if (word == "override") {
      isOverride = true;
    } else if (word == "abstract") {
      isAbstract = true;
    } else if (word == "declare") {
      report(at, "'declare' is not supported: a field gets its value in its declaration or in the "
                 "constructor");
    } else if (word == "accessor") {
      // An ordinary field: reading or writing a field never runs code
      // (docs/decorators.md, section 1).
      accessor = true;
    } else {
      isAsync = true; // `async m() {}`
    }
  }
  // `get area() {}` and `set width(v) {}`: reading a field never runs code,
  // but in a built-in module of tz written in tz, which has them as node's
  // classes do (see FunctionDecl::accessor).
  FunctionDecl::Accessor accessorKind = FunctionDecl::Accessor::None;
  bool accessorAhead =
      tok_.is(Tok::Identifier) && (tok_.text == "get" || tok_.text == "set") &&
      !peek().newlineBefore &&
      (peek().is(Tok::Identifier) || isKeyword(peek().kind) || peek().is(Tok::StringLiteral));
  if (accessorAhead && library_) {
    accessorKind = tok_.text == "get" ? FunctionDecl::Accessor::Getter
                                      : FunctionDecl::Accessor::Setter;
    advance();
  } else if (accessorAhead) {
    bool getter = tok_.text == "get";
    advance();
    std::string name = tok_.text.str();
    std::string setter = name.empty() ? "set" : "set" + std::string(1, llvm::toUpper(name[0])) +
                                                    name.substr(1);
    if (getter)
      report(loc, "getters are not supported: reading a field never runs code; write a method "
                  "instead, e.g. '" + name + "()'");
    else
      report(loc, "setters are not supported: writing a field never runs code; write a method "
                  "instead, e.g. '" + setter + "(value)'");
    skipClassMember();
    return;
  }
  if (tok_.is(Tok::LBracket) && !symbolKeyAhead()) {
    report(loc, "a member name in brackets is not supported: an object has a fixed set of fields; "
                "keep a dictionary in a field, e.g. 'items: Record<string, T> = {}' or "
                "'items = new Map<string, T>()'");
    skipClassMember();
    return;
  }
  const char *start = tok_.text.data();
  bool generator = accept(Tok::Star); // `*items() { yield ... }`
  llvm::SMLoc nameLoc = tok_.loc();
  bool plainName = tok_.is(Tok::Identifier);
  llvm::StringRef name;
  if (!parseFieldName(name))
    return;
  if (name.starts_with("#"))
    access = Access::Private;

  if (tok_.is(Tok::LParen) || tok_.is(Tok::Less)) {
    auto *fn = ctx_.make<FunctionDecl>(nameLoc, name);
    fn->cls = cls;
    fn->isStatic = isStatic;
    fn->accessor = accessorKind;
    if (accessorKind != FunctionDecl::Accessor::None)
      llvm::erase_if(decorators, [&](const Decorator &d) {
        auto *name = dyn_cast<NameExpr>(d.expr);
        if (!name || name->name != "__tz_hidden")
          return false;
        fn->hiddenGetter = true;
        return true;
      });
    if (accessor)
      report(loc, "'accessor' goes before a field, not a method");
    // The class gets a constructor's parameters, and their decorators.
    if (!decorators.empty() && plainName && name == "constructor" && !isStatic)
      report(decorators[0].loc, "a decorator cannot decorate a constructor; decorate the class, "
                                "or a parameter of the constructor");
    else
      fn->decorators = std::move(decorators);
    fn->access = access;
    fn->isConstructor = plainName && name == "constructor" && !isStatic;
    fn->isGenerator = generator && !fn->isConstructor;
    fn->isAsync = isAsync && !fn->isConstructor;
    fn->isOverride = isOverride && !fn->isConstructor;
    fn->isAbstract = isAbstract && !fn->isConstructor;
    if (generator && fn->isConstructor)
      report(loc, "a constructor cannot be a generator: 'new' gives the object");
    if (isAsync && fn->isConstructor)
      report(loc, "a constructor cannot be 'async': 'new' gives the object, not a Promise");
    if (readonly)
      report(loc, "a method cannot be 'readonly': a method cannot be replaced anyway");
    if ((isOverride || isAbstract) && fn->isConstructor)
      report(loc, std::string("a constructor cannot be '") +
                      (isOverride ? "override" : "abstract") + "'");
    if (isAbstract && isStatic)
      report(loc, "a static method cannot be 'abstract': it belongs to the class itself");
    parseMethodRest(fn, start);
    if (recovering_)
      return;
    if (fn->isAbstract) {
      if (fn->body)
        report(nameLoc, "abstract method '" + name + "' cannot have a body: the classes that "
                                                     "extend '" + cls->name +
                            "' give it one; to give it one here, drop 'abstract'");
      // An empty body, which nothing runs: the method table holds what the
      // classes that extend it give.
      fn->body = ctx_.make<BlockStmt>(nameLoc);
      cls->methods.push_back(fn);
      return;
    }
    if (!fn->body) {
      std::string what = fn->isConstructor ? "the constructor" : "method '" + name.str() + "'";
      report(nameLoc, what + " has no body; overload signatures over one body are not "
                             "supported: give each variant a body of its own");
      return;
    }
    if (!fn->isConstructor) {
      cls->methods.push_back(fn);
      return;
    }
    if (fn->returnType)
      report(fn->returnType->loc, "a constructor cannot have a return type");
    // Parameter properties are fields too, which the constructor sets before
    // anything else.
    std::vector<Stmt *> sets;
    size_t properties = 0;
    for (const ClassField &f : cls->fields)
      properties += f.parameter;
    for (const Param &param : fn->params) {
      if (!param.property)
        continue;
      fieldNames_.insert(param.name);
      if (!llvm::any_of(cls->fields, [&](const ClassField &f) {
            return f.parameter && f.name == param.name;
          })) {
        ClassField field;
        field.name = param.name;
        field.loc = param.loc;
        field.type = param.type;
        field.readonly = param.readonly;
        field.access = param.access;
        field.parameter = true;
        cls->fields.insert(cls->fields.begin() + std::ptrdiff_t(properties++), field);
      }
      auto *target =
          ctx_.make<MemberExpr>(param.loc, ctx_.make<NameExpr>(param.loc, "this"), param.name);
      auto *set = ctx_.make<AssignExpr>(param.loc, std::nullopt, target,
                                        ctx_.make<NameExpr>(param.loc, param.name));
      sets.push_back(ctx_.make<ExprStmt>(param.loc, set));
    }
    // In a class that extends another, `this` exists only once `super(...)`
    // has run: the fields are set right after it, as in JavaScript.
    auto at = fn->body->body.begin();
    if (cls->base)
      for (auto it = fn->body->body.begin(); it != fn->body->body.end(); ++it) {
        auto *stmt = dyn_cast<ExprStmt>(*it);
        auto *call = stmt ? dyn_cast<CallExpr>(stmt->expr) : nullptr;
        auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
        if (callee && callee->name == "super") {
          at = it + 1;
          break;
        }
      }
    fn->body->body.insert(at, sets.begin(), sets.end());
    cls->constructors.push_back(fn);
    return;
  }

  if (generator || isAsync) {
    error(tok_.loc(), generator ? "expected '(' after the name of the generator method"
                                : "expected '(' after the name of the async method");
    return;
  }
  ClassField field;
  field.name = name;
  field.loc = nameLoc;
  field.isStatic = isStatic;
  field.readonly = readonly;
  field.access = access;
  field.isOverride = isOverride;
  field.accessor = accessor;
  field.decorators = std::move(decorators);
  if (isAbstract)
    report(loc, "abstract fields are not supported: declare the field in the class that sets it, "
                "or give it a value here");
  if (accept(Tok::Question)) {
    field.optional = true;
  } else if (tok_.is(Tok::Bang) && !tok_.newlineBefore) {
    advance();
    field.definite = true;
  }
  if (accept(Tok::Colon))
    field.type = parseType();
  if (accept(Tok::Assign))
    field.init = parseExpr();
  if (!accept(Tok::Semi) && !accept(Tok::Comma) && !tok_.is(Tok::RBrace) && !tok_.newlineBefore) {
    error(tok_.loc(), llvm::Twine("expected ';' after the field, found ") + tokSpelling(tok_.kind));
    return;
  }
  cls->fields.push_back(std::move(field));
}

void Parser::parseMethodRest(FunctionDecl *fn, const char *start) {
  // `yield` belongs to the generator method whose body it is in.
  llvm::SaveAndRestore<bool> inGenerator(inGenerator_, fn->isGenerator);
  if (tok_.is(Tok::Less))
    fn->typeParams = parseTypeParams();
  ParamPatterns patterns;
  parseParams(fn, &patterns);
  if (accept(Tok::Colon))
    fn->returnType = parseReturnType();
  if (tok_.is(Tok::LBrace)) {
    fn->body = parseBlock();
    addPrologue(fn, patterns);
  } else if (!accept(Tok::Semi) && !tok_.newlineBefore && !tok_.is(Tok::RBrace) &&
             !tok_.is(Tok::Eof)) {
    error(tok_.loc(), "expected '{' to start the body of the method");
  }
  fn->source = textFrom(start);
}

FunctionDecl *Parser::parseMethodCopy() {
  const char *start = tok_.text.data();
  bool generator = accept(Tok::Star);
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef name = tok_.text;
  bool constructor = tok_.is(Tok::Identifier) && name == "constructor";
  advance();
  auto *fn = ctx_.make<FunctionDecl>(loc, name);
  fn->isConstructor = constructor;
  fn->isGenerator = generator;
  parseMethodRest(fn, start);
  return fn;
}

// `switch (x) { case a: ... default: ... }`
Stmt *Parser::parseSwitch() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'switch'
  expect(Tok::LParen, "after 'switch'");
  Expr *discriminant = parseSequence();
  expect(Tok::RParen, "after the value of 'switch'");
  auto *stmt = ctx_.make<SwitchStmt>(loc, discriminant);
  expect(Tok::LBrace, "to start the cases of 'switch'");
  bool hasDefault = false;
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    SwitchCase c;
    c.loc = tok_.loc();
    if (accept(Tok::KwCase)) {
      c.test = parseExpr();
    } else if (tok_.is(Tok::KwDefault)) {
      if (hasDefault)
        report(tok_.loc(), "a 'switch' can have only one 'default'");
      hasDefault = true;
      advance();
    } else {
      error(tok_.loc(), llvm::Twine("expected 'case' or 'default', found ") + tokSpelling(tok_.kind));
      break;
    }
    expect(Tok::Colon, c.test ? "after the value of 'case'" : "after 'default'");
    while (!tok_.is(Tok::KwCase) && !tok_.is(Tok::KwDefault) && !tok_.is(Tok::RBrace) &&
           !tok_.is(Tok::Eof)) {
      c.body.push_back(parseStatement());
      if (recovering_)
        synchronize();
    }
    stmt->cases.push_back(std::move(c));
  }
  expect(Tok::RBrace, "to close 'switch'");
  return stmt;
}

BlockStmt *Parser::parseBlock() {
  auto *block = ctx_.make<BlockStmt>(tok_.loc());
  expect(Tok::LBrace);
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    block->body.push_back(parseStatement());
    if (recovering_)
      synchronize();
  }
  expect(Tok::RBrace, "to close the block");
  return block;
}

// A function declaration. TypeScript's overload signatures (`function f(x:
// int): int` without a body, before the implementation, which then works out
// what it was given) are an error: a plain function has one body, and a
// variant with other parameters is a function of its own, with its own name.
// The signatures are skipped and the implementation is kept, so that the rest
// of the program is still checked.
Stmt *Parser::parseFunctionDecl(bool isAsync) {
  FunctionDecl *fn = parseFunction(/*expression=*/false, isAsync);
  if (fn->body)
    return fn;
  // An intrinsic of a built-in module written in tz: the runtime has its
  // code (see FunctionDecl::external).
  if (library_ && fn->name.starts_with("__tz_")) {
    fn->external = true;
    fn->body = ctx_.make<BlockStmt>(fn->loc());
    return fn;
  }
  llvm::StringRef name = fn->name;
  llvm::SMLoc loc = fn->loc();
  while (!fn->body) {
    if (tok_.is(Tok::Identifier) && tok_.text == "export" && peek().is(Tok::KwFunction))
      advance();
    if (!tok_.is(Tok::KwFunction) || peek().text != name) {
      report(loc, "function '" + name + "' has no body; add it after the parameters: '{ ... }'");
      return ctx_.make<EmptyStmt>(loc);
    }
    fn = parseFunction(/*expression=*/false, isAsync);
  }
  report(loc, "overload signatures are not supported: plain functions are not overloaded; give each "
              "variant its own name and body");
  return fn;
}

FunctionDecl *Parser::parseFunction(bool expression, bool isAsync) {
  const char *start = tok_.text.data();
  llvm::SMLoc keywordLoc = tok_.loc();
  advance(); // 'function'
  bool generator = accept(Tok::Star);

  llvm::StringRef name;
  llvm::SMLoc nameLoc = tok_.loc();
  if (tok_.is(Tok::Identifier)) {
    name = tok_.text;
    advance();
  } else if (expression) {
    nameLoc = keywordLoc;
  } else {
    error(tok_.loc(), "expected a function name");
  }
  auto *fn = ctx_.make<FunctionDecl>(nameLoc, name);
  fn->isExpression = expression;
  fn->isGenerator = generator;
  fn->isAsync = isAsync;
  if (expression)
    fn->ownName = name;
  if (tok_.is(Tok::Less))
    fn->typeParams = parseTypeParams();
  // `yield` is an operator only in the body of a generator.
  bool outerGenerator = inGenerator_;
  inGenerator_ = generator;
  ParamPatterns patterns;
  parseParams(fn, &patterns);

  if (accept(Tok::Colon))
    fn->returnType = parseReturnType();

  if (tok_.is(Tok::LBrace)) {
    // `yield` belongs to the generator whose body it is in.
    llvm::SaveAndRestore<bool> inGenerator(inGenerator_, generator);
    fn->body = parseBlock();
    addPrologue(fn, patterns);
  } else if (!expression &&
             (accept(Tok::Semi) || tok_.newlineBefore || tok_.is(Tok::RBrace) || tok_.is(Tok::Eof))) {
    // No body, as in an overload signature (see parseFunctionDecl).
  } else {
    error(tok_.loc(), "expected '{' to start the function body");
    fn->body = ctx_.make<BlockStmt>(tok_.loc());
  }
  inGenerator_ = outerGenerator;
  fn->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
  return fn;
}

void Parser::parseParams(FunctionDecl *fn, ParamPatterns *patterns) {
  expect(Tok::LParen, fn->isArrow ? "to start the parameters" : "after the function name");
  while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
    Param param;
    // `constructor(@Inject("name") name: string)` (docs/decorators.md).
    param.decorators = parseDecorators();
    if (recovering_)
      break;
    // `constructor(public x: int)`: a parameter property.
    while (tok_.is(Tok::Identifier) &&
           (tok_.text == "public" || tok_.text == "private" || tok_.text == "protected" ||
            tok_.text == "readonly" || tok_.text == "override") &&
           peek().is(Tok::Identifier)) {
      if (!fn->isConstructor)
        report(tok_.loc(), "'" + tok_.text + "' before a parameter makes it a field, which only "
                                             "a constructor's parameter can be");
      param.property = true;
      if (tok_.text == "readonly")
        param.readonly = true;
      else if (tok_.text == "private")
        param.access = Access::Private;
      else if (tok_.text == "protected")
        param.access = Access::Protected;
      advance();
    }
    param.rest = accept(Tok::Ellipsis);
    param.loc = tok_.loc();
    if (tok_.is(Tok::Identifier)) {
      param.name = tok_.text;
      advance();
    } else if ((tok_.is(Tok::LBrace) || tok_.is(Tok::LBracket)) && !param.rest) {
      // `({ x, y }: Point)`: the body destructures a hidden parameter.
      const char *start = tok_.text.data();
      std::unique_ptr<Pattern> pattern = parseBindingPattern();
      param.name = ctx_.hiddenName();
      param.pattern = textFrom(start);
      if (patterns)
        patterns->emplace_back(param.name, std::move(pattern));
    } else {
      error(tok_.loc(),
            param.rest ? "expected a parameter name after '...'" : "expected a parameter name");
      break;
    }
    if (accept(Tok::Question))
      param.optional = true;
    if (accept(Tok::Colon))
      param.type = parseType();
    if (tok_.is(Tok::Assign)) {
      if (param.optional)
        report(tok_.loc(), "a parameter with a default value is optional already; remove the '?'");
      advance();
      param.defaultValue = parseExpr();
    }
    if (param.rest && (param.optional || param.defaultValue))
      report(param.loc, "a rest parameter cannot be optional or have a default value");
    fn->params.push_back(param);
    if (!accept(Tok::Comma))
      break;
    if (param.rest && !tok_.is(Tok::RParen))
      report(param.loc, "a rest parameter must be the last parameter");
  }
  expect(Tok::RParen, "to close the parameter list");
}

// `function f({ x, y }: Point) { body }` is
// `function f(%0: Point) { let x = %0.x, y = %0.y; body }`: as in
// JavaScript, the variables can be assigned to.
void Parser::addPrologue(FunctionDecl *fn, ParamPatterns &patterns) {
  if (patterns.empty() || !fn->body)
    return;
  auto *decl = ctx_.make<VarDeclStmt>(fn->body->loc(), /*isConst=*/false);
  for (auto &[name, pattern] : patterns)
    declareElements(decl, *pattern, name);
  fn->body->body.insert(fn->body->body.begin(), decl);
}

llvm::StringRef Parser::textFrom(const char *start) const {
  return llvm::StringRef(start, static_cast<size_t>(std::max(prevEnd_, start) - start));
}

bool Parser::asyncFunctionAhead() {
  return tok_.is(Tok::Identifier) && tok_.text == "async" && peek().is(Tok::KwFunction) &&
         !peek().newlineBefore;
}

// At `async`: an async arrow `async x => ...` or `async (x) => ...`, or an
// async function expression, follows; otherwise `async` is a name, and
// nothing is consumed.
Expr *Parser::tryAsyncFunction() {
  llvm::SMLoc loc = tok_.loc();
  if (asyncFunctionAhead()) {
    advance();
    return ctx_.make<FunctionExpr>(loc, parseFunction(/*expression=*/true, /*isAsync=*/true));
  }
  const Token &next = peek();
  if (next.newlineBefore || !(next.is(Tok::Identifier) || next.is(Tok::LParen)))
    return nullptr;
  Snapshot start = snapshot();
  advance(); // 'async'
  bool arrow = tok_.is(Tok::Identifier) ? peek().is(Tok::Arrow) && !peek().newlineBefore
                                        : arrowAhead();
  if (!arrow) {
    rewind(std::move(start));
    return nullptr;
  }
  FunctionExpr *fe = parseArrow();
  fe->fn->isAsync = true;
  return fe;
}

// At `(`: the parameters of an arrow function follow if they parse as such
// and `=>` (or a return type and `=>`) comes after them.
bool Parser::arrowAhead() {
  Snapshot start = snapshot();
  bool muted = diags_.setMuted(true);
  recovering_ = false;
  FunctionDecl probe(tok_.loc(), "");
  probe.isArrow = true;
  parseParams(&probe);
  bool arrow = !recovering_;
  if (arrow && accept(Tok::Colon)) {
    parseType();
    arrow = !recovering_;
  }
  arrow = arrow && tok_.is(Tok::Arrow);
  diags_.setMuted(muted);
  rewind(std::move(start));
  return arrow;
}

FunctionExpr *Parser::parseArrow(std::vector<TypeParamDecl> typeParams, const char *start) {
  if (!start)
    start = tok_.text.data();
  llvm::SMLoc loc = tok_.loc();
  auto *fn = ctx_.make<FunctionDecl>(loc, "");
  fn->isExpression = fn->isArrow = true;
  fn->typeParams = std::move(typeParams);
  // An arrow cannot yield for the generator around it.
  llvm::SaveAndRestore<bool> inGenerator(inGenerator_, false);
  ParamPatterns patterns;
  if (tok_.is(Tok::Identifier)) {
    Param param;
    param.loc = tok_.loc();
    param.name = tok_.text;
    advance();
    fn->params.push_back(param);
  } else {
    parseParams(fn, &patterns);
    if (accept(Tok::Colon))
      fn->returnType = parseReturnType();
  }
  if (tok_.is(Tok::Arrow) && tok_.newlineBefore)
    report(tok_.loc(), "'=>' must be on the same line as the parameters");
  expect(Tok::Arrow, "after the parameters of the arrow function");
  if (tok_.is(Tok::LBrace)) {
    fn->body = parseBlock();
    // `() => { a: 1 }` is a block with the label `a`, not an object.
    if (fn->body->body.size() == 1)
      if (auto *labeled = dyn_cast<LabeledStmt>(fn->body->body[0]); labeled && isa<ExprStmt>(labeled->body))
        report(labeled->loc(), "'{' after '=>' starts the body of the function, so '" + labeled->label +
                                   ":' is a label, and the function returns nothing");
  } else {
    // `x => value` returns the value.
    llvm::SMLoc valueLoc = tok_.loc();
    Expr *value = parseExpr();
    fn->body = ctx_.make<BlockStmt>(valueLoc);
    fn->body->body.push_back(ctx_.make<ReturnStmt>(valueLoc, value));
    fn->concise = true;
  }
  addPrologue(fn, patterns);
  fn->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
  return ctx_.make<FunctionExpr>(loc, fn);
}

FunctionDecl *Parser::parseFunctionExprCopy() {
  if (tok_.is(Tok::KwFunction))
    return parseFunction(/*expression=*/true);
  if (tok_.is(Tok::Less)) {
    const char *start = tok_.text.data();
    std::vector<TypeParamDecl> typeParams = parseTypeParams();
    return parseArrow(std::move(typeParams), start)->fn;
  }
  return parseArrow()->fn;
}

// `<T, U extends number = int>`. TypeScript's modifiers of type parameters
// (`const T`, `in T`, `out T`) say nothing a copy for each type needs.
std::vector<TypeParamDecl> Parser::parseTypeParams() {
  std::vector<TypeParamDecl> params;
  advance(); // '<'
  while (!tok_.is(Tok::Greater) && !tok_.is(Tok::Eof)) {
    if ((tok_.is(Tok::KwConst) || tok_.is(Tok::KwIn) || (tok_.is(Tok::Identifier) && tok_.text == "out")) &&
        peek().is(Tok::Identifier)) {
      report(tok_.loc(), "'" + tok_.text + "' before a type parameter is not supported; remove it");
      advance();
    }
    TypeParamDecl param;
    param.loc = tok_.loc();
    if (!tok_.is(Tok::Identifier)) {
      error(tok_.loc(), llvm::Twine("expected a type parameter name, found ") + tokSpelling(tok_.kind));
      return params;
    }
    param.name = tok_.text;
    advance();
    if (tok_.is(Tok::Identifier) && tok_.text == "extends") {
      advance();
      param.constraint = parseType();
    }
    if (accept(Tok::Assign))
      param.defaultType = parseType();
    params.push_back(std::move(param));
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::Greater, "to close the type parameters");
  return params;
}

// Parses `let`/`const` declarators without the trailing semicolon, so that it
// also serves `for (let i = 0; ...)`.
VarDeclStmt *Parser::parseVarDecl() {
  auto *decl = ctx_.make<VarDeclStmt>(tok_.loc(), tok_.is(Tok::KwConst));
  advance(); // 'let' / 'const' / 'var'
  parseDeclarators(decl);
  return decl;
}

void Parser::parseDeclarators(VarDeclStmt *decl) {
  do {
    if (tok_.is(Tok::LBracket) || tok_.is(Tok::LBrace)) {
      // `const [a, b]: int[] = value`, `const { x, y }: Point = value`
      std::unique_ptr<Pattern> pattern = parseBindingPattern();
      std::optional<TypeRef> typeAnn;
      if (accept(Tok::Colon))
        typeAnn = parseType();
      if (!tok_.is(Tok::Assign)) {
        error(pattern->loc, pattern->object
                                ? "a destructuring declaration needs a value: '{ a, b } = ...'"
                                : "a destructuring declaration needs a value: '[a, b] = ...'");
        return;
      }
      advance();
      declarePattern(decl, *pattern, parseExpr(), std::move(typeAnn));
      continue;
    }
    VarDeclarator d;
    if (!parseBinding(d))
      return;
    if (accept(Tok::Assign))
      d.init = parseExpr();
    // `const Point = class { ... }` names the class, as in JavaScript.
    if (auto *ce = llvm::dyn_cast_or_null<ClassExpr>(d.init); ce && ce->decl->name.empty())
      ce->decl->name = d.name;
    if (decl->isConst && d.init)
      recordConstant(d.name, d.init);
    decl->decls.push_back(d);
  } while (accept(Tok::Comma));
}

static std::string toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);
  return utf8;
}

void Parser::recordConstant(llvm::StringRef name, const Expr *init) {
  std::vector<std::string> values;
  if (auto *lit = dyn_cast<StringLitExpr>(init))
    values.push_back(toUtf8(lit->value));
  else if (auto *tmpl = dyn_cast<TemplateExpr>(init); tmpl && tmpl->exprs.empty())
    values.push_back(toUtf8(tmpl->quasis[0]));
  else if (auto *other = dyn_cast<NameExpr>(init))
    values = stringConstants_.lookup(other->name); // declared earlier
  std::vector<std::string> &all = stringConstants_[name];
  all.insert(all.end(), values.begin(), values.end());
}

// A variable name with an optional type annotation.
bool Parser::parseBinding(VarDeclarator &d) {
  d.loc = tok_.loc();
  if (!tok_.is(Tok::Identifier)) {
    error(tok_.loc(), "expected a variable name");
    return false;
  }
  d.name = tok_.text;
  advance();
  if (accept(Tok::Colon))
    d.typeAnn = parseType();
  return true;
}

//===----------------------------------------------------------------------===//
// Destructuring
//===----------------------------------------------------------------------===//

bool Parser::Pattern::declares(llvm::StringRef name) const {
  for (const Element &e : elements)
    if (e.name == name || (e.nested && e.nested->declares(name)))
      return true;
  return false;
}

// `[a, , b = 1, [c, d], ...rest]` in a declaration.
std::unique_ptr<Parser::Pattern> Parser::parseBindingPattern() {
  if (tok_.is(Tok::LBrace))
    return parseObjectPattern();
  auto pattern = std::make_unique<Pattern>();
  pattern->loc = tok_.loc();
  advance(); // '['
  while (!tok_.is(Tok::RBracket) && !tok_.is(Tok::Eof)) {
    Pattern::Element e;
    e.loc = tok_.loc();
    if (accept(Tok::Comma)) {
      e.hole = true;
      pattern->elements.push_back(std::move(e));
      continue;
    }
    e.rest = accept(Tok::Ellipsis);
    if (tok_.is(Tok::LBracket) || tok_.is(Tok::LBrace)) {
      e.nested = parseBindingPattern();
    } else if (tok_.is(Tok::Identifier)) {
      e.name = tok_.text;
      advance();
    } else {
      error(tok_.loc(), llvm::Twine("expected a variable name in the destructuring pattern, found ") +
                            tokSpelling(tok_.kind));
      return pattern;
    }
    if (tok_.is(Tok::Assign)) {
      if (e.rest)
        report(tok_.loc(), "a rest element cannot have a default value");
      advance();
      e.defaultValue = parseExpr();
    }
    bool rest = e.rest;
    pattern->elements.push_back(std::move(e));
    if (!accept(Tok::Comma))
      break;
    if (rest)
      report(pattern->elements.back().loc, "a rest element must be the last one");
  }
  expect(Tok::RBracket, "to close the destructuring pattern");
  return pattern;
}

// `{ a, b: c, d = 1, e: { f }, "g-h": g, [key]: k, ...rest }` in a
// declaration.
std::unique_ptr<Parser::Pattern> Parser::parseObjectPattern() {
  auto pattern = std::make_unique<Pattern>();
  pattern->loc = tok_.loc();
  pattern->object = true;
  advance(); // '{'
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    Pattern::Element e;
    e.loc = tok_.loc();
    if (accept(Tok::Ellipsis)) {
      e.rest = true;
      if (!tok_.is(Tok::Identifier)) {
        error(tok_.loc(), "expected a variable name after '...' in the destructuring pattern");
        return pattern;
      }
      e.name = tok_.text;
      advance();
    } else {
      bool identifier = tok_.is(Tok::Identifier);
      if (accept(Tok::LBracket)) {
        e.keyExpr = parseExpr();
        expect(Tok::RBracket, "to close the key");
      } else if (!parseFieldName(e.key)) {
        skipToClosingBrace();
        return pattern;
      }
      if (accept(Tok::Colon)) {
        if (tok_.is(Tok::LBracket) || tok_.is(Tok::LBrace)) {
          e.nested = parseBindingPattern();
        } else if (tok_.is(Tok::Identifier)) {
          e.name = tok_.text;
          advance();
        } else {
          error(tok_.loc(),
                llvm::Twine("expected a variable name in the destructuring pattern, found ") +
                    tokSpelling(tok_.kind));
          return pattern;
        }
      } else if (identifier && !e.keyExpr) {
        e.name = e.key; // `{ x }` is `{ x: x }`
      } else {
        error(tok_.loc(), llvm::Twine("a key in a destructuring pattern needs a variable: '") +
                              (e.keyExpr ? llvm::Twine("[key]") : llvm::Twine(e.key)) + ": name'");
        return pattern;
      }
      if (accept(Tok::Assign))
        e.defaultValue = parseExpr();
    }
    bool rest = e.rest;
    pattern->elements.push_back(std::move(e));
    if (!accept(Tok::Comma))
      break;
    if (rest)
      report(pattern->elements.back().loc, "a rest element must be the last one");
  }
  expect(Tok::RBrace, "to close the destructuring pattern");
  return pattern;
}

std::unique_ptr<Parser::Pattern> Parser::patternFromLiteral(Expr *expr) {
  auto pattern = std::make_unique<Pattern>();
  pattern->loc = expr->loc();
  if (auto *object = dyn_cast<ObjectLitExpr>(expr)) {
    // `({ a, b: o.c, d = 1, ...rest } = value)`
    pattern->object = true;
    for (size_t i = 0; i < object->props.size(); ++i) {
      const ObjectLitExpr::Property &prop = object->props[i];
      Pattern::Element e;
      e.loc = prop.loc;
      e.rest = prop.spread;
      e.key = prop.name;
      e.keyExpr = prop.key;
      if (e.rest && i + 1 != object->props.size())
        report(e.loc, "a rest element must be the last one");
      Expr *target = prop.value;
      if (auto *assign = dyn_cast<AssignExpr>(target); assign && !assign->op && !e.rest) {
        target = assign->target; // `{ a = 1 }` or `{ a: b = 1 }`
        e.defaultValue = assign->value;
      }
      if (isa<ArrayLitExpr>(target) || isa<ObjectLitExpr>(target))
        e.nested = patternFromLiteral(target);
      else
        e.target = target;
      pattern->elements.push_back(std::move(e));
    }
    return pattern;
  }
  auto *lit = cast<ArrayLitExpr>(expr);
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    const ArrayLitExpr::Element &element = lit->elements[i];
    Pattern::Element e;
    e.loc = element.expr->loc();
    e.hole = element.hole;
    e.rest = element.spread;
    if (e.rest && i + 1 != lit->elements.size())
      report(e.loc, "a rest element must be the last one");
    Expr *target = element.expr;
    if (auto *assign = dyn_cast<AssignExpr>(target); assign && !assign->op && !e.rest) {
      target = assign->target; // `[a = 1] = values`
      e.defaultValue = assign->value;
    }
    if (isa<ArrayLitExpr>(target) || isa<ObjectLitExpr>(target))
      e.nested = patternFromLiteral(target);
    else if (!e.hole)
      e.target = target;
    pattern->elements.push_back(std::move(e));
  }
  return pattern;
}

VarDeclarator Parser::hiddenDeclarator(llvm::SMLoc loc, Expr *init) {
  VarDeclarator d;
  d.name = ctx_.hiddenName();
  d.loc = loc;
  d.init = init;
  return d;
}

// `const [a, b = 1, ...rest] = value` is
// `const %0 = value, a = %0[0], b = %0[1] ?? 1, rest = %0.slice(2)`.
void Parser::declarePattern(VarDeclStmt *decl, const Pattern &pattern, Expr *value,
                            std::optional<TypeRef> typeAnn) {
  VarDeclarator hidden = hiddenDeclarator(pattern.loc, value);
  hidden.typeAnn = std::move(typeAnn);
  hidden.destructured = true;
  hidden.objectPattern = pattern.object;
  hidden.patternLength = pattern.elements.size();
  hidden.patternRest = !pattern.elements.empty() && pattern.elements.back().rest;
  decl->decls.push_back(hidden);
  declareElements(decl, pattern, hidden.name);
}

void Parser::declareElements(VarDeclStmt *decl, const Pattern &pattern, llvm::StringRef base) {
  std::vector<const IndexExpr *> keyReads;
  for (size_t i = 0; i < pattern.elements.size(); ++i) {
    const Pattern::Element &e = pattern.elements[i];
    if (e.hole)
      continue;
    Expr *value = elementOf(base, pattern, i, keyReads);
    if (e.nested) {
      declarePattern(decl, *e.nested, value, std::nullopt);
      continue;
    }
    VarDeclarator d;
    d.name = e.name;
    d.loc = e.loc;
    d.init = value;
    decl->decls.push_back(d);
  }
}

// `[a, b] = value` is `{ const %0 = value; a = %0[0]; b = %0[1] }`. When value
// is an array literal, as in `[a, b] = [b, a]`, its elements go to hidden
// variables instead, and no array is made: `{ const %0 = b, %1 = a; a = %0;
// b = %1 }`. The same for an object pattern, `({ a, b } = o)`, with fields.
Stmt *Parser::assignPattern(llvm::SMLoc loc, const Pattern &pattern, Expr *value) {
  auto *block = ctx_.make<BlockStmt>(loc);
  auto *decl = ctx_.make<VarDeclStmt>(loc, /*isConst=*/true);
  block->body.push_back(decl);
  auto *lit = dyn_cast<ArrayLitExpr>(value);
  bool direct = !pattern.object && lit && lit->elements.size() >= pattern.elements.size() &&
                llvm::none_of(lit->elements,
                              [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; }) &&
                llvm::none_of(pattern.elements, [](const Pattern::Element &e) { return e.rest; });
  std::vector<llvm::StringRef> values;
  if (direct) {
    for (const ArrayLitExpr::Element &e : lit->elements) {
      decl->decls.push_back(hiddenDeclarator(e.expr->loc(), e.expr));
      values.push_back(decl->decls.back().name);
    }
  } else {
    VarDeclarator hidden = hiddenDeclarator(pattern.loc, value);
    hidden.destructured = true;
    hidden.objectPattern = pattern.object;
    hidden.patternLength = pattern.elements.size();
    hidden.patternRest = !pattern.elements.empty() && pattern.elements.back().rest;
    decl->decls.push_back(hidden);
  }
  llvm::StringRef base = decl->decls.front().name;
  std::vector<const IndexExpr *> keyReads;
  for (size_t i = 0; i < pattern.elements.size(); ++i) {
    const Pattern::Element &e = pattern.elements[i];
    if (e.hole)
      continue;
    Expr *element;
    if (direct) {
      element = ctx_.make<NameExpr>(e.loc, values[i]);
      if (e.defaultValue)
        element = ctx_.make<BinaryExpr>(e.loc, BinaryOp::Nullish, element, e.defaultValue);
    } else {
      element = elementOf(base, pattern, i, keyReads);
    }
    if (e.nested)
      block->body.push_back(assignPattern(e.loc, *e.nested, element));
    else
      block->body.push_back(ctx_.make<ExprStmt>(e.loc, ctx_.make<AssignExpr>(e.loc, std::nullopt, e.target, element)));
  }
  return block;
}

Expr *Parser::elementOf(llvm::StringRef base, const Pattern &pattern, size_t i,
                        std::vector<const IndexExpr *> &keyReads) {
  const Pattern::Element &e = pattern.elements[i];
  auto *source = ctx_.make<NameExpr>(e.loc, base);
  Expr *element;
  if (!pattern.object) {
    auto *position = ctx_.make<IntLitExpr>(e.loc, i);
    if (e.rest) {
      auto *slice = ctx_.make<MemberExpr>(e.loc, source, "slice");
      return ctx_.make<CallExpr>(e.loc, slice, std::vector<Expr *>{position});
    }
    element = ctx_.make<IndexExpr>(e.loc, source, position);
  } else if (e.rest) {
    // `{ a, ...rest }`: a new object with the other fields, `{ ...%0 }`
    // without a.
    auto *copy = ctx_.make<ObjectLitExpr>(e.loc);
    ObjectLitExpr::Property spread;
    spread.loc = e.loc;
    spread.value = source;
    spread.spread = true;
    for (const Pattern::Element &other : pattern.elements)
      if (!other.rest && !other.keyExpr)
        spread.omit.push_back(other.key);
    spread.omitKeys = keyReads;
    copy->props.push_back(std::move(spread));
    return copy;
  } else if (e.keyExpr) {
    auto *read = ctx_.make<IndexExpr>(e.loc, source, e.keyExpr);
    keyReads.push_back(read);
    element = read;
  } else {
    element = ctx_.make<MemberExpr>(e.loc, source, e.key);
  }
  if (e.defaultValue)
    element = ctx_.make<BinaryExpr>(e.loc, BinaryOp::Nullish, element, e.defaultValue);
  return element;
}

Stmt *Parser::parseIf() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'if'
  expect(Tok::LParen, "after 'if'");
  Expr *cond = parseSequence();
  expect(Tok::RParen, "after the condition");
  Stmt *thenStmt = parseStatement();
  Stmt *elseStmt = nullptr;
  if (accept(Tok::KwElse))
    elseStmt = parseStatement();
  return ctx_.make<IfStmt>(loc, cond, thenStmt, elseStmt);
}

Stmt *Parser::parseWhile() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'while'
  expect(Tok::LParen, "after 'while'");
  Expr *cond = parseSequence();
  expect(Tok::RParen, "after the condition");
  Stmt *body = parseStatement();
  return ctx_.make<WhileStmt>(loc, cond, body);
}

Stmt *Parser::parseDoWhile() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'do'
  Stmt *body = parseStatement();
  expect(Tok::KwWhile, "after the body of 'do'");
  expect(Tok::LParen, "after 'while'");
  Expr *cond = parseSequence();
  expect(Tok::RParen, "after the condition");
  // A semicolon after `do ... while (...)` is always optional.
  accept(Tok::Semi);
  return ctx_.make<DoWhileStmt>(loc, body, cond);
}

Stmt *Parser::parseFor() {
  llvm::SMLoc forLoc = tok_.loc();
  auto *loop = ctx_.make<ForStmt>(forLoc);
  advance(); // 'for'
  llvm::SMLoc awaitLoc;
  if (tok_.is(Tok::Identifier) && tok_.text == "await") {
    awaitLoc = tok_.loc();
    advance();
  }
  expect(Tok::LParen, "after 'for'");

  // In the initializer, as in JavaScript, `in` is not an operator: it
  // belongs to `for (x in y)`.
  llvm::SaveAndRestore<bool> noIn(noIn_, true);
  if (tok_.is(Tok::KwLet) || tok_.is(Tok::KwConst) || tok_.is(Tok::KwVar)) {
    if (tok_.is(Tok::KwVar))
      error(tok_.loc(), "'var' is not supported; use 'let' or 'const'");
    auto *decl = ctx_.make<VarDeclStmt>(tok_.loc(), tok_.is(Tok::KwConst));
    advance();
    VarDeclarator first;
    std::unique_ptr<Pattern> pattern;
    if (tok_.is(Tok::LBracket) || tok_.is(Tok::LBrace))
      pattern = parseBindingPattern();
    else if (!parseBinding(first))
      return loop;
    bool forIn = tok_.is(Tok::KwIn);
    if ((tok_.is(Tok::Identifier) && tok_.text == "of") || forIn) {
      // `for (const k in o)` walks the keys of an object.
      if (forIn && pattern)
        report(pattern->loc,
               "the variable of 'for...in' is a key, a string, and cannot be destructured");
      advance();
      noIn_ = false;
      auto *forOf = ctx_.make<ForOfStmt>(forLoc);
      forOf->isConst = decl->isConst;
      forOf->keys = forIn;
      forOf->isAwait = awaitLoc.isValid();
      if (forIn && forOf->isAwait)
        report(awaitLoc, "'for await' goes over the values of an iterable, with 'of': 'for await "
                         "(const x of xs)'");
      forOf->iterable = parseExpr();
      expect(Tok::RParen, "to close the loop header");
      forOf->body = parseStatement();
      if (!pattern || forIn) {
        forOf->binding = first;
        if (pattern) {
          forOf->binding.name = ctx_.hiddenName();
          forOf->binding.loc = pattern->loc;
        }
        return forOf;
      }
      // `for (const [a, b] of pairs) body` is
      // `for (const %0 of pairs) { const a = %0[0], b = %0[1]; body }`.
      forOf->binding.name = ctx_.hiddenName();
      forOf->binding.loc = pattern->loc;
      forOf->binding.destructured = true;
      forOf->binding.objectPattern = pattern->object;
      auto *elements = ctx_.make<VarDeclStmt>(pattern->loc, decl->isConst);
      declareElements(elements, *pattern, forOf->binding.name);
      auto *body = ctx_.make<BlockStmt>(forOf->body->loc());
      body->body = {elements, forOf->body};
      forOf->body = body;
      return forOf;
    }
    if (pattern) {
      if (!expect(Tok::Assign, "after the destructuring pattern"))
        return loop;
      declarePattern(decl, *pattern, parseExpr(), std::nullopt);
    } else {
      if (accept(Tok::Assign))
        first.init = parseExpr();
      decl->decls.push_back(first);
    }
    if (accept(Tok::Comma))
      parseDeclarators(decl);
    loop->init = decl;
  } else if (!tok_.is(Tok::Semi)) {
    llvm::SMLoc loc = tok_.loc();
    loop->init = ctx_.make<ExprStmt>(loc, parseSequence());
  }
  if (tok_.is(Tok::KwIn) || (tok_.is(Tok::Identifier) && tok_.text == "of")) {
    error(tok_.loc(), tok_.is(Tok::KwIn)
                          ? "'for...in' needs a new variable: 'for (const k in ...)'"
                          : "'for...of' needs a new variable: 'for (const x of ...)'");
    return loop;
  }
  noIn_ = false;
  if (awaitLoc.isValid())
    report(awaitLoc, "'for await' goes over the values of an iterable: 'for await (const x of "
                     "xs)'");
  expect(Tok::Semi, "after the loop initializer");

  if (!tok_.is(Tok::Semi))
    loop->cond = parseSequence();
  expect(Tok::Semi, "after the loop condition");

  if (!tok_.is(Tok::RParen))
    loop->step = parseSequence();
  expect(Tok::RParen, "to close the loop header");

  loop->body = parseStatement();
  return loop;
}

Stmt *Parser::parseReturn() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'return'
  Expr *value = nullptr;
  // `return` followed by a line break returns nothing, as in JavaScript.
  if (!tok_.is(Tok::Semi) && !tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof) && !tok_.newlineBefore)
    value = parseSequence();
  expectSemicolon();
  return ctx_.make<ReturnStmt>(loc, value);
}

// `throw value`. As in JavaScript, the value must start on the same line.
Stmt *Parser::parseThrow() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'throw'
  if (tok_.newlineBefore || tok_.is(Tok::Semi) || tok_.is(Tok::RBrace) || tok_.is(Tok::Eof)) {
    error(loc, "'throw' needs a value on the same line, e.g. 'throw new Error(\"...\")'");
    return ctx_.make<EmptyStmt>(loc);
  }
  Expr *value = parseSequence();
  expectSemicolon();
  return ctx_.make<ThrowStmt>(loc, value);
}

// `try { ... } catch (e) { ... } finally { ... }`; `catch { ... }` without a
// variable, and `catch (e: unknown)` as strict TypeScript writes it.
Stmt *Parser::parseTry() {
  auto *stmt = ctx_.make<TryStmt>(tok_.loc());
  advance(); // 'try'
  stmt->block = parseBlock();
  if (tok_.is(Tok::Identifier) && tok_.text == "catch") {
    advance();
    if (accept(Tok::LParen)) {
      if (tok_.is(Tok::LBrace) || tok_.is(Tok::LBracket)) {
        report(tok_.loc(), "destructuring the caught error is not supported; write 'catch (e)' and "
                           "read its fields, e.g. 'e.message'");
        while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof))
          advance();
      } else {
        parseBinding(stmt->param);
      }
      expect(Tok::RParen, "after the variable of 'catch'");
    }
    stmt->handler = parseBlock();
  }
  if (tok_.is(Tok::Identifier) && tok_.text == "finally") {
    advance();
    stmt->finalizer = parseBlock();
  }
  if (!stmt->handler && !stmt->finalizer)
    report(stmt->loc(), "'try' needs 'catch' or 'finally' after its block");
  return stmt;
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

// type := unionType ('extends' unionType '?' type ':' type)?
TypeRef Parser::parseType() {
  const char *start = tok_.text.data();
  bool noConditional = std::exchange(noConditional_, false);
  bool outer = std::exchange(disallowConditional_, noConditional);
  TypeRef check = parseUnionType();
  disallowConditional_ = outer;
  if (noConditional || !tok_.is(Tok::Identifier) || tok_.text != "extends")
    return check;
  // `T extends string ? A : B`: a conditional type (docs/generics.md,
  // section 10).
  TypeRef ref;
  ref.kind = TypeRef::Kind::Conditional;
  ref.loc = check.loc;
  advance(); // 'extends'
  ref.args.push_back(std::move(check));
  noConditional_ = true;
  ref.args.push_back(parseType());
  if (!expect(Tok::Question,
              "after the 'extends' clause of the conditional type, e.g. 'T extends string "
              "? A : B'"))
    return TypeRef();
  ref.args.push_back(parseType());
  if (!expect(Tok::Colon, "between the branches of the conditional type"))
    return TypeRef();
  ref.args.push_back(parseType());
  ref.source = textFrom(start);
  return ref;
}

// unionType := '|'? intersectionType ('|' intersectionType)*
TypeRef Parser::parseUnionType() {
  llvm::SMLoc loc = tok_.loc();
  accept(Tok::Pipe); // a leading `|` is allowed, as in TypeScript
  TypeRef first = parseIntersectionType();
  if (!tok_.is(Tok::Pipe))
    return first;
  TypeRef ref;
  ref.kind = TypeRef::Kind::Union;
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (accept(Tok::Pipe))
    ref.args.push_back(parseIntersectionType());
  return ref;
}

// intersectionType := '&'? arrayType ('&' arrayType)*
TypeRef Parser::parseIntersectionType() {
  llvm::SMLoc loc = tok_.loc();
  accept(Tok::Amp);
  TypeRef first = parseArrayType();
  if (!tok_.is(Tok::Amp))
    return first;
  TypeRef ref;
  ref.kind = TypeRef::Kind::Intersection;
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (accept(Tok::Amp))
    ref.args.push_back(parseArrayType());
  return ref;
}

// returnType := ('asserts'? (name | 'this') 'is' type) | 'asserts' (name | 'this') | type
TypeRef Parser::parseReturnType() {
  auto onLine = [&](const char *text) {
    return peek().is(Tok::Identifier) && !peek().newlineBefore && (!text || peek().text == text);
  };
  bool asserts = tok_.is(Tok::Identifier) && tok_.text == "asserts" && onLine(nullptr);
  if (!asserts && !(tok_.is(Tok::Identifier) && onLine("is")))
    return parseType();
  TypeRef ref;
  ref.kind = TypeRef::Kind::Predicate;
  ref.loc = tok_.loc();
  ref.asserts = asserts;
  if (asserts)
    advance();
  ref.name = tok_.text;
  advance();
  if (tok_.is(Tok::Identifier) && tok_.text == "is" && !tok_.newlineBefore) {
    advance();
    ref.args.push_back(parseType());
  }
  return ref;
}

// arrayType := 'readonly'? 'keyof'? primaryType ('[' type? ']')*
TypeRef Parser::parseArrayType() {
  // `keyof T`: the names of the fields of T.
  if (tok_.is(Tok::Identifier) && tok_.text == "keyof" &&
      (peek().is(Tok::Identifier) || peek().is(Tok::LParen) || peek().is(Tok::LBrace) ||
       peek().is(Tok::KwTypeof))) {
    TypeRef ref;
    ref.kind = TypeRef::Kind::KeyOf;
    ref.loc = tok_.loc();
    advance();
    ref.args.push_back(parseArrayType());
    return ref;
  }
  // `readonly T[]` is for tsc: the compiler does not stop changes to the array.
  if (tok_.is(Tok::Identifier) && tok_.text == "readonly" &&
      (peek().is(Tok::Identifier) || peek().is(Tok::LParen) || peek().is(Tok::LBracket))) {
    llvm::SMLoc loc = tok_.loc();
    advance();
    TypeRef ref = parseArrayType();
    if (ref.kind == TypeRef::Kind::Tuple)
      ref.readonly.assign(ref.args.size(), true); // its elements cannot be assigned to
    else if (ref.kind != TypeRef::Kind::Array)
      report(loc, "'readonly' applies only to array and tuple types");
    return ref;
  }
  TypeRef ref = parsePrimaryType();
  while (tok_.is(Tok::LBracket) && !tok_.newlineBefore) {
    TypeRef array;
    array.kind = TypeRef::Kind::Array;
    array.loc = ref.loc;
    advance();
    // `T[K]`: the type of the member K of T.
    if (!tok_.is(Tok::RBracket)) {
      array.kind = TypeRef::Kind::Indexed;
      array.args.push_back(std::move(ref));
      array.args.push_back(parseType());
      expect(Tok::RBracket, "to close the indexed access type");
      ref = std::move(array);
      continue;
    }
    expect(Tok::RBracket, "in the array type");
    array.args.push_back(std::move(ref));
    ref = std::move(array);
  }
  return ref;
}

// primaryType := name ('<' type (',' type)* '>')? | '(' type ')' | objectType
TypeRef Parser::parsePrimaryType() {
  TypeRef ref;
  ref.loc = tok_.loc();
  if (tok_.is(Tok::LBrace))
    return parseObjectType();
  if (tok_.is(Tok::LBracket))
    return parseTupleType();
  if (tok_.is(Tok::Less)) {
    // `<X>(x: X) => X`, a function that is generic itself. Which copy of a
    // generic function runs is decided when compiling, so a value must have
    // its types fixed; the function that takes one can be generic instead.
    std::vector<TypeParamDecl> params = parseTypeParams();
    const char *start = tok_.text.data();
    if (tok_.is(Tok::LParen))
      parseFunctionType();
    std::string names;
    for (const TypeParamDecl &p : params)
      names += (names.empty() ? "" : ", ") + p.name.str();
    llvm::StringRef type(start, static_cast<size_t>(std::max(prevEnd_, start) - start));
    report(ref.loc, "a function type cannot be generic itself: which copy of a generic function runs is "
                    "decided when compiling, so a function value has fixed types; make the function that "
                    "takes it generic instead, e.g. 'function apply<" + names + ">(f: " + type + ")'");
    return TypeRef(); // an empty name: an error already reported
  }
  if (tok_.is(Tok::LParen) && functionTypeAhead())
    return parseFunctionType();
  // `infer U` in the pattern of a conditional type, `infer N extends int`
  // with a constraint. As in TypeScript, `extends` after it starts a
  // conditional type if `?` follows, unless conditional types cannot be here.
  if (tok_.is(Tok::Identifier) && tok_.text == "infer" && peek().is(Tok::Identifier)) {
    ref.kind = TypeRef::Kind::Infer;
    advance(); // 'infer'
    ref.name = tok_.text;
    advance();
    if (tok_.is(Tok::Identifier) && tok_.text == "extends") {
      Snapshot start = snapshot();
      advance();
      noConditional_ = true;
      TypeRef constraint = parseType();
      if (disallowConditional_ || !tok_.is(Tok::Question))
        ref.args.push_back(std::move(constraint));
      else
        rewind(std::move(start));
    }
    return ref;
  }
  // Template literal types: `` `on${K}` ``, and `` `text` ``, a string
  // literal type.
  if (tok_.is(Tok::TemplateString) || tok_.is(Tok::TemplateHead)) {
    std::u16string text;
    decodeTemplatePart(tok_, text, diags_);
    if (tok_.is(Tok::TemplateString)) {
      ref.kind = TypeRef::Kind::Literal;
      ref.name = "string";
      ref.text = text;
      advance();
      return ref;
    }
    ref.kind = TypeRef::Kind::Template;
    ref.texts.push_back(text);
    advance();
    for (bool done = false; !done;) {
      ref.args.push_back(parseType());
      if (!tok_.is(Tok::TemplateMiddle) && !tok_.is(Tok::TemplateTail)) {
        error(tok_.loc(), "expected '}' to close the type in the template literal type");
        return TypeRef();
      }
      decodeTemplatePart(tok_, text, diags_);
      ref.texts.push_back(text);
      done = tok_.is(Tok::TemplateTail);
      advance();
    }
    return ref;
  }
  // Literal types: `"circle"`, `0`, `-1.5`, `true`.
  if (tok_.is(Tok::StringLiteral)) {
    ref.kind = TypeRef::Kind::Literal;
    ref.name = "string";
    decodeStringLiteral(tok_, ref.text, diags_);
    advance();
    return ref;
  }
  if (tok_.is(Tok::KwTrue) || tok_.is(Tok::KwFalse)) {
    ref.kind = TypeRef::Kind::Literal;
    ref.name = "boolean";
    ref.number = tok_.is(Tok::KwTrue) ? 1 : 0;
    advance();
    return ref;
  }
  bool negative =
      tok_.is(Tok::Minus) && (peek().is(Tok::IntLiteral) || peek().is(Tok::FloatLiteral));
  if (negative || tok_.is(Tok::IntLiteral) || tok_.is(Tok::FloatLiteral)) {
    if (negative)
      advance();
    Expr *lit = tok_.is(Tok::IntLiteral) ? parseIntLiteral() : parseFloatLiteral();
    ref.kind = TypeRef::Kind::Literal;
    ref.name = "number";
    if (auto *whole = dyn_cast<IntLitExpr>(lit)) {
      ref.number = double(whole->value);
      ref.integer = true;
    } else {
      ref.number = cast<FloatLitExpr>(lit)->value;
    }
    if (negative)
      ref.number = -ref.number;
    return ref;
  }
  if (accept(Tok::LParen)) {
    ref = parseType();
    expect(Tok::RParen, "to close the parenthesized type");
    return ref;
  }
  // `typeof defaults`, `typeof config.server`: the type of a value.
  if (tok_.is(Tok::KwTypeof)) {
    ref.kind = TypeRef::Kind::TypeOf;
    advance();
    if (!tok_.is(Tok::Identifier)) {
      error(tok_.loc(), llvm::Twine("expected the name of a value after 'typeof', found ") +
                            tokSpelling(tok_.kind));
      return ref;
    }
    ref.names.push_back(tok_.text);
    advance();
    while (tok_.is(Tok::Dot) && peek().is(Tok::Identifier)) {
      advance();
      ref.names.push_back(tok_.text);
      advance();
    }
    return ref;
  }
  if (!tok_.is(Tok::Identifier) && !tok_.is(Tok::KwVoid)) {
    error(tok_.loc(), llvm::Twine("expected a type, found ") + tokSpelling(tok_.kind));
    return ref;
  }
  ref.name = tok_.text;
  advance();
  // `NodeJS.Timeout`, as the types of Node name the objects of timers.
  if (ref.name == "NodeJS" && tok_.is(Tok::Dot) && peek().is(Tok::Identifier)) {
    advance();
    ref.name = ctx_.keepName(("NodeJS." + tok_.text).str());
    advance();
  }
  // `Intl.NumberFormat`: a type of the namespace Intl, by its dotted name.
  if (ref.name == "Intl" && tok_.is(Tok::Dot) && peek().is(Tok::Identifier)) {
    advance();
    ref.name = ctx_.keepName("Intl." + tok_.text.str());
    advance();
  }
  if (tok_.is(Tok::Less)) {
    ref.kind = TypeRef::Kind::Generic;
    ref.args = parseTypeArgs();
  }
  return ref;
}

bool Parser::mappedTypeAhead() {
  if (!tok_.is(Tok::LBracket) || !peek().is(Tok::Identifier))
    return false;
  Snapshot start = snapshot();
  advance(); // '['
  advance(); // the name
  bool in = tok_.is(Tok::KwIn);
  rewind(start);
  return in;
}

// `{ [K in Keys]: T }`, with `readonly`, `-readonly`, `?` and `-?`; the only
// member of its object type.
TypeRef Parser::parseMappedType(TypeRef &object, int readonlyChange) {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Mapped;
  ref.loc = object.loc;
  ref.mappedReadonly = readonlyChange;
  advance(); // '['
  ref.name = tok_.text;
  advance(); // the name of the key
  advance(); // 'in'
  ref.args.push_back(parseType());
  // `[K in keyof T as `get${Capitalize<K>}`]`: the name each key gives. The
  // `as` here is a part of the type, not the operator, which tz has not.
  std::optional<TypeRef> rename;
  if (accept(Tok::KwAs))
    rename = parseType();
  if (!expect(Tok::RBracket, "after the keys of the mapped type")) {
    skipToClosingBrace();
    return ref;
  }
  if ((tok_.is(Tok::Minus) || tok_.is(Tok::Plus)) && peek().is(Tok::Question)) {
    ref.mappedOptional = tok_.is(Tok::Minus) ? -1 : 1;
    advance();
    advance();
  } else if (accept(Tok::Question)) {
    ref.mappedOptional = 1;
  }
  if (!expect(Tok::Colon, "before the type of the values of the mapped type")) {
    skipToClosingBrace();
    return ref;
  }
  ref.args.push_back(parseType());
  if (rename)
    ref.args.push_back(std::move(*rename));
  if (!accept(Tok::Semi))
    accept(Tok::Comma);
  if (!tok_.is(Tok::RBrace) || !object.args.empty()) {
    error(tok_.loc(), "a mapped type cannot stand next to fields; make them an object type of their "
                      "own");
    skipToClosingBrace();
    return ref;
  }
  advance(); // '}'
  return ref;
}

bool Parser::symbolKeyAhead() {
  return tok_.is(Tok::LBracket) && peek().is(Tok::Identifier) && peek().text == "Symbol";
}

bool Parser::parseFieldName(llvm::StringRef &name) {
  // `[Symbol.iterator]`, the one name of a member in brackets
  // (docs/iterators.md, section 5).
  if (symbolKeyAhead()) {
    llvm::SMLoc loc = tok_.loc();
    advance(); // '['
    advance(); // 'Symbol'
    if (!expect(Tok::Dot, "after 'Symbol'"))
      return false;
    llvm::StringRef which = tok_.text;
    advance();
    if (!expect(Tok::RBracket, "to close the name"))
      return false;
    if (which == "asyncIterator") {
      // What `for await` iterates (docs/iterators.md, step 4).
      name = "[Symbol.asyncIterator]";
      return true;
    }
    if (which != "iterator")
      report(loc, "'Symbol." + which + "' cannot name a member: of the symbols of JavaScript, only "
                  "'Symbol.iterator' and 'Symbol.asyncIterator' can (docs/iterators.md, section "
                  "5)");
    name = "[Symbol.iterator]";
    return true;
  }
  if (tok_.is(Tok::Identifier) || isKeyword(tok_.kind)) {
    name = tok_.text;
    fieldNames_.insert(name);
    advance();
    return true;
  }
  if (tok_.is(Tok::StringLiteral)) {
    // `"content-type": ...` names the field by the text of the string.
    std::u16string text;
    decodeStringLiteral(tok_, text, diags_);
    name = ctx_.keepName(toUtf8(text));
    fieldNames_.insert(name);
    advance();
    return true;
  }
  if (tok_.is(Tok::LBracket))
    error(tok_.loc(), "a field name in brackets is not supported yet; write the name itself");
  else if (tok_.is(Tok::IntLiteral) || tok_.is(Tok::FloatLiteral))
    error(tok_.loc(), "a number cannot name a field; an object with numbered entries is an array");
  else
    error(tok_.loc(), llvm::Twine("expected a field name, found ") + tokSpelling(tok_.kind));
  return false;
}

// `{ x: int; readonly y?: string }`. Fields are separated by `;`, `,` or line
// breaks.
TypeRef Parser::parseObjectType() {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Object;
  ref.loc = tok_.loc();
  advance(); // '{'
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    llvm::SMLoc loc = tok_.loc();
    bool readonly = false;
    // `-readonly` and `+readonly` before a mapped type.
    int readonlyChange = 0;
    if ((tok_.is(Tok::Minus) || tok_.is(Tok::Plus)) && peek().is(Tok::Identifier) &&
        peek().text == "readonly") {
      readonlyChange = tok_.is(Tok::Minus) ? -1 : 1;
      advance();
    }
    if (tok_.is(Tok::Identifier) && tok_.text == "readonly" && !peek().is(Tok::Colon) &&
        !peek().is(Tok::Question) && !peek().is(Tok::LParen)) {
      readonly = true;
      readonlyChange = readonlyChange ? readonlyChange : 1;
      advance();
      loc = tok_.loc();
    }
    if (tok_.is(Tok::LBracket) && mappedTypeAhead())
      return parseMappedType(ref, readonlyChange);
    if (tok_.is(Tok::LBracket) && !symbolKeyAhead()) {
      // `{ [key: string]: T }` is a dictionary, `Record<string, T>`
      // (docs/objects.md, section 9): an object has a fixed set of fields.
      advance(); // '['
      if (!tok_.is(Tok::Identifier) || !peek().is(Tok::Colon)) {
        error(loc, "expected an index signature, '{ [key: string]: T }', or a mapped type, "
                   "'{ [K in Keys]: T }'");
        skipToClosingBrace();
        return ref;
      }
      advance(); // the name of the key
      advance(); // ':'
      TypeRef key = parseType();
      if (!expect(Tok::RBracket, "after the type of the keys") ||
          !expect(Tok::Colon, "before the type of the values")) {
        skipToClosingBrace();
        return ref;
      }
      TypeRef value = parseType();
      if (!accept(Tok::Semi))
        accept(Tok::Comma);
      if (!ref.args.empty() || !tok_.is(Tok::RBrace)) {
        error(loc, "an index signature cannot stand next to fields: an object has a fixed set of "
                   "fields, and a dictionary only keys; make the fields an object of their own, "
                   "or a dictionary 'Record<string, T>' alone");
        skipToClosingBrace();
        return ref;
      }
      advance(); // '}'
      TypeRef record;
      record.kind = TypeRef::Kind::Generic;
      record.name = "Record";
      record.loc = ref.loc;
      record.args.push_back(std::move(key));
      record.args.push_back(std::move(value));
      return record;
    }
    if (tok_.is(Tok::LParen) || tok_.is(Tok::Less) || tok_.is(Tok::KwNew)) {
      error(loc, "call signatures are not supported; a field can hold a function: 'f: (x: int) => int'");
      skipToClosingBrace();
      return ref;
    }
    llvm::StringRef name;
    if (!parseFieldName(name)) {
      skipToClosingBrace();
      return ref;
    }
    bool optional = accept(Tok::Question);
    if (tok_.is(Tok::Less)) {
      error(tok_.loc(), "generic methods in types are not supported yet");
      skipToClosingBrace();
      return ref;
    }
    if (tok_.is(Tok::LParen)) {
      // `area(): double`: a read-only field that holds a function.
      TypeRef fn;
      if (!parseSignatureParams(fn) || recovering_) {
        skipToClosingBrace();
        return ref;
      }
      if (!accept(Tok::Colon)) {
        error(tok_.loc(), "method '" + name + "' needs its return type, e.g. '" + name +
                              "(): void'");
        skipToClosingBrace();
        return ref;
      }
      fn.args.push_back(parseType());
      ref.args.push_back(std::move(fn));
      ref.names.push_back(name);
      ref.locs.push_back(loc);
      ref.optional.push_back(optional);
      ref.readonly.push_back(true);
      ref.methods.resize(ref.args.size() - 1);
      ref.methods.push_back(true);
      if (accept(Tok::Semi) || accept(Tok::Comma))
        continue;
      if (!tok_.is(Tok::RBrace) && !tok_.newlineBefore) {
        error(tok_.loc(),
              llvm::Twine("expected ';' after the method, found ") + tokSpelling(tok_.kind));
        skipToClosingBrace();
        return ref;
      }
      continue;
    }
    if (!tok_.is(Tok::Colon)) {
      error(tok_.loc(), "field '" + name + "' needs a type, e.g. '" + name + ": int'");
      skipToClosingBrace();
      return ref;
    }
    advance();
    ref.args.push_back(parseType());
    ref.names.push_back(name);
    ref.locs.push_back(loc);
    ref.optional.push_back(optional);
    ref.readonly.push_back(readonly);
    if (accept(Tok::Semi) || accept(Tok::Comma))
      continue;
    if (!tok_.is(Tok::RBrace) && !tok_.newlineBefore) {
      error(tok_.loc(), llvm::Twine("expected ';' after the field, found ") + tokSpelling(tok_.kind));
      skipToClosingBrace();
      return ref;
    }
  }
  expect(Tok::RBrace, "to close the object type");
  return ref;
}

// `[string, int]`, and `[]`, the arguments of an event without any
// (docs/events.md). Labels, as in `[name: string, age: int]`, are allowed
// and change nothing.
TypeRef Parser::parseTupleType() {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Tuple;
  ref.loc = tok_.loc();
  advance(); // '['
  while (!tok_.is(Tok::RBracket) && !tok_.is(Tok::Eof)) {
    // `...A`: the types a list of them stands for, which Sema checks.
    bool spread = accept(Tok::Ellipsis);
    if (tok_.is(Tok::Identifier) && peek().is(Tok::Colon)) {
      advance(); // the label
      advance(); // ':'
    }
    ref.args.push_back(parseType());
    ref.readonly.push_back(false);
    ref.spread.push_back(spread);
    if (tok_.is(Tok::Question)) {
      report(tok_.loc(), "optional elements in a tuple type are not supported: a tuple has a fixed "
                         "length; give the element the type 'T | undefined'");
      advance();
    }
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RBracket, "to close the tuple type");
  return ref;
}

// At `(`: a function type if a parameter list (names, optionally with `?`,
// `...` and types) and `=>` follow; otherwise a parenthesized type.
bool Parser::functionTypeAhead() {
  Snapshot start = snapshot();
  bool muted = diags_.setMuted(true);
  recovering_ = false;
  advance(); // '('
  bool ok = true;
  while (ok && !tok_.is(Tok::RParen)) {
    accept(Tok::Ellipsis);
    ok = accept(Tok::Identifier);
    accept(Tok::Question);
    if (ok && accept(Tok::Colon)) {
      parseType();
      ok = !recovering_;
    }
    if (ok && !accept(Tok::Comma))
      break;
  }
  ok = ok && accept(Tok::RParen) && tok_.is(Tok::Arrow);
  diags_.setMuted(muted);
  rewind(std::move(start));
  return ok;
}

TypeRef Parser::parseFunctionType() {
  TypeRef ref;
  if (!parseSignatureParams(ref))
    return ref;
  expect(Tok::Arrow, "after the parameters of the function type");
  ref.args.push_back(parseReturnType());
  return ref;
}

bool Parser::parseSignatureParams(TypeRef &ref) {
  ref.kind = TypeRef::Kind::Function;
  ref.loc = tok_.loc();
  advance(); // '('
  while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
    bool rest = accept(Tok::Ellipsis);
    llvm::SMLoc loc = tok_.loc();
    llvm::StringRef name = tok_.text;
    if (!expect(Tok::Identifier, "for a parameter of the function type"))
      return false;
    bool optional = accept(Tok::Question);
    TypeRef param;
    param.loc = loc;
    if (accept(Tok::Colon))
      param = parseType();
    else
      report(loc, "parameter '" + name + "' of the function type needs a type, e.g. '" + name + ": int'");
    if (ref.rest)
      report(loc, "a rest parameter must be the last parameter");
    ref.args.push_back(std::move(param));
    ref.optional.push_back(optional);
    ref.names.push_back(name);
    ref.rest |= rest;
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RParen, "to close the parameters of the function type");
  return true;
}

// `<T, U>`. A closing `>>` (as in `Array<Array<int>>`) is split in two.
std::vector<TypeRef> Parser::parseTypeArgs() {
  std::vector<TypeRef> args;
  advance(); // '<'
  do
    args.push_back(parseType());
  while (accept(Tok::Comma));

  switch (tok_.kind) {
  case Tok::Greater:
    advance();
    break;
  case Tok::Shr:
  case Tok::UShr:
  case Tok::GreaterEq:
  case Tok::ShrAssign:
  case Tok::UShrAssign:
    // Keep the rest of the token: `>>` leaves `>`, `>=` leaves `=`.
    tok_.kind = tok_.is(Tok::Shr)        ? Tok::Greater
                : tok_.is(Tok::UShr)     ? Tok::Shr
                : tok_.is(Tok::GreaterEq) ? Tok::Assign
                : tok_.is(Tok::ShrAssign) ? Tok::GreaterEq
                                          : Tok::ShrAssign;
    tok_.text = tok_.text.drop_front();
    tok_.newlineBefore = false;
    break;
  default:
    error(tok_.loc(), llvm::Twine("expected '>' to close the type arguments, found ") +
                          tokSpelling(tok_.kind));
    break;
  }
  return args;
}

bool Parser::tryTypeArgs(std::vector<TypeRef> &args, bool callOnly) {
  Snapshot start = snapshot();
  bool muted = diags_.setMuted(true);
  recovering_ = false;
  std::vector<TypeRef> parsed = parseTypeArgs();
  // As in TypeScript: a call, or a token that ends the expression (an
  // instantiation `first<int>` used as a value). After anything else, as in
  // `a < b > c`, the `<` compares.
  bool ok = !recovering_;
  switch (tok_.kind) {
  case Tok::LParen:
    break;
  case Tok::RParen:
  case Tok::RBracket:
  case Tok::RBrace:
  case Tok::Comma:
  case Tok::Semi:
  case Tok::Colon:
  case Tok::Eof:
    ok = ok && !callOnly;
    break;
  default:
    ok = ok && tok_.newlineBefore && !callOnly;
    break;
  }
  diags_.setMuted(muted);
  if (!ok) {
    rewind(std::move(start));
    return false;
  }
  recovering_ = start.recovering;
  args = std::move(parsed);
  return true;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

// `yield value`, a bare `yield`, or `yield* iterable`, at the level of an
// assignment. The value is on the same line, or there is none.
Expr *Parser::parseYield() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'yield'
  bool delegate = !tok_.newlineBefore && accept(Tok::Star);
  Expr *value = nullptr;
  if (delegate) {
    value = parseExpr();
  } else if (!tok_.newlineBefore) {
    // Only a token that can start an expression starts the value: `yield)`
    // and `yield;` have none.
    switch (tok_.kind) {
    case Tok::RParen:
    case Tok::RBracket:
    case Tok::RBrace:
    case Tok::Comma:
    case Tok::Semi:
    case Tok::Colon:
    case Tok::Question:
    case Tok::Eof:
    case Tok::TemplateMiddle:
    case Tok::TemplateTail:
      break;
    default:
      if (binaryPrec(tok_.kind) < 0 || tok_.is(Tok::Plus) || tok_.is(Tok::Minus) ||
          tok_.is(Tok::Less))
        value = parseExpr();
    }
  }
  if (!inGenerator_) {
    report(loc, "'yield' can only be used in a generator function, declared with 'function*'");
    return value ? value : ctx_.make<UndefinedExpr>(loc);
  }
  return ctx_.make<YieldExpr>(loc, value, delegate);
}

// `a, b, c`
Expr *Parser::parseSequence() {
  Expr *first = parseExpr();
  if (!tok_.is(Tok::Comma))
    return first;
  std::vector<Expr *> exprs{first};
  while (accept(Tok::Comma))
    exprs.push_back(parseExpr());
  return ctx_.make<SequenceExpr>(first->loc(), std::move(exprs));
}

// Assignment is right-associative and has the lowest precedence after the
// comma.
Expr *Parser::parseExpr() {
  const char *start = tok_.text.begin();
  if (tok_.is(Tok::Identifier) && tok_.text == "yield")
    return parseYield();
  Expr *lhs = parseConditional();
  std::optional<BinaryOp> op;
  if (!assignmentOp(tok_.kind, op))
    return lhs;
  llvm::SMLoc loc = tok_.loc();
  advance();
  Expr *rhs = parseExpr();
  auto *assign = ctx_.make<AssignExpr>(loc, op, lhs, rhs);
  assign->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
  return assign;
}

Expr *Parser::parseConditional() {
  Expr *cond = parseBinary(0);
  if (!tok_.is(Tok::Question))
    return cond;
  llvm::SMLoc loc = tok_.loc();
  advance();
  Expr *thenExpr = parseExpr();
  expect(Tok::Colon, "in the conditional expression");
  Expr *elseExpr = parseExpr();
  return ctx_.make<ConditionalExpr>(loc, cond, thenExpr, elseExpr);
}

Expr *Parser::parseBinary(int minPrec) {
  const char *start = tok_.text.begin();
  // `-x ** 2` is an error in JavaScript: it could mean either `(-x) ** 2` or
  // `-(x ** 2)`.
  llvm::StringRef unary;
  if (tok_.is(Tok::Minus) || tok_.is(Tok::Plus) || tok_.is(Tok::Bang) || tok_.is(Tok::Tilde) ||
      tok_.is(Tok::KwTypeof) || tok_.is(Tok::KwVoid) || tok_.is(Tok::KwDelete))
    unary = tok_.text;
  Expr *lhs = parseUnary();
  if (!unary.empty() && tok_.is(Tok::StarStar))
    report(tok_.loc(), "the left side of '**' cannot be a unary '" + unary +
                           "' expression; add parentheses, e.g. '(" + unary +
                           (llvm::isAlpha(unary[0]) ? " " : "") + "x) ** 2'");
  for (;;) {
    if (tok_.is(Tok::KwAs) && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      lhs = ctx_.make<CastExpr>(loc, lhs, parseType());
      continue;
    }
    // `x satisfies T` checks x against T and leaves its type as it is. tz
    // has no operators on types, as with `as`: the type goes on the variable.
    if (tok_.is(Tok::Identifier) && tok_.text == "satisfies" && !tok_.newlineBefore &&
        kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      const char *typeStart = tok_.text.data();
      parseType();
      llvm::StringRef type(typeStart,
                           static_cast<size_t>(std::max(prevEnd_, typeStart) - typeStart));
      report(loc, "'satisfies' is not supported, just as 'as' is not; declare the type of the "
                  "variable instead: 'const x: " + type + " = ...'");
      continue;
    }
    if (tok_.is(Tok::KwIn) && noIn_)
      return lhs;
    // `x instanceof C`, a relational operator that lexes as a name.
    if (tok_.is(Tok::Identifier) && tok_.text == "instanceof" && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      Expr *rhs = parseBinary(kRelationalPrec + 1);
      auto *bin = ctx_.make<BinaryExpr>(loc, BinaryOp::InstanceOf, lhs, rhs);
      bin->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
      lhs = bin;
      continue;
    }
    int prec = binaryPrec(tok_.kind);
    if (prec < 0 || prec < minPrec)
      return lhs;
    BinaryOp op = binaryOpFor(tok_.kind);
    llvm::SMLoc loc = tok_.loc();
    // `==` never converts its operands (both sides must have the same type),
    // so the strict form of JavaScript has nothing left to do.
    if (tok_.is(Tok::EqEqEq))
      report(loc, "'===' is not supported; use '==', which does not convert types");
    else if (tok_.is(Tok::BangEqEq))
      report(loc, "'!==' is not supported; use '!=', which does not convert types");
    advance();
    Expr *rhs = parseBinary(prec == kPowPrec ? prec : prec + 1);
    auto *bin = ctx_.make<BinaryExpr>(loc, op, lhs, rhs);
    bin->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
    lhs = bin;
  }
}

Expr *Parser::parseUnary() {
  llvm::SMLoc loc = tok_.loc();
  // `await x`: a file is a module, where `await` is always the operator.
  if (tok_.is(Tok::Identifier) && tok_.text == "await") {
    advance();
    return ctx_.make<AwaitExpr>(loc, parseUnary());
  }
  std::optional<UnaryOp> op;
  switch (tok_.kind) {
  case Tok::Minus: op = UnaryOp::Neg; break;
  case Tok::Plus: op = UnaryOp::Plus; break;
  case Tok::Bang: op = UnaryOp::Not; break;
  case Tok::Tilde: op = UnaryOp::BitNot; break;
  case Tok::KwTypeof: op = UnaryOp::TypeOf; break;
  case Tok::KwVoid: op = UnaryOp::Void; break;
  case Tok::KwDelete: op = UnaryOp::Delete; break;
  case Tok::PlusPlus:
  case Tok::MinusMinus: {
    bool increment = tok_.is(Tok::PlusPlus);
    advance();
    return ctx_.make<UpdateExpr>(loc, increment, /*prefix=*/true, parseUnary());
  }
  default:
    return parsePostfix();
  }
  advance();
  return ctx_.make<UnaryExpr>(loc, *op, parseUnary());
}

Expr *Parser::parsePostfix() {
  Expr *expr = parseCallOrMember();
  // A line break before `++`/`--` makes it a prefix operator of the next line.
  if ((tok_.is(Tok::PlusPlus) || tok_.is(Tok::MinusMinus)) && !tok_.newlineBefore) {
    bool increment = tok_.is(Tok::PlusPlus);
    llvm::SMLoc loc = tok_.loc();
    advance();
    return ctx_.make<UpdateExpr>(loc, increment, /*prefix=*/false, expr);
  }
  return expr;
}

// Calls, `.name`, `[index]`, `x!`, and their optional forms `?.(`, `?.name`
// and `?.[`. After a `?.` the rest of them form an optional chain. Also
// tagged templates. A property name may be a keyword, as in `set.delete(x)`.
Expr *Parser::parseCallOrMember() {
  Expr *expr = parsePrimary();
  bool chain = false;
  auto finish = [&]() -> Expr * {
    return chain ? ctx_.make<OptionalChainExpr>(expr->loc(), expr) : expr;
  };
  for (;;) {
    // `f<int>(x)` and `f<int>`: the type arguments of a generic function.
    if (auto *name = dyn_cast<NameExpr>(expr);
        name && !chain && tok_.is(Tok::Less) && name->typeArgs.empty() && tryTypeArgs(name->typeArgs))
      continue;
    // `JSON.parse<User>(text)`: type arguments of a method, right before its
    // call.
    if (auto *member = dyn_cast<MemberExpr>(expr);
        member && tok_.is(Tok::Less) && member->typeArgs.empty() &&
        tryTypeArgs(member->typeArgs, /*callOnly=*/true))
      continue;
    bool optional = false;
    if (tok_.is(Tok::QuestionDot)) {
      optional = chain = true;
      advance();
      if (tok_.is(Tok::Identifier) || isKeyword(tok_.kind)) {
        auto *member = ctx_.make<MemberExpr>(tok_.loc(), expr, tok_.text);
        member->optional = true;
        expr = member;
        advance();
        continue;
      }
      if (!tok_.is(Tok::LParen) && !tok_.is(Tok::LBracket)) {
        error(tok_.loc(), "expected a property name, '[' or '(' after '?.'");
        return finish();
      }
    }
    if (tok_.is(Tok::LParen)) {
      // Diagnostics about a call point at its start, e.g. `f` in `f(x)`.
      llvm::SMLoc loc = expr->loc();
      advance();
      llvm::SaveAndRestore<bool> allowIn(noIn_, false);
      std::vector<Expr *> args;
      std::vector<bool> spread;
      while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
        bool spreads = accept(Tok::Ellipsis);
        if (spreads || !spread.empty()) {
          spread.resize(args.size(), false);
          spread.push_back(spreads);
        }
        args.push_back(parseExpr());
        if (!accept(Tok::Comma))
          break;
      }
      expect(Tok::RParen, "to close the argument list");
      auto *call = ctx_.make<CallExpr>(loc, expr, std::move(args));
      call->optional = optional;
      call->spread = std::move(spread);
      expr = call;
    } else if (tok_.is(Tok::Dot)) {
      advance();
      llvm::SMLoc loc = tok_.loc();
      if (!tok_.is(Tok::Identifier) && !isKeyword(tok_.kind)) {
        error(tok_.loc(), "expected a property name after '.'");
        return finish();
      }
      expr = ctx_.make<MemberExpr>(loc, expr, tok_.text);
      advance();
    } else if (tok_.is(Tok::LBracket)) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      llvm::SaveAndRestore<bool> allowIn(noIn_, false);
      Expr *index = parseSequence();
      expect(Tok::RBracket, "to close the index");
      auto *access = ctx_.make<IndexExpr>(loc, expr, index);
      access->optional = optional;
      expr = access;
    } else if (tok_.is(Tok::TemplateString) || tok_.is(Tok::TemplateHead)) {
      // tag`a${x}b${y}c` calls the tag with the texts around the values, as
      // one array, and then the values: tag(["a", "b", "c"], x, y). String.raw
      // gets the texts as written, escapes and all.
      if (chain)
        report(tok_.loc(), "a tagged template cannot be part of an optional chain; check the tag "
                           "before the template");
      auto *member = dyn_cast<MemberExpr>(expr);
      auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
      bool raw = object && object->name == "String" && member->name == "raw";
      llvm::SMLoc loc = expr->loc();
      TemplateExpr *tmpl = parseTemplate(raw);
      auto *texts = ctx_.make<ArrayLitExpr>(tmpl->loc());
      for (std::u16string &text : tmpl->quasis)
        texts->elements.push_back({ctx_.make<StringLitExpr>(tmpl->loc(), std::move(text)), false});
      std::vector<Expr *> args{texts};
      args.insert(args.end(), tmpl->exprs.begin(), tmpl->exprs.end());
      auto *call = ctx_.make<CallExpr>(loc, expr, std::move(args));
      call->tagged = true;
      expr = call;
    } else if (tok_.is(Tok::Bang) && !tok_.newlineBefore) {
      // `x!`: x is not undefined (checked when the program runs).
      expr = ctx_.make<UnaryExpr>(tok_.loc(), UnaryOp::NonNull, expr);
      advance();
    } else {
      return finish();
    }
  }
}

Expr *Parser::parsePrimary() {
  llvm::SMLoc loc = tok_.loc();
  // `in` is an operator again inside brackets, even in a loop header.
  llvm::SaveAndRestore<bool> allowIn(noIn_, false);
  if (tok_.is(Tok::At)) {
    // `{ @Log m() {} }`, `@Log function () {}`: what follows is still parsed.
    rejectDecorators(parseDecorators());
    if (recovering_)
      return ctx_.make<IntLitExpr>(loc, 0);
    return parsePrimary();
  }
  switch (tok_.kind) {
  case Tok::IntLiteral:
    return parseIntLiteral();
  case Tok::FloatLiteral:
    return parseFloatLiteral();
  case Tok::StringLiteral: {
    std::u16string value;
    decodeStringLiteral(tok_, value, diags_);
    advance();
    return ctx_.make<StringLitExpr>(loc, std::move(value));
  }
  case Tok::TemplateString:
  case Tok::TemplateHead:
    return parseTemplate();
  case Tok::Slash:
  case Tok::SlashAssign: {
    // An expression starts here, so `/` starts a regex literal, which the
    // lexer reads again from there (it took the `/` for division).
    if (hasPeeked_) {
      lexer_.restore(peekState_);
      hasPeeked_ = false;
    }
    tok_ = lexer_.regex(tok_.text.data(), tok_.newlineBefore);
    std::u16string pattern, flags;
    decodeRegex(tok_, pattern, flags);
    auto *lit = ctx_.make<RegexLitExpr>(loc, std::move(pattern), std::move(flags));
    lit->written = tok_.text.size();
    advance();
    return lit;
  }
  case Tok::KwTrue:
  case Tok::KwFalse: {
    bool value = tok_.is(Tok::KwTrue);
    advance();
    return ctx_.make<BoolLitExpr>(loc, value);
  }
  case Tok::Identifier: {
    if (tok_.text == "import") {
      // `import("./x")` and `import.meta`: what follows parses as a call or
      // a member of a placeholder.
      report(loc, peek().is(Tok::Dot)
                      ? "'import.meta' is not supported"
                      : "dynamic 'import()' is not supported: every file the program uses is "
                        "compiled into it; import by name at the top of the file, e.g. 'import "
                        "{ a } from \"./x\"'");
      advance();
      return ctx_.make<IntLitExpr>(loc, 0);
    }
    if (peek().is(Tok::Arrow) && !peek().newlineBefore)
      return parseArrow(); // `x => ...`
    if (tok_.text == "async")
      if (Expr *fn = tryAsyncFunction())
        return fn;
    if (tok_.text == "class" && (peek().is(Tok::Identifier) || peek().is(Tok::LBrace))) {
      // `const C = class { ... }`, `return class extends Base { ... }`.
      ClassDecl *cls = parseClass(/*expression=*/true);
      return ctx_.make<ClassExpr>(loc, cls);
    }
    if (tok_.text == "yield" && inGenerator_) {
      // `1 + yield x` is an error in JavaScript: the operand must be in
      // parentheses.
      report(loc, "'yield' must be in parentheses here, e.g. '1 + (yield x)'");
      return parseYield();
    }
    llvm::StringRef name = tok_.text;
    advance();
    if (name == "undefined")
      return ctx_.make<UndefinedExpr>(loc);
    return ctx_.make<NameExpr>(loc, name);
  }
  case Tok::KwFunction: {
    FunctionDecl *fn = parseFunction(/*expression=*/true);
    return ctx_.make<FunctionExpr>(loc, fn);
  }
  case Tok::LParen: {
    if (arrowAhead())
      return parseArrow();
    advance();
    Expr *inner = parseSequence();
    expect(Tok::RParen, "to close the parenthesized expression");
    return inner;
  }
  case Tok::LBracket:
    return parseArrayLiteral();
  case Tok::LBrace:
    return parseObjectLiteral();
  case Tok::KwNew:
    return parseNew();
  case Tok::Less: {
    // `<T>(x: T) => x`, a generic arrow.
    Snapshot start = snapshot();
    const char *text = tok_.text.data();
    bool muted = diags_.setMuted(true);
    recovering_ = false;
    std::vector<TypeParamDecl> typeParams = parseTypeParams();
    bool arrow = !recovering_ && tok_.is(Tok::LParen) && arrowAhead();
    diags_.setMuted(muted);
    if (arrow) {
      recovering_ = start.recovering;
      return parseArrow(std::move(typeParams), text);
    }
    rewind(std::move(start));
    error(loc, "expected an expression, found '<'");
    return ctx_.make<IntLitExpr>(loc, 0);
  }
  default:
    error(loc, llvm::Twine("expected an expression, found ") + tokSpelling(tok_.kind));
    // Placeholder so callers never see null; the source will not compile anyway.
    return ctx_.make<IntLitExpr>(loc, 0);
  }
}

Expr *Parser::parseArrayLiteral() {
  auto *lit = ctx_.make<ArrayLitExpr>(tok_.loc());
  advance(); // '['
  while (!tok_.is(Tok::RBracket) && !tok_.is(Tok::Eof)) {
    if (tok_.is(Tok::Comma)) {
      // A hole: an error, unless the literal turns out to be a pattern, as in
      // `[, b] = pair`.
      lit->elements.push_back({ctx_.make<UndefinedExpr>(tok_.loc()), false, /*hole=*/true});
      advance();
      continue;
    }
    bool spread = accept(Tok::Ellipsis);
    lit->elements.push_back({parseExpr(), spread});
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RBracket, "to close the array");
  return lit;
}

// `{ x: 1, y }`
Expr *Parser::parseObjectLiteral() {
  auto *lit = ctx_.make<ObjectLitExpr>(tok_.loc());
  advance(); // '{'
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    // `{ @Log m() {} }`: what follows is still parsed.
    if (tok_.is(Tok::At)) {
      rejectDecorators(parseDecorators());
      if (recovering_)
        return lit;
    }
    llvm::SMLoc loc = tok_.loc();
    if (accept(Tok::Ellipsis)) {
      // `{ ...o }` copies the fields of o.
      ObjectLitExpr::Property spread;
      spread.loc = loc;
      spread.value = parseExpr();
      spread.spread = true;
      lit->props.push_back(std::move(spread));
      if (!accept(Tok::Comma))
        break;
      continue;
    }
    if (!symbolKeyAhead() && accept(Tok::LBracket)) {
      // `[key]: value`, where the key must be a constant (see Sema).
      ObjectLitExpr::Property prop;
      prop.loc = loc;
      prop.key = parseExpr();
      expect(Tok::RBracket, "to close the key");
      // The field it names, if the key is known by now (see fieldNames).
      if (auto *name = dyn_cast<NameExpr>(prop.key))
        for (const std::string &text : stringConstants_.lookup(name->name))
          fieldNames_.insert(text);
      else if (auto *text = dyn_cast<StringLitExpr>(prop.key))
        fieldNames_.insert(toUtf8(text->value));
      if (tok_.is(Tok::LParen) || tok_.is(Tok::Less)) {
        error(loc, "a method cannot have a key in brackets; write its name: 'f() { ... }'");
        skipToClosingBrace();
        return lit;
      }
      if (!expect(Tok::Colon, "after the key in brackets")) {
        skipToClosingBrace();
        return lit;
      }
      prop.value = parseExpr();
      lit->props.push_back(std::move(prop));
      if (!accept(Tok::Comma))
        break;
      continue;
    }
    // `get x() {}`, `set x(v) {}`, `async f() {}`
    bool accessor = tok_.is(Tok::Identifier) && (tok_.text == "get" || tok_.text == "set") &&
                    (peek().is(Tok::Identifier) || isKeyword(peek().kind) || peek().is(Tok::StringLiteral));
    if (accessor) {
      error(loc, "getters and setters are not supported: reading or writing a field never runs code; "
                 "write a function instead");
      skipToClosingBrace();
      return lit;
    }
    // `async f() {}`
    bool isAsync = tok_.is(Tok::Identifier) && tok_.text == "async" && !peek().is(Tok::Colon) &&
                   !peek().is(Tok::Comma) && !peek().is(Tok::RBrace) && !peek().is(Tok::LParen) &&
                   !peek().newlineBefore;
    if (isAsync)
      advance();
    const char *start = tok_.text.data();
    bool generator = accept(Tok::Star); // `*items() { yield ... }`
    bool identifier = tok_.is(Tok::Identifier);
    llvm::StringRef name;
    if (!parseFieldName(name)) {
      skipToClosingBrace();
      return lit;
    }
    Expr *value = nullptr;
    bool defaulted = false, method = false;
    if (tok_.is(Tok::LParen) || tok_.is(Tok::Less)) {
      // `f() { ... }`: a method, which sees the object as `this`.
      auto *fn = ctx_.make<FunctionDecl>(loc, name);
      fn->isExpression = true;
      fn->isGenerator = generator;
      fn->isAsync = isAsync;
      parseMethodRest(fn, start);
      if (recovering_ || !fn->body) {
        if (!fn->body)
          error(tok_.loc(), "expected '{' to start the body of the method");
        skipToClosingBrace();
        return lit;
      }
      value = ctx_.make<FunctionExpr>(loc, fn);
      method = true;
    } else if (generator || isAsync) {
      error(tok_.loc(), generator ? "expected '(' after the name of the generator method"
                                  : "expected '(' after the name of the async method");
      skipToClosingBrace();
      return lit;
    } else if (accept(Tok::Colon)) {
      value = parseExpr();
    } else if (identifier && (tok_.is(Tok::Comma) || tok_.is(Tok::RBrace))) {
      // `{ x }` is `{ x: x }`.
      value = name == "undefined" ? static_cast<Expr *>(ctx_.make<UndefinedExpr>(loc))
                                  : ctx_.make<NameExpr>(loc, name);
    } else if (identifier && tok_.is(Tok::Assign)) {
      // `{ x = 1 }`: a default value, which only a destructuring pattern
      // such as `({ x = 1 } = o)` may have (Sema reports it otherwise).
      llvm::SMLoc assignLoc = tok_.loc();
      advance();
      value = ctx_.make<AssignExpr>(assignLoc, std::nullopt, ctx_.make<NameExpr>(loc, name),
                                    parseExpr());
      defaulted = true;
    } else {
      error(tok_.loc(), llvm::Twine("expected ':' after the field name, found ") + tokSpelling(tok_.kind));
      skipToClosingBrace();
      return lit;
    }
    ObjectLitExpr::Property prop;
    prop.name = name;
    prop.loc = loc;
    prop.value = value;
    prop.defaulted = defaulted;
    prop.method = method;
    lit->props.push_back(std::move(prop));
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RBrace, "to close the object");
  return lit;
}

// `new Name<T>(args)`
Expr *Parser::parseNew() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'new'
  // `new (Mixin(User))()`: a class as a value (see NewExpr::callee).
  Expr *callee = nullptr;
  if (tok_.is(Tok::LParen)) {
    llvm::SMLoc at = tok_.loc();
    advance();
    llvm::SaveAndRestore<bool> allowIn(noIn_, false);
    callee = parseSequence();
    expect(Tok::RParen, "to close the class");
    std::vector<Expr *> args;
    if (accept(Tok::LParen)) {
      while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
        args.push_back(parseExpr());
        if (!accept(Tok::Comma))
          break;
      }
      expect(Tok::RParen, "to close the argument list");
    }
    auto *create = ctx_.make<NewExpr>(at, "", std::vector<TypeRef>(), std::move(args));
    create->callee = callee;
    return create;
  }
  if (!tok_.is(Tok::Identifier)) {
    error(tok_.loc(), "expected a class name after 'new'");
    return ctx_.make<IntLitExpr>(loc, 0);
  }
  llvm::StringRef name = tok_.text;
  llvm::SMLoc nameLoc = tok_.loc();
  advance();
  // `new Intl.DateTimeFormat()`: a constructor of a namespace.
  llvm::StringRef member;
  llvm::SMLoc memberLoc;
  if (accept(Tok::Dot)) {
    if (tok_.is(Tok::Identifier)) {
      member = tok_.text;
      memberLoc = tok_.loc();
      advance();
    } else {
      error(tok_.loc(), "expected a name after '.'");
    }
  }
  // `new this.types[i]()`, `new a.b.c()`: a class as a value.
  if (tok_.is(Tok::Dot) || tok_.is(Tok::LBracket)) {
    callee = ctx_.make<NameExpr>(nameLoc, name);
    if (!member.empty())
      callee = ctx_.make<MemberExpr>(memberLoc, callee, member);
    while (tok_.is(Tok::Dot) || tok_.is(Tok::LBracket)) {
      if (accept(Tok::Dot)) {
        if (!tok_.is(Tok::Identifier) && !isKeyword(tok_.kind)) {
          error(tok_.loc(), "expected a property name after '.'");
          break;
        }
        callee = ctx_.make<MemberExpr>(tok_.loc(), callee, tok_.text);
        advance();
        continue;
      }
      llvm::SMLoc at = tok_.loc();
      advance(); // '['
      llvm::SaveAndRestore<bool> allowIn(noIn_, false);
      Expr *index = parseSequence();
      expect(Tok::RBracket, "to close the index");
      callee = ctx_.make<IndexExpr>(at, callee, index);
    }
    name = member = {};
  }
  std::vector<TypeRef> typeArgs;
  if (tok_.is(Tok::Less))
    typeArgs = parseTypeArgs();
  std::vector<Expr *> args;
  if (accept(Tok::LParen)) {
    while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
      args.push_back(parseExpr());
      if (!accept(Tok::Comma))
        break;
    }
    expect(Tok::RParen, "to close the argument list");
  }
  auto *create = ctx_.make<NewExpr>(nameLoc, name, std::move(typeArgs), std::move(args));
  create->member = member;
  create->callee = callee;
  return create;
}

TemplateExpr *Parser::parseTemplate(bool raw) {
  auto *tmpl = ctx_.make<TemplateExpr>(tok_.loc());
  std::u16string text;
  auto decode = [&] {
    if (raw)
      decodeTemplateRaw(tok_, text);
    else
      decodeTemplatePart(tok_, text, diags_);
  };
  decode();
  tmpl->quasis.push_back(text);
  bool done = tok_.is(Tok::TemplateString);
  advance();
  while (!done) {
    tmpl->exprs.push_back(parseSequence());
    if (!tok_.is(Tok::TemplateMiddle) && !tok_.is(Tok::TemplateTail)) {
      error(tok_.loc(), "expected '}' to close the template substitution");
      tmpl->quasis.emplace_back();
      return tmpl;
    }
    decode();
    tmpl->quasis.push_back(text);
    done = tok_.is(Tok::TemplateTail);
    advance();
  }
  return tmpl;
}

Expr *Parser::parseIntLiteral() {
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef source = tok_.text;
  llvm::SmallString<32> digits;
  for (char c : tok_.text)
    if (c != '_')
      digits.push_back(c);
  advance();

  llvm::StringRef text = digits;
  unsigned radix = 10;
  if (text.size() > 2 && text[0] == '0') {
    switch (text[1]) {
    case 'x': case 'X': radix = 16; break;
    case 'b': case 'B': radix = 2; break;
    case 'o': case 'O': radix = 8; break;
    default: break;
    }
    if (radix != 10)
      text = text.drop_front(2);
  }

  uint64_t value = 0;
  if (!text.getAsInteger(radix, value)) {
    auto *lit = ctx_.make<IntLitExpr>(loc, value);
    lit->text = source; // a decimal reads its value from the text
    return lit;
  }
  for (char c : text) {
    unsigned digit = llvm::isDigit(c) ? c - '0' : llvm::isAlpha(c) ? (c | 0x20) - 'a' + 10 : radix;
    if (digit >= radix) {
      report(loc, "invalid integer literal");
      return ctx_.make<IntLitExpr>(loc, 0);
    }
  }
  // Beyond 64 bits: the nearest double, as in JavaScript.
  double rounded = 0;
  if (radix == 10) {
    text.getAsDouble(rounded);
  } else {
    unsigned bits = static_cast<unsigned>(text.size()) * (radix == 16 ? 4 : radix == 8 ? 3 : 1);
    llvm::APFloat number(llvm::APFloat::IEEEdouble());
    number.convertFromAPInt(llvm::APInt(bits, text, static_cast<uint8_t>(radix)), /*IsSigned=*/false,
                            llvm::APFloat::rmNearestTiesToEven);
    rounded = number.convertToDouble();
  }
  auto *lit = ctx_.make<FloatLitExpr>(loc, rounded);
  lit->text = source;
  return lit;
}

Expr *Parser::parseFloatLiteral() {
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef text = tok_.text;
  llvm::SmallString<32> digits;
  for (char c : tok_.text)
    if (c != '_')
      digits.push_back(c);
  advance();

  double value = 0;
  if (llvm::StringRef(digits).getAsDouble(value))
    report(loc, "invalid number literal");
  auto *lit = ctx_.make<FloatLitExpr>(loc, value);
  lit->text = text;
  return lit;
}

} // namespace tz
