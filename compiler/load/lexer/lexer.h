#pragma once

#include "compiler/load/token/token.h"
#include "compiler/program/diagnostics/diagnostics.h"

#include <string>
#include <vector>

namespace tz {

class Lexer {
public:
  Lexer(llvm::StringRef source, Diagnostics &diags);

  Token next();
  // A regex literal from the `/` at `start`, which next() took for division:
  // the parser knows that an expression starts there.
  Token regex(const char *start, bool newlineBefore);

  // Where the lexer is, so that the parser can look ahead and come back.
  struct State {
    const char *cur;
    std::vector<unsigned> templateBraces;
  };
  State save() const { return {cur_, templateBraces_}; }
  void restore(State state) {
    cur_ = state.cur;
    templateBraces_ = std::move(state.templateBraces);
  }

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
// The raw text of a template part, as String.raw gives it: escapes stay as
// written, and only line breaks are normalized to "\n".
void decodeTemplateRaw(const Token &tok, std::u16string &out);
// The pattern and the flags of a regex literal, as UTF-16; the escapes stay,
// they are the regex's own.
void decodeRegex(const Token &tok, std::u16string &pattern, std::u16string &flags);

} // namespace tz
