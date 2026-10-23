#include "parse/parser.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

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
  case BinaryOp::Eq: return "===";
  case BinaryOp::Ne: return "!==";
  case BinaryOp::LogAnd: return "&&";
  case BinaryOp::LogOr: return "||";
  case BinaryOp::Nullish: return "??";
  }
  return "?";
}

namespace {

// Binding strength of binary operators, following JavaScript. `as` binds like
// the relational operators; it is parsed only for Sema to report that the
// language has no `as`.
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
  case Tok::GreaterEq: return kRelationalPrec;
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
    return true;
  default:
    return false;
  }
}

} // namespace

Parser::Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags)
    : lexer_(lexer), ctx_(ctx), diags_(diags) {
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
  return {lexer_.save(), tok_, peeked_, prevEnd_, hasPeeked_, recovering_};
}

void Parser::rewind(Snapshot s) {
  lexer_.restore(std::move(s.lexer));
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
    if (tok_.newlineBefore && startsStatement(tok_.kind))
      return;
    advance();
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
  return file;
}

Stmt *Parser::parseStatement(bool topLevel) {
  llvm::SMLoc loc = tok_.loc();
  switch (tok_.kind) {
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
  case Tok::Semi:
    advance();
    return ctx_.make<EmptyStmt>(loc);
  case Tok::Identifier:
    // `export` before a declaration is for tsc and other modules; a program
    // is one file for now, so it changes nothing.
    if (tok_.text == "export" && !peek().newlineBefore) {
      const Token &next = peek();
      if (next.is(Tok::KwFunction) || next.is(Tok::KwConst) || next.is(Tok::KwLet) ||
          next.is(Tok::KwEnum) || (next.is(Tok::Identifier) && next.text == "type")) {
        if (!topLevel)
          report(loc, "'export' is allowed only at the top level");
        advance();
        return parseStatement(topLevel);
      }
      if (next.is(Tok::KwDefault) || next.is(Tok::LBrace) || next.is(Tok::Star)) {
        error(loc, "only declarations can be exported; a program is one file for now");
        return ctx_.make<EmptyStmt>(loc);
      }
    }
    if (peek().is(Tok::Colon)) {
      llvm::StringRef label = tok_.text;
      advance();
      advance(); // ':'
      return ctx_.make<LabeledStmt>(loc, label, parseStatement());
    }
    // `type` declares an alias only when a name follows on the same line;
    // otherwise it is an ordinary name.
    if (tok_.text == "type" && peek().is(Tok::Identifier) && !peek().newlineBefore)
      return parseTypeAlias();
    [[fallthrough]];
  default: {
    Expr *expr = parseSequence();
    // `[a, b] = [b, a]`
    if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op)
      if (auto *lit = dyn_cast<ArrayLitExpr>(assign->target)) {
        Stmt *stmt = assignPattern(loc, *patternFromLiteral(lit), assign->value);
        expectSemicolon();
        return stmt;
      }
    expectSemicolon();
    return ctx_.make<ExprStmt>(loc, expr);
  }
  }
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

// `type Name = T`
Stmt *Parser::parseTypeAlias() {
  advance(); // 'type'
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef name = tok_.text;
  advance();
  if (tok_.is(Tok::Less)) {
    report(tok_.loc(), "generic type aliases are not supported yet");
    parseTypeArgs();
  }
  expect(Tok::Assign, "after the name of the type");
  TypeRef type = parseType();
  expectSemicolon();
  return ctx_.make<TypeAliasDecl>(loc, name, std::move(type));
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
    if (accept(Tok::Assign))
      member.init = parseExpr();
    decl->members.push_back(member);
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RBrace, "to close the enum");
  return decl;
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

