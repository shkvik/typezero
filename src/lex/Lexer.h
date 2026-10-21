#pragma once

#include "lex/Token.h"
#include "support/Diagnostics.h"

#include <string>

namespace tz {

class Lexer {
public:
  Lexer(llvm::StringRef source, Diagnostics &diags);

  Token next();

private:
  // Skips whitespace and comments; returns true if a line break was skipped.
  bool skipTrivia();
  char peek(size_t offset = 0) const { return cur_ + offset < end_ ? cur_[offset] : '\0'; }
  bool match(char c);
  Token make(Tok kind, const char *start, bool newlineBefore) const;
  Token lexNumber(const char *start, bool newlineBefore);
  Token lexString(const char *start, char quote, bool newlineBefore);
  Token lexIdentifier(const char *start, bool newlineBefore);

  const char *cur_;
  const char *end_;
  Diagnostics &diags_;
};

// Decodes the escapes of a string literal token (quotes included) into UTF-8.
// Returns false and reports an error on a malformed escape.
bool decodeStringLiteral(const Token &tok, std::string &out, Diagnostics &diags);

} // namespace tz
