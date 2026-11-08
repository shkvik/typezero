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
  Token regex(const char *start, bool newlineBefore);

  struct State {
    const char *cur;
    std::vector<unsigned> templateBraces;
  };
  State save() const {
    return {this->cur, this->templateBraces};
  }
  void restore(State state) {
    this->cur = state.cur;
    this->templateBraces = std::move(state.templateBraces);
  }

private:
  static bool isIdentStart(char c);
  static bool isIdentChar(char c);
  bool skipTrivia();
  char peek(size_t offset = 0) const {
    return this->cur + offset < this->end ? this->cur[offset] : '\0';
  }
  bool match(char c);
  Token make(Tok kind, const char *start, bool newlineBefore) const;
  Token lexNumber(const char *start, bool newlineBefore);
  Token lexString(const char *start, char quote, bool newlineBefore);
  Token lexTemplate(const char *start, bool continuation, bool newlineBefore);
  Token lexIdentifier(const char *start, bool newlineBefore);

  const char *cur;
  const char *end;
  Diagnostics &diags;
  std::vector<unsigned> templateBraces;
};

}