// A function declaration, after any overload signatures (`function f(x:
// int): int` without a body): those are for tsc, and calls use the
// implementation, which must come right after them.
Stmt *Parser::parseFunctionDecl() {
  FunctionDecl *fn = parseFunction(/*expression=*/false);
  while (!fn->body) {
    llvm::StringRef name = fn->name;
    if (tok_.is(Tok::Identifier) && tok_.text == "export" && peek().is(Tok::KwFunction))
      advance();
    if (!tok_.is(Tok::KwFunction)) {
      report(fn->loc(), "the overload signature of '" + name +
                            "' must be followed by the function's implementation");
      return ctx_.make<EmptyStmt>(fn->loc());
    }
    fn = parseFunction(/*expression=*/false);
    if (fn->name != name)
      report(fn->loc(), "expected the implementation of '" + name + "' after its overload signature");
  }
  return fn;
}

FunctionDecl *Parser::parseFunction(bool expression) {
  const char *start = tok_.text.data();
  llvm::SMLoc keywordLoc = tok_.loc();
  advance(); // 'function'

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
  if (expression)
    fn->ownName = name;
  parseParams(fn);

  if (accept(Tok::Colon))
    fn->returnType = parseType();

  if (tok_.is(Tok::LBrace)) {
    fn->body = parseBlock();
  } else if (!expression &&
             (accept(Tok::Semi) || tok_.newlineBefore || tok_.is(Tok::RBrace) || tok_.is(Tok::Eof))) {
    // An overload signature: no body (see parseFunctionDecl).
  } else {
    error(tok_.loc(), "expected '{' to start the function body");
    fn->body = ctx_.make<BlockStmt>(tok_.loc());
  }
  fn->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
  return fn;
}

