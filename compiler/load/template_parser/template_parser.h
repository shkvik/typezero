#pragma once

#include "compiler/load/parser/parser.h"

namespace tz {

class TemplateParser {
public:
  explicit TemplateParser(Parser &parser);

  TemplateExpr *parseTemplate(bool raw = false);

private:
  Parser &parser_;
};

}
