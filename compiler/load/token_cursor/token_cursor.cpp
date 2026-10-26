#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
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

namespace {

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

}

TokenCursor::TokenCursor(Parser &parser) : parser_(parser) {}

void TokenCursor::advance() {
  parser_.prevEnd_ = parser_.tok_.text.end();
  if (parser_.hasPeeked_) {
    parser_.tok_ = parser_.peeked_;
    parser_.hasPeeked_ = false;
    return;
  }
  parser_.tok_ = parser_.lexer_.next();
}

const Token &TokenCursor::peek() {
  if (!parser_.hasPeeked_) {
    parser_.peekState_ = parser_.lexer_.save();
    parser_.peeked_ = parser_.lexer_.next();
    parser_.hasPeeked_ = true;
  }

  return parser_.peeked_;
}

bool TokenCursor::accept(Tok kind) {
  if (!parser_.tok_.is(kind)) {
    return false;
  }
  advance();

  return true;
}

bool TokenCursor::expect(Tok kind, llvm::StringRef context) {
  if (accept(kind)) {
    return true;
  }
  std::string message = std::string("expected ") + tokSpelling(kind);
  if (!context.empty()) {
    message += " " + context.str();
  }
  error(parser_.tok_.loc(), message + ", found " + tokSpelling(parser_.tok_.kind));

  return false;
}

void TokenCursor::expectSemicolon() {
  if (accept(Tok::Semi)) {
    return;
  }
  if (parser_.tok_.is(Tok::RBrace) || parser_.tok_.is(Tok::Eof) || parser_.tok_.newlineBefore) {
    return;
  }
  error(parser_.tok_.loc(), llvm::Twine("expected ';', found ") + tokSpelling(parser_.tok_.kind));
}

void TokenCursor::error(llvm::SMLoc loc, const llvm::Twine &message) {
  if (parser_.recovering_) {
    return;
  }
  parser_.recovering_ = true;
  parser_.diags_.error(loc, message);
}

void TokenCursor::report(llvm::SMLoc loc, const llvm::Twine &message) {
  if (!parser_.recovering_) {
    parser_.diags_.error(loc, message);
  }
}

Parser::Snapshot TokenCursor::snapshot() const {
  return {parser_.lexer_.save(), parser_.peekState_, parser_.tok_,       parser_.peeked_,
          parser_.prevEnd_,      parser_.hasPeeked_, parser_.recovering_};
}

void TokenCursor::rewind(Parser::Snapshot s) {
  parser_.lexer_.restore(std::move(s.lexer));
  parser_.peekState_ = std::move(s.peekState);
  parser_.tok_ = s.tok;
  parser_.peeked_ = s.peeked;
  parser_.prevEnd_ = s.prevEnd;
  parser_.hasPeeked_ = s.hasPeeked;
  parser_.recovering_ = s.recovering;
}

void TokenCursor::synchronize() {
  parser_.recovering_ = false;
  for (;;) {
    if (parser_.tok_.is(Tok::Eof) || parser_.tok_.is(Tok::RBrace)) {
      return;
    }
    if (parser_.tok_.is(Tok::Semi)) {
      advance();
      return;
    }
    bool declaration = parser_.tok_.is(Tok::Identifier) &&
                       (parser_.tok_.text == "type" || parser_.tok_.text == "interface" ||
                        parser_.tok_.text == "namespace" || parser_.tok_.text == "module" ||
                        parser_.tok_.text == "declare" || parser_.tok_.text == "class") &&
                       peek().is(Tok::Identifier) && !peek().newlineBefore;
    declaration |= parser_.tok_.is(Tok::Identifier) &&
                   (parser_.tok_.text == "import" || parser_.tok_.text == "export" ||
                    parser_.tok_.text == "throw" || parser_.tok_.text == "try");
    if (parser_.tok_.newlineBefore && (startsStatement(parser_.tok_.kind) || declaration)) {
      return;
    }
    advance();
  }
}

void TokenCursor::skipToClosingBrace() {
  for (unsigned depth = 0; !parser_.tok_.is(Tok::Eof); advance()) {
    if (parser_.tok_.is(Tok::LBrace)) {
      ++depth;
    } else if (parser_.tok_.is(Tok::RBrace)) {
      if (depth == 0) {
        advance();
        return;
      }
      --depth;
    }
  }
}

llvm::StringRef TokenCursor::textFrom(const char *start) const {
  return llvm::StringRef(start, static_cast<size_t>(std::max(parser_.prevEnd_, start) - start));
}

}