void Parser::parseParams(FunctionDecl *fn) {
  expect(Tok::LParen, fn->isArrow ? "to start the parameters" : "after the function name");
  while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
    Param param;
    param.rest = accept(Tok::Ellipsis);
    param.loc = tok_.loc();
    if (tok_.is(Tok::Identifier)) {
      param.name = tok_.text;
      advance();
    } else {
      error(tok_.loc(), "expected a parameter name");
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

FunctionExpr *Parser::parseArrow() {
  const char *start = tok_.text.data();
  llvm::SMLoc loc = tok_.loc();
  auto *fn = ctx_.make<FunctionDecl>(loc, "");
  fn->isExpression = fn->isArrow = true;
  if (tok_.is(Tok::Identifier)) {
    Param param;
    param.loc = tok_.loc();
    param.name = tok_.text;
    advance();
    fn->params.push_back(param);
  } else {
    parseParams(fn);
    if (accept(Tok::Colon))
      fn->returnType = parseType();
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
  fn->source = llvm::StringRef(start, static_cast<size_t>(prevEnd_ - start));
  return ctx_.make<FunctionExpr>(loc, fn);
}

FunctionDecl *Parser::parseFunctionExprCopy() {
  if (tok_.is(Tok::KwFunction))
    return parseFunction(/*expression=*/true);
  return parseArrow()->fn;
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
    if (tok_.is(Tok::LBracket)) {
      // `const [a, b]: int[] = value`
      std::unique_ptr<Pattern> pattern = parseBindingPattern();
      std::optional<TypeRef> typeAnn;
      if (accept(Tok::Colon))
        typeAnn = parseType();
      if (!tok_.is(Tok::Assign)) {
        error(tok_.loc(), "a destructuring declaration needs a value: '[a, b] = ...'");
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
    decl->decls.push_back(d);
  } while (accept(Tok::Comma));
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
    if (tok_.is(Tok::LBracket)) {
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

std::unique_ptr<Parser::Pattern> Parser::patternFromLiteral(ArrayLitExpr *lit) {
  auto pattern = std::make_unique<Pattern>();
  pattern->loc = lit->loc();
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
    if (auto *nested = dyn_cast<ArrayLitExpr>(target))
      e.nested = patternFromLiteral(nested);
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
  decl->decls.push_back(hidden);
  declareElements(decl, pattern, hidden.name);
}

void Parser::declareElements(VarDeclStmt *decl, const Pattern &pattern, llvm::StringRef base) {
  for (size_t i = 0; i < pattern.elements.size(); ++i) {
    const Pattern::Element &e = pattern.elements[i];
    if (e.hole)
      continue;
    Expr *value = elementOf(base, i, e);
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
// b = %1 }`.
Stmt *Parser::assignPattern(llvm::SMLoc loc, const Pattern &pattern, Expr *value) {
  auto *block = ctx_.make<BlockStmt>(loc);
  auto *decl = ctx_.make<VarDeclStmt>(loc, /*isConst=*/true);
  block->body.push_back(decl);
  auto *lit = dyn_cast<ArrayLitExpr>(value);
  bool direct = lit && lit->elements.size() >= pattern.elements.size() &&
                llvm::none_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; }) &&
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
    decl->decls.push_back(hidden);
  }
  llvm::StringRef base = decl->decls.front().name;
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
      element = elementOf(base, i, e);
    }
    if (e.nested)
      block->body.push_back(assignPattern(e.loc, *e.nested, element));
    else
      block->body.push_back(ctx_.make<ExprStmt>(e.loc, ctx_.make<AssignExpr>(e.loc, std::nullopt, e.target, element)));
  }
  return block;
}

Expr *Parser::elementOf(llvm::StringRef base, size_t i, const Pattern::Element &e) {
  auto *array = ctx_.make<NameExpr>(e.loc, base);
  auto *position = ctx_.make<IntLitExpr>(e.loc, i);
  if (e.rest) {
    auto *slice = ctx_.make<MemberExpr>(e.loc, array, "slice");
    return ctx_.make<CallExpr>(e.loc, slice, std::vector<Expr *>{position});
  }
  Expr *element = ctx_.make<IndexExpr>(e.loc, array, position);
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
  expect(Tok::LParen, "after 'for'");

  if (tok_.is(Tok::KwLet) || tok_.is(Tok::KwConst) || tok_.is(Tok::KwVar)) {
    if (tok_.is(Tok::KwVar))
      error(tok_.loc(), "'var' is not supported; use 'let' or 'const'");
    auto *decl = ctx_.make<VarDeclStmt>(tok_.loc(), tok_.is(Tok::KwConst));
    advance();
    VarDeclarator first;
    std::unique_ptr<Pattern> pattern;
    if (tok_.is(Tok::LBracket))
      pattern = parseBindingPattern();
    else if (!parseBinding(first))
      return loop;
    if (tok_.is(Tok::Identifier) && tok_.text == "of") {
      advance();
      auto *forOf = ctx_.make<ForOfStmt>(forLoc);
      forOf->isConst = decl->isConst;
      forOf->iterable = parseExpr();
      expect(Tok::RParen, "to close the loop header");
      forOf->body = parseStatement();
      if (!pattern) {
        forOf->binding = first;
        return forOf;
      }
      // `for (const [a, b] of pairs) body` is
      // `for (const %0 of pairs) { const a = %0[0], b = %0[1]; body }`.
      forOf->binding.name = ctx_.hiddenName();
      forOf->binding.loc = pattern->loc;
      forOf->binding.destructured = true;
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
  if (tok_.is(Tok::Identifier) && (tok_.text == "of" || tok_.text == "in")) {
    error(tok_.loc(), tok_.text == "in" ? "'for...in' loops are not supported; use 'for...of'"
                                        : "'for...of' needs a new variable: 'for (const x of ...)'");
    return loop;
  }
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

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

// type := '|'? arrayType ('|' arrayType)*
TypeRef Parser::parseType() {
  llvm::SMLoc loc = tok_.loc();
  accept(Tok::Pipe); // a leading `|` is allowed, as in TypeScript
  TypeRef first = parseArrayType();
  if (!tok_.is(Tok::Pipe))
    return first;
  TypeRef ref;
  ref.kind = TypeRef::Kind::Union;
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (accept(Tok::Pipe))
    ref.args.push_back(parseArrayType());
  return ref;
}

// arrayType := 'readonly'? primaryType ('[' ']')*
TypeRef Parser::parseArrayType() {
  // `readonly T[]` is for tsc: the compiler does not stop changes to the array.
  if (tok_.is(Tok::Identifier) && tok_.text == "readonly" &&
      (peek().is(Tok::Identifier) || peek().is(Tok::LParen))) {
    llvm::SMLoc loc = tok_.loc();
    advance();
    TypeRef ref = parseArrayType();
    if (ref.kind != TypeRef::Kind::Array)
      report(loc, "'readonly' applies only to array types");
    return ref;
  }
  TypeRef ref = parsePrimaryType();
  while (tok_.is(Tok::LBracket)) {
    TypeRef array;
    array.kind = TypeRef::Kind::Array;
    array.loc = ref.loc;
    advance();
    expect(Tok::RBracket, "in the array type");
    array.args.push_back(std::move(ref));
    ref = std::move(array);
  }
  return ref;
}

// primaryType := name ('<' type (',' type)* '>')? | '(' type ')'
TypeRef Parser::parsePrimaryType() {
  TypeRef ref;
  ref.loc = tok_.loc();
  if (tok_.is(Tok::LParen) && functionTypeAhead())
    return parseFunctionType();
  if (accept(Tok::LParen)) {
    ref = parseType();
    expect(Tok::RParen, "to close the parenthesized type");
    return ref;
  }
  if (!tok_.is(Tok::Identifier)) {
    error(tok_.loc(), llvm::Twine("expected a type, found ") + tokSpelling(tok_.kind));
    return ref;
  }
  ref.name = tok_.text;
  advance();
  if (tok_.is(Tok::Less)) {
    ref.kind = TypeRef::Kind::Generic;
    ref.args = parseTypeArgs();
  }
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
  ref.kind = TypeRef::Kind::Function;
  ref.loc = tok_.loc();
  advance(); // '('
  while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
    bool rest = accept(Tok::Ellipsis);
    llvm::SMLoc loc = tok_.loc();
    llvm::StringRef name = tok_.text;
    if (!expect(Tok::Identifier, "for a parameter of the function type"))
      return ref;
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
    ref.rest |= rest;
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RParen, "to close the parameters of the function type");
  expect(Tok::Arrow, "after the parameters of the function type");
  ref.args.push_back(parseType());
  return ref;
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

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

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
  Expr *lhs = parseConditional();
  std::optional<BinaryOp> op;
  if (!assignmentOp(tok_.kind, op))
    return lhs;
  llvm::SMLoc loc = tok_.loc();
  advance();
  Expr *rhs = parseExpr();
  return ctx_.make<AssignExpr>(loc, op, lhs, rhs);
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
  // `-x ** 2` is an error in JavaScript: it could mean either `(-x) ** 2` or
  // `-(x ** 2)`.
  llvm::StringRef unary;
  if (tok_.is(Tok::Minus) || tok_.is(Tok::Plus) || tok_.is(Tok::Bang) || tok_.is(Tok::Tilde) ||
      tok_.is(Tok::KwTypeof))
    unary = tok_.text;
  Expr *lhs = parseUnary();
  if (!unary.empty() && tok_.is(Tok::StarStar))
    report(tok_.loc(), "the left side of '**' cannot be a unary '" + unary +
                           "' expression; add parentheses, e.g. '(" + unary +
                           (unary == "typeof" ? " " : "") + "x) ** 2'");
  for (;;) {
    if (tok_.is(Tok::KwAs) && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      lhs = ctx_.make<CastExpr>(loc, lhs, parseType());
      continue;
    }
    int prec = binaryPrec(tok_.kind);
    if (prec < 0 || prec < minPrec)
      return lhs;
    BinaryOp op = binaryOpFor(tok_.kind);
    llvm::SMLoc loc = tok_.loc();
    advance();
    Expr *rhs = parseBinary(prec == kPowPrec ? prec : prec + 1);
    lhs = ctx_.make<BinaryExpr>(loc, op, lhs, rhs);
  }
}

Expr *Parser::parseUnary() {
  llvm::SMLoc loc = tok_.loc();
  std::optional<UnaryOp> op;
  switch (tok_.kind) {
  case Tok::Minus: op = UnaryOp::Neg; break;
  case Tok::Plus: op = UnaryOp::Plus; break;
  case Tok::Bang: op = UnaryOp::Not; break;
  case Tok::Tilde: op = UnaryOp::BitNot; break;
  case Tok::KwTypeof: op = UnaryOp::TypeOf; break;
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
// and `?.[`. After a `?.` the rest of them form an optional chain.
Expr *Parser::parseCallOrMember() {
  Expr *expr = parsePrimary();
  bool chain = false;
  auto finish = [&]() -> Expr * {
    return chain ? ctx_.make<OptionalChainExpr>(expr->loc(), expr) : expr;
  };
  for (;;) {
    bool optional = false;
    if (tok_.is(Tok::QuestionDot)) {
      optional = chain = true;
      advance();
      if (tok_.is(Tok::Identifier)) {
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
      if (!tok_.is(Tok::Identifier)) {
        error(tok_.loc(), "expected a property name after '.'");
        return finish();
      }
      expr = ctx_.make<MemberExpr>(loc, expr, tok_.text);
      advance();
    } else if (tok_.is(Tok::LBracket)) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      Expr *index = parseSequence();
      expect(Tok::RBracket, "to close the index");
      auto *access = ctx_.make<IndexExpr>(loc, expr, index);
      access->optional = optional;
      expr = access;
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
  case Tok::KwTrue:
  case Tok::KwFalse: {
    bool value = tok_.is(Tok::KwTrue);
    advance();
    return ctx_.make<BoolLitExpr>(loc, value);
  }
  case Tok::Identifier: {
    if (peek().is(Tok::Arrow) && !peek().newlineBefore)
      return parseArrow(); // `x => ...`
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
  case Tok::KwNew:
    return parseNew();
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

// `new Name<T>(args)`
Expr *Parser::parseNew() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'new'
  if (!tok_.is(Tok::Identifier)) {
    error(tok_.loc(), "expected a class name after 'new'");
    return ctx_.make<IntLitExpr>(loc, 0);
  }
  llvm::StringRef name = tok_.text;
  llvm::SMLoc nameLoc = tok_.loc();
  advance();
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
  return ctx_.make<NewExpr>(nameLoc, name, std::move(typeArgs), std::move(args));
}

Expr *Parser::parseTemplate() {
  auto *tmpl = ctx_.make<TemplateExpr>(tok_.loc());
  std::u16string text;
  decodeTemplatePart(tok_, text, diags_);
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
    decodeTemplatePart(tok_, text, diags_);
    tmpl->quasis.push_back(text);
    done = tok_.is(Tok::TemplateTail);
    advance();
  }
  return tmpl;
}

Expr *Parser::parseIntLiteral() {
  llvm::SMLoc loc = tok_.loc();
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
  if (!text.getAsInteger(radix, value))
    return ctx_.make<IntLitExpr>(loc, value);
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
  return ctx_.make<FloatLitExpr>(loc, rounded);
}

Expr *Parser::parseFloatLiteral() {
  llvm::SMLoc loc = tok_.loc();
  llvm::SmallString<32> digits;
  for (char c : tok_.text)
    if (c != '_')
      digits.push_back(c);
  advance();

  double value = 0;
  if (llvm::StringRef(digits).getAsDouble(value))
    report(loc, "invalid number literal");
  return ctx_.make<FloatLitExpr>(loc, value);
}

} // namespace tz
