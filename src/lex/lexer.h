#pragma once

#include "lex/Token.h"
#include "support/Diagnostics.h"

#include <string>
#include <vector>

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
  // Lexes template text after '`' (continuation = false) or after the '}'
  // that closes a substitution (continuation = true).
  Token lexTemplate(const char *start, bool continuation, bool newlineBefore);
  Token lexIdentifier(const char *start, bool newlineBefore);

  const char *cur_;
  const char *end_;
  Diagnostics &diags_;
  // One entry per open `${`: the number of unclosed '{' inside it, so that the
  // matching '}' resumes the template.
  std::vector<unsigned> templateBraces_;
};

// Decode the text of a string literal or a template part into UTF-16 code
// units, the way JavaScript sees it. Return false after reporting a malformed
// escape.
bool decodeStringLiteral(const Token &tok, std::u16string &out, Diagnostics &diags);
bool decodeTemplatePart(const Token &tok, std::u16string &out, Diagnostics &diags);

} // namespace tz
