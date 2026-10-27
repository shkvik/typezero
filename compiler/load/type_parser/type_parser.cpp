#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/function_type_parser/function_type_parser.h"
#include "compiler/load/literal_text/literal_text.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/object_type_parser/object_type_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/primary_parser/primary_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/var_parser/var_parser.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

TypeParser::TypeParser(Parser &parser) : parser_(parser) {}

TypeRef TypeParser::parseType() {
  const char *start = parser_.tok_.text.data();
  bool noConditional = std::exchange(parser_.noConditional_, false);
  bool outer = std::exchange(parser_.disallowConditional_, noConditional);
  TypeRef check = parseUnionType();
  parser_.disallowConditional_ = outer;
  if (noConditional || !parser_.tok_.is(Tok::Identifier) || parser_.tok_.text != "extends") {
    return check;
  }
  TypeRef ref;
  ref.kind = TypeRef::Kind::Conditional;
  ref.loc = check.loc;
  parser_.cursor().advance();
  ref.args.push_back(std::move(check));
  parser_.noConditional_ = true;
  ref.args.push_back(parseType());
  if (!parser_.cursor().expect(
          Tok::Question,
          "after the 'extends' clause of the conditional type, e.g. 'T extends string "
          "? A : B'")) {
    return TypeRef();
  }
  ref.args.push_back(parseType());
  if (!parser_.cursor().expect(Tok::Colon, "between the branches of the conditional type")) {
    return TypeRef();
  }
  ref.args.push_back(parseType());
  ref.source = parser_.cursor().textFrom(start);

  return ref;
}

TypeRef TypeParser::parseUnionType() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().accept(Tok::Pipe);
  TypeRef first = parseIntersectionType();
  if (!parser_.tok_.is(Tok::Pipe)) {
    return first;
  }
  TypeRef ref;
  ref.kind = TypeRef::Kind::Union;
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (parser_.cursor().accept(Tok::Pipe)) {
    ref.args.push_back(parseIntersectionType());
  }

  return ref;
}

TypeRef TypeParser::parseIntersectionType() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().accept(Tok::Amp);
  TypeRef first = parseArrayType();
  if (!parser_.tok_.is(Tok::Amp)) {
    return first;
  }
  TypeRef ref;
  ref.kind = TypeRef::Kind::Intersection;
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (parser_.cursor().accept(Tok::Amp)) {
    ref.args.push_back(parseArrayType());
  }

  return ref;
}

TypeRef TypeParser::parseReturnType() {
  auto onLine = [&](const char *text) {
    return parser_.cursor().peek().is(Tok::Identifier) && !parser_.cursor().peek().newlineBefore &&
           (!text || parser_.cursor().peek().text == text);
  };
  bool asserts =
      parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "asserts" && onLine(nullptr);
  if (!asserts && !(parser_.tok_.is(Tok::Identifier) && onLine("is"))) {
    return parseType();
  }
  TypeRef ref;
  ref.kind = TypeRef::Kind::Predicate;
  ref.loc = parser_.tok_.loc();
  ref.asserts = asserts;
  if (asserts) {
    parser_.cursor().advance();
  }
  ref.name = parser_.tok_.text;
  parser_.cursor().advance();
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "is" &&
      !parser_.tok_.newlineBefore) {
    parser_.cursor().advance();
    ref.args.push_back(parseType());
  }

  return ref;
}

TypeRef TypeParser::parseArrayType() {
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "keyof" &&
      (parser_.cursor().peek().is(Tok::Identifier) || parser_.cursor().peek().is(Tok::LParen) ||
       parser_.cursor().peek().is(Tok::LBrace) || parser_.cursor().peek().is(Tok::KwTypeof))) {
    TypeRef ref;
    ref.kind = TypeRef::Kind::KeyOf;
    ref.loc = parser_.tok_.loc();
    parser_.cursor().advance();
    ref.args.push_back(parseArrayType());
    return ref;
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "readonly" &&
      (parser_.cursor().peek().is(Tok::Identifier) || parser_.cursor().peek().is(Tok::LParen) ||
       parser_.cursor().peek().is(Tok::LBracket))) {
    llvm::SMLoc loc = parser_.tok_.loc();
    parser_.cursor().advance();
    TypeRef ref = parseArrayType();
    if (ref.kind == TypeRef::Kind::Tuple) {
      ref.readonly.assign(ref.args.size(), true);
    } else if (ref.kind != TypeRef::Kind::Array) {
      parser_.cursor().report(loc, "'readonly' applies only to array and tuple types");
    }
    return ref;
  }
  TypeRef ref = parsePrimaryType();
  while (parser_.tok_.is(Tok::LBracket) && !parser_.tok_.newlineBefore) {
    TypeRef array;
    array.kind = TypeRef::Kind::Array;
    array.loc = ref.loc;
    parser_.cursor().advance();
    if (!parser_.tok_.is(Tok::RBracket)) {
      array.kind = TypeRef::Kind::Indexed;
      array.args.push_back(std::move(ref));
      array.args.push_back(parseType());
      parser_.cursor().expect(Tok::RBracket, "to close the indexed access type");
      ref = std::move(array);
      continue;
    }
    parser_.cursor().expect(Tok::RBracket, "in the array type");
    array.args.push_back(std::move(ref));
    ref = std::move(array);
  }

  return ref;
}

