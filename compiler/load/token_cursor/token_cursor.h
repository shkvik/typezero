#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// The tokens one by one for the other parsers: the current one, the next, what is
// expected, and the errors, after which the parser skips to the next statement.
class TokenCursor {
public:
  explicit TokenCursor(Parser &parser);

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
  Parser::Snapshot snapshot() const;
  void rewind(Parser::Snapshot snapshot);
  // The source text from `start` to the end of the last token, for messages.
  llvm::StringRef textFrom(const char *start) const;

private:
  Parser &parser_;
};

} // namespace tz
