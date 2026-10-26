#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class TokenCursor {
public:
  explicit TokenCursor(Parser &parser);

  void advance();
  const Token &peek();
  bool accept(Tok kind);
  bool expect(Tok kind, llvm::StringRef context = {});
  void expectSemicolon();
  void error(llvm::SMLoc loc, const llvm::Twine &message);
  void report(llvm::SMLoc loc, const llvm::Twine &message);
  void synchronize();
  void skipToClosingBrace();
  Parser::Snapshot snapshot() const;
  void rewind(Parser::Snapshot snapshot);
  llvm::StringRef textFrom(const char *start) const;

private:
  Parser &parser_;
};

}
