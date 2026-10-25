#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/token_cursor/token_cursor.h"
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

TemplateParser::TemplateParser(Parser &parser) : parser_(parser) {}

TemplateExpr *TemplateParser::parseTemplate(bool raw) {
  auto *tmpl = parser_.ctx_.make<TemplateExpr>(parser_.tok_.loc());
  std::u16string text;
  auto decode = [&] {
    if (raw) {
      decodeTemplateRaw(parser_.tok_, text);
    } else {
      decodeTemplatePart(parser_.tok_, text, parser_.diags_);
    }
  };
  decode();
  tmpl->quasis.push_back(text);
  bool done = parser_.tok_.is(Tok::TemplateString);
  parser_.cursor().advance();
  while (!done) {
    tmpl->exprs.push_back(parser_.exprs().parseSequence());
    if (!parser_.tok_.is(Tok::TemplateMiddle) && !parser_.tok_.is(Tok::TemplateTail)) {
      parser_.cursor().error(parser_.tok_.loc(), "expected '}' to close the template substitution");
      tmpl->quasis.emplace_back();
      return tmpl;
    }
    decode();
    tmpl->quasis.push_back(text);
    done = parser_.tok_.is(Tok::TemplateTail);
    parser_.cursor().advance();
  }

  return tmpl;
}

} // namespace tz
