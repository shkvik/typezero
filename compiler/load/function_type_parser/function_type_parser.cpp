#include "compiler/load/function_type_parser/function_type_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/primary_parser/primary_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
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

namespace tz {

FunctionTypeParser::FunctionTypeParser(Parser &parser) : parser_(parser) {}

bool FunctionTypeParser::functionTypeAhead() {
  Parser::Snapshot start = parser_.cursor().snapshot();
  bool muted = parser_.diags_.setMuted(true);
  parser_.recovering_ = false;
  parser_.cursor().advance();
  bool ok = true;
  while (ok && !parser_.tok_.is(Tok::RParen)) {
    parser_.cursor().accept(Tok::Ellipsis);
    ok = parser_.cursor().accept(Tok::Identifier);
    parser_.cursor().accept(Tok::Question);
    if (ok && parser_.cursor().accept(Tok::Colon)) {
      parser_.types().parseType();
      ok = !parser_.recovering_;
    }
    if (ok && !parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  ok = ok && parser_.cursor().accept(Tok::RParen) && parser_.tok_.is(Tok::Arrow);
  parser_.diags_.setMuted(muted);
  parser_.cursor().rewind(std::move(start));

  return ok;
}

TypeRef FunctionTypeParser::parseFunctionType() {
  TypeRef ref;
  if (!parseSignatureParams(ref)) {
    return ref;
  }
  parser_.cursor().expect(Tok::Arrow, "after the parameters of the function type");
  ref.args.push_back(parser_.types().parseReturnType());

  return ref;
}

bool FunctionTypeParser::parseSignatureParams(TypeRef &ref) {
  ref.kind = TypeRef::Kind::Function;
  ref.loc = parser_.tok_.loc();
  parser_.cursor().advance();
  while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
    bool rest = parser_.cursor().accept(Tok::Ellipsis);
    llvm::SMLoc loc = parser_.tok_.loc();
    llvm::StringRef name = parser_.tok_.text;
    if (!parser_.cursor().expect(Tok::Identifier, "for a parameter of the function type")) {
      return false;
    }
    bool optional = parser_.cursor().accept(Tok::Question);
    TypeRef param;
    param.loc = loc;
    if (parser_.cursor().accept(Tok::Colon)) {
      param = parser_.types().parseType();
    } else {
      parser_.cursor().report(loc, "parameter '" + name +
                                       "' of the function type needs a type, e.g. '" + name +
                                       ": int'");
    }
    if (ref.rest) {
      parser_.cursor().report(loc, "a rest parameter must be the last parameter");
    }
    ref.args.push_back(std::move(param));
    ref.optional.push_back(optional);
    ref.names.push_back(name);
    ref.rest |= rest;
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RParen, "to close the parameters of the function type");

  return true;
}

}