TypeRef TypeParser::parsePrimaryType() {
  TypeRef ref;
  ref.loc = parser_.tok_.loc();
  if (parser_.tok_.is(Tok::LBrace)) {
    return parser_.objectTypes().parseObjectType();
  }
  if (parser_.tok_.is(Tok::LBracket)) {
    return parser_.objectTypes().parseTupleType();
  }
  if (parser_.tok_.is(Tok::Less)) {
    std::vector<TypeParamDecl> params = parser_.functions().parseTypeParams();
    const char *start = parser_.tok_.text.data();
    if (parser_.tok_.is(Tok::LParen)) {
      parser_.functionTypes().parseFunctionType();
    }
    std::string names;
    for (const TypeParamDecl &p : params) {
      names += (names.empty() ? "" : ", ") + p.name.str();
    }
    llvm::StringRef type(start, static_cast<size_t>(std::max(parser_.prevEnd_, start) - start));
    parser_.cursor().report(
        ref.loc,
        "a function type cannot be generic itself: which copy of a generic function runs is "
        "decided when compiling, so a function value has fixed types; make the function that "
        "takes it generic instead, e.g. 'function apply<" +
            names + ">(f: " + type + ")'");
    return TypeRef();
  }
  if (parser_.tok_.is(Tok::LParen) && parser_.functionTypes().functionTypeAhead()) {
    return parser_.functionTypes().parseFunctionType();
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "infer" &&
      parser_.cursor().peek().is(Tok::Identifier)) {
    ref.kind = TypeRef::Kind::Infer;
    parser_.cursor().advance();
    ref.name = parser_.tok_.text;
    parser_.cursor().advance();
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "extends") {
      Parser::Snapshot start = parser_.cursor().snapshot();
      parser_.cursor().advance();
      parser_.noConditional_ = true;
      TypeRef constraint = parseType();
      if (parser_.disallowConditional_ || !parser_.tok_.is(Tok::Question)) {
        ref.args.push_back(std::move(constraint));
      } else {
        parser_.cursor().rewind(std::move(start));
      }
    }
    return ref;
  }
  if (parser_.tok_.is(Tok::TemplateString) || parser_.tok_.is(Tok::TemplateHead)) {
    std::u16string text;
    decodeTemplatePart(parser_.tok_, text, parser_.diags_);
    if (parser_.tok_.is(Tok::TemplateString)) {
      ref.kind = TypeRef::Kind::Literal;
      ref.name = "string";
      ref.text = text;
      parser_.cursor().advance();
      return ref;
    }
    ref.kind = TypeRef::Kind::Template;
    ref.texts.push_back(text);
    parser_.cursor().advance();
    for (bool done = false; !done;) {
      ref.args.push_back(parseType());
      if (!parser_.tok_.is(Tok::TemplateMiddle) && !parser_.tok_.is(Tok::TemplateTail)) {
        parser_.cursor().error(parser_.tok_.loc(),
                               "expected '}' to close the type in the template literal type");
        return TypeRef();
      }
      decodeTemplatePart(parser_.tok_, text, parser_.diags_);
      ref.texts.push_back(text);
      done = parser_.tok_.is(Tok::TemplateTail);
      parser_.cursor().advance();
    }
    return ref;
  }
  if (parser_.tok_.is(Tok::StringLiteral)) {
    ref.kind = TypeRef::Kind::Literal;
    ref.name = "string";
    decodeStringLiteral(parser_.tok_, ref.text, parser_.diags_);
    parser_.cursor().advance();
    return ref;
  }
  if (parser_.tok_.is(Tok::KwTrue) || parser_.tok_.is(Tok::KwFalse)) {
    ref.kind = TypeRef::Kind::Literal;
    ref.name = "boolean";
    ref.number = parser_.tok_.is(Tok::KwTrue) ? 1 : 0;
    parser_.cursor().advance();
    return ref;
  }
  bool negative = parser_.tok_.is(Tok::Minus) && (parser_.cursor().peek().is(Tok::IntLiteral) ||
                                                  parser_.cursor().peek().is(Tok::FloatLiteral));
  if (negative || parser_.tok_.is(Tok::IntLiteral) || parser_.tok_.is(Tok::FloatLiteral)) {
    if (negative) {
      parser_.cursor().advance();
    }
    Expr *lit = parser_.tok_.is(Tok::IntLiteral) ? parser_.primaries().parseIntLiteral()
                                                 : parser_.primaries().parseFloatLiteral();
    ref.kind = TypeRef::Kind::Literal;
    ref.name = "number";
    if (auto *whole = dyn_cast<IntLitExpr>(lit)) {
      ref.number = double(whole->value);
      ref.integer = true;
    } else {
      ref.number = cast<FloatLitExpr>(lit)->value;
    }
    if (negative) {
      ref.number = -ref.number;
    }
    return ref;
  }
  if (parser_.cursor().accept(Tok::LParen)) {
    ref = parseType();
    parser_.cursor().expect(Tok::RParen, "to close the parenthesized type");
    return ref;
  }
  if (parser_.tok_.is(Tok::KwTypeof)) {
    ref.kind = TypeRef::Kind::TypeOf;
    parser_.cursor().advance();
    if (!parser_.tok_.is(Tok::Identifier)) {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected the name of a value after 'typeof', found ") +
                                 tokSpelling(parser_.tok_.kind));
      return ref;
    }
    ref.names.push_back(parser_.tok_.text);
    parser_.cursor().advance();
    while (parser_.tok_.is(Tok::Dot) && parser_.cursor().peek().is(Tok::Identifier)) {
      parser_.cursor().advance();
      ref.names.push_back(parser_.tok_.text);
      parser_.cursor().advance();
    }
    return ref;
  }
  if (!parser_.tok_.is(Tok::Identifier) && !parser_.tok_.is(Tok::KwVoid)) {
    parser_.cursor().error(parser_.tok_.loc(),
                           llvm::Twine("expected a type, found ") + tokSpelling(parser_.tok_.kind));
    return ref;
  }
  ref.name = parser_.tok_.text;
  parser_.cursor().advance();
  if (ref.name == "NodeJS" && parser_.tok_.is(Tok::Dot) &&
      parser_.cursor().peek().is(Tok::Identifier)) {
    parser_.cursor().advance();
    ref.name = parser_.ctx_.keepName(("NodeJS." + parser_.tok_.text).str());
    parser_.cursor().advance();
  }
  if ((ref.name == "Intl" || ref.name == "Bit") && parser_.tok_.is(Tok::Dot) &&
      parser_.cursor().peek().is(Tok::Identifier)) {
    parser_.cursor().advance();
    ref.name = parser_.ctx_.keepName(ref.name.str() + "." + parser_.tok_.text.str());
    parser_.cursor().advance();
  }
  if (parser_.tok_.is(Tok::Less)) {
    ref.kind = TypeRef::Kind::Generic;
    ref.args = parseTypeArgs();
  }

  return ref;
}

