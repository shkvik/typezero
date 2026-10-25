#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

// Template literals.
class TemplateParser {
public:
  explicit TemplateParser(Parser &parser);

  // A template literal; `raw` keeps the texts as written, for String.raw.
  TemplateExpr *parseTemplate(bool raw = false);

private:
  Parser &parser_;
};

} // namespace tz
