#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "compiler/program/to_utf8/to_utf8.h"
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

TokenCursor::TokenCursor(Parser &parser)
    : parser(parser) {}

bool TokenCursor::startsStatement(Tok kind) {
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

void TokenCursor::advance() {
  this->parser.prevEnd = this->parser.tok.text.end();
  if (this->parser.hasPeeked) {
    this->parser.tok = this->parser.peeked;
    this->parser.hasPeeked = false;
    return;
  }
  this->parser.tok = this->parser.lexer.next();
}

const Token &TokenCursor::peek() {
  if (!this->parser.hasPeeked) {
    this->parser.peekState = this->parser.lexer.save();
    this->parser.peeked = this->parser.lexer.next();
    this->parser.hasPeeked = true;
  }

  return this->parser.peeked;
}

bool TokenCursor::accept(Tok kind) {
  if (!this->parser.tok.is(kind)) {
    return false;
  }
  advance();

  return true;
}

bool TokenCursor::expect(Tok kind, llvm::StringRef context) {
  if (accept(kind)) {
    return true;
  }
  std::string message = std::string("expected ") + TokenKinds::tokSpelling(kind);
  if (!context.empty()) {
    message += " " + context.str();
  }
  error(this->parser.tok.loc(),
        message + ", found " + TokenKinds::tokSpelling(this->parser.tok.kind));

  return false;
}

void TokenCursor::expectSemicolon() {
  if (accept(Tok::Semi)) {
    return;
  }
  if (this->parser.tok.is(Tok::RBrace) || this->parser.tok.is(Tok::Eof) ||
      this->parser.tok.newlineBefore) {
    return;
  }
  error(this->parser.tok.loc(),
        llvm::Twine("expected ';', found ") + TokenKinds::tokSpelling(this->parser.tok.kind));
}

void TokenCursor::error(llvm::SMLoc loc, const llvm::Twine &message) {
  if (this->parser.recovering) {
    return;
  }
  this->parser.recovering = true;
  this->parser.diags.error(loc, message);
}

void TokenCursor::report(llvm::SMLoc loc, const llvm::Twine &message) {
  if (!this->parser.recovering) {
    this->parser.diags.error(loc, message);
  }
}

Parser::Snapshot TokenCursor::snapshot() const {
  return {this->parser.lexer.save(), this->parser.peekState, this->parser.tok,
          this->parser.peeked,       this->parser.prevEnd,   this->parser.hasPeeked,
          this->parser.recovering};
}

void TokenCursor::rewind(Parser::Snapshot s) {
  this->parser.lexer.restore(std::move(s.lexer));
  this->parser.peekState = std::move(s.peekState);
  this->parser.tok = s.tok;
  this->parser.peeked = s.peeked;
  this->parser.prevEnd = s.prevEnd;
  this->parser.hasPeeked = s.hasPeeked;
  this->parser.recovering = s.recovering;
}

void TokenCursor::synchronize() {
  this->parser.recovering = false;
  for (;;) {
    if (this->parser.tok.is(Tok::Eof) || this->parser.tok.is(Tok::RBrace)) {
      return;
    }
    if (this->parser.tok.is(Tok::Semi)) {
      advance();
      return;
    }
    bool declaration = this->parser.tok.is(Tok::Identifier) &&
                       (this->parser.tok.text == "type" || this->parser.tok.text == "interface" ||
                        this->parser.tok.text == "namespace" || this->parser.tok.text == "module" ||
                        this->parser.tok.text == "declare" || this->parser.tok.text == "class") &&
                       peek().is(Tok::Identifier) && !peek().newlineBefore;
    declaration |= this->parser.tok.is(Tok::Identifier) &&
                   (this->parser.tok.text == "import" || this->parser.tok.text == "export" ||
                    this->parser.tok.text == "throw" || this->parser.tok.text == "try");
    if (this->parser.tok.newlineBefore &&
        (TokenCursor::startsStatement(this->parser.tok.kind) || declaration)) {
      return;
    }
    advance();
  }
}

void TokenCursor::skipToClosingBrace() {
  for (unsigned depth = 0; !this->parser.tok.is(Tok::Eof); advance()) {
    if (this->parser.tok.is(Tok::LBrace)) {
      ++depth;
    } else if (this->parser.tok.is(Tok::RBrace)) {
      if (depth == 0) {
        advance();
        return;
      }
      --depth;
    }
  }
}

llvm::StringRef TokenCursor::textFrom(const char *start) const {
  return llvm::StringRef(start, static_cast<size_t>(std::max(this->parser.prevEnd, start) - start));
}

}