std::vector<TypeRef> TypeParser::parseTypeArgs() {
  std::vector<TypeRef> args;
  parser_.cursor().advance();
  do {
    args.push_back(parseType());
  } while (parser_.cursor().accept(Tok::Comma));

  switch (parser_.tok_.kind) {
  case Tok::Greater:
    parser_.cursor().advance();
    break;
  case Tok::Shr:
  case Tok::UShr:
  case Tok::GreaterEq:
  case Tok::ShrAssign:
  case Tok::UShrAssign:
    parser_.tok_.kind = parser_.tok_.is(Tok::Shr)         ? Tok::Greater
                        : parser_.tok_.is(Tok::UShr)      ? Tok::Shr
                        : parser_.tok_.is(Tok::GreaterEq) ? Tok::Assign
                        : parser_.tok_.is(Tok::ShrAssign) ? Tok::GreaterEq
                                                          : Tok::ShrAssign;
    parser_.tok_.text = parser_.tok_.text.drop_front();
    parser_.tok_.newlineBefore = false;
    break;
  default:
    parser_.cursor().error(parser_.tok_.loc(),
                           llvm::Twine("expected '>' to close the type arguments, found ") +
                               tokSpelling(parser_.tok_.kind));
    break;
  }

  return args;
}

bool TypeParser::tryTypeArgs(std::vector<TypeRef> &args, bool callOnly) {
  Parser::Snapshot start = parser_.cursor().snapshot();
  bool muted = parser_.diags_.setMuted(true);
  parser_.recovering_ = false;
  std::vector<TypeRef> parsed = parseTypeArgs();
  bool ok = !parser_.recovering_;
  switch (parser_.tok_.kind) {
  case Tok::LParen:
    break;
  case Tok::RParen:
  case Tok::RBracket:
  case Tok::RBrace:
  case Tok::Comma:
  case Tok::Semi:
  case Tok::Colon:
  case Tok::Eof:
    ok = ok && !callOnly;
    break;
  default:
    ok = ok && parser_.tok_.newlineBefore && !callOnly;
    break;
  }
  parser_.diags_.setMuted(muted);
  if (!ok) {
    parser_.cursor().rewind(std::move(start));
    return false;
  }
  parser_.recovering_ = start.recovering;
  args = std::move(parsed);

  return true;
}

}
