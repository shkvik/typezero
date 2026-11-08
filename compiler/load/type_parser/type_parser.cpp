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
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/var_parser/var_parser.h"
#include "compiler/program/to_utf8/to_utf8.h"
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

TypeParser::TypeParser(Parser &parser)
    : parser(parser) {}

TypeRef TypeParser::parseType() {
  const char *start = this->parser.tok.text.data();
  bool noConditional = std::exchange(this->parser.noConditional, false);
  bool outer = std::exchange(this->parser.disallowConditional, noConditional);
  TypeRef check = parseUnionType();
  this->parser.disallowConditional = outer;
  if (noConditional || !this->parser.tok.is(Tok::Identifier) ||
      this->parser.tok.text != "extends") {
    return check;
  }
  TypeRef ref;
  ref.form = ConditionalTypeRef{};
  ref.loc = check.loc;
  this->parser.cursor().advance();
  ref.args.push_back(std::move(check));
  this->parser.noConditional = true;
  ref.args.push_back(parseType());
  if (!this->parser.cursor().expect(
          Tok::Question,
          "after the 'extends' clause of the conditional type, e.g. 'T extends string "
          "? A : B'")) {
    return TypeRef();
  }
  ref.args.push_back(parseType());
  if (!this->parser.cursor().expect(Tok::Colon, "between the branches of the conditional type")) {
    return TypeRef();
  }
  ref.args.push_back(parseType());
  ref.source = this->parser.cursor().textFrom(start);

  return ref;
}

TypeRef TypeParser::parseUnionType() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().accept(Tok::Pipe);
  TypeRef first = parseIntersectionType();
  if (!this->parser.tok.is(Tok::Pipe)) {
    return first;
  }
  TypeRef ref;
  ref.form = UnionTypeRef{};
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (this->parser.cursor().accept(Tok::Pipe)) {
    ref.args.push_back(parseIntersectionType());
  }

  return ref;
}

TypeRef TypeParser::parseIntersectionType() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().accept(Tok::Amp);
  TypeRef first = parseArrayType();
  if (!this->parser.tok.is(Tok::Amp)) {
    return first;
  }
  TypeRef ref;
  ref.form = IntersectionTypeRef{};
  ref.loc = loc;
  ref.args.push_back(std::move(first));
  while (this->parser.cursor().accept(Tok::Amp)) {
    ref.args.push_back(parseArrayType());
  }

  return ref;
}

TypeRef TypeParser::parseReturnType() {
  auto onLine = [&](const char *text) {
    return this->parser.cursor().peek().is(Tok::Identifier) &&
           !this->parser.cursor().peek().newlineBefore &&
           (!text || this->parser.cursor().peek().text == text);
  };
  bool asserts =
      this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "asserts" && onLine(nullptr);
  if (!asserts && !(this->parser.tok.is(Tok::Identifier) && onLine("is"))) {
    return parseType();
  }
  TypeRef ref;
  ref.form = PredicateTypeRef{.asserts = asserts};
  ref.loc = this->parser.tok.loc();
  if (asserts) {
    this->parser.cursor().advance();
  }
  ref.name = this->parser.tok.text;
  this->parser.cursor().advance();
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "is" &&
      !this->parser.tok.newlineBefore) {
    this->parser.cursor().advance();
    ref.args.push_back(parseType());
  }

  return ref;
}

TypeRef TypeParser::parseArrayType() {
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "keyof" &&
      (this->parser.cursor().peek().is(Tok::Identifier) ||
       this->parser.cursor().peek().is(Tok::LParen) ||
       this->parser.cursor().peek().is(Tok::LBrace) ||
       this->parser.cursor().peek().is(Tok::KwTypeof))) {
    TypeRef ref;
    ref.form = KeyOfTypeRef{};
    ref.loc = this->parser.tok.loc();
    this->parser.cursor().advance();
    ref.args.push_back(parseArrayType());
    return ref;
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "readonly" &&
      (this->parser.cursor().peek().is(Tok::Identifier) ||
       this->parser.cursor().peek().is(Tok::LParen) ||
       this->parser.cursor().peek().is(Tok::LBracket))) {
    llvm::SMLoc loc = this->parser.tok.loc();
    this->parser.cursor().advance();
    TypeRef ref = parseArrayType();
    if (TupleTypeRef *tuple = ref.form.as<TupleTypeRef>()) {
      tuple->readonly.assign(ref.args.size(), true);
    } else if (ref.kind() != TypeRefKind::ARRAY_REF) {
      this->parser.cursor().report(loc, "'readonly' applies only to array and tuple types");
    }
    return ref;
  }
  TypeRef ref = parsePrimaryType();
  while (this->parser.tok.is(Tok::LBracket) && !this->parser.tok.newlineBefore) {
    TypeRef array;
    array.form = ArrayTypeRef{};
    array.loc = ref.loc;
    this->parser.cursor().advance();
    if (!this->parser.tok.is(Tok::RBracket)) {
      array.form = IndexedTypeRef{};
      array.args.push_back(std::move(ref));
      array.args.push_back(parseType());
      this->parser.cursor().expect(Tok::RBracket, "to close the indexed access type");
      ref = std::move(array);
      continue;
    }
    this->parser.cursor().expect(Tok::RBracket, "in the array type");
    array.args.push_back(std::move(ref));
    ref = std::move(array);
  }

  return ref;
}

TypeRef TypeParser::parsePrimaryType() {
  TypeRef ref;
  ref.loc = this->parser.tok.loc();
  if (this->parser.tok.is(Tok::LBrace)) {
    return this->parser.objectTypes().parseObjectType();
  }
  if (this->parser.tok.is(Tok::LBracket)) {
    return this->parser.objectTypes().parseTupleType();
  }
  if (this->parser.tok.is(Tok::Less)) {
    std::vector<TypeParamDecl> params = this->parser.functions().parseTypeParams();
    const char *start = this->parser.tok.text.data();
    if (this->parser.tok.is(Tok::LParen)) {
      this->parser.functionTypes().parseFunctionType();
    }
    std::string names;
    for (const TypeParamDecl &p : params) {
      names += (names.empty() ? "" : ", ") + p.name.str();
    }
    llvm::StringRef type(start, static_cast<size_t>(std::max(this->parser.prevEnd, start) - start));
    this->parser.cursor().report(
        ref.loc,
        "a function type cannot be generic itself: which copy of a generic function runs is "
        "decided when compiling, so a function value has fixed types; make the function that "
        "takes it generic instead, e.g. 'function apply<" +
            names + ">(f: " + type + ")'");
    return TypeRef();
  }
  if (this->parser.tok.is(Tok::LParen) && this->parser.functionTypes().functionTypeAhead()) {
    return this->parser.functionTypes().parseFunctionType();
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "infer" &&
      this->parser.cursor().peek().is(Tok::Identifier)) {
    ref.form = InferTypeRef{};
    this->parser.cursor().advance();
    ref.name = this->parser.tok.text;
    this->parser.cursor().advance();
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "extends") {
      Parser::Snapshot start = this->parser.cursor().snapshot();
      this->parser.cursor().advance();
      this->parser.noConditional = true;
      TypeRef constraint = parseType();
      if (this->parser.disallowConditional || !this->parser.tok.is(Tok::Question)) {
        ref.args.push_back(std::move(constraint));
      } else {
        this->parser.cursor().rewind(std::move(start));
      }
    }
    return ref;
  }
  if (this->parser.tok.is(Tok::TemplateString) || this->parser.tok.is(Tok::TemplateHead)) {
    std::u16string text;
    LiteralText::decodeTemplatePart(this->parser.tok, text, this->parser.diags);
    if (this->parser.tok.is(Tok::TemplateString)) {
      ref.form = LiteralTypeRef{.text = text};
      ref.name = "string";
      this->parser.cursor().advance();
      return ref;
    }
    ref.form = TemplateTypeRef{.texts = {text}};
    TemplateTypeRef *parts = ref.form.as<TemplateTypeRef>();
    this->parser.cursor().advance();
    for (bool done = false; !done;) {
      ref.args.push_back(parseType());
      if (!this->parser.tok.is(Tok::TemplateMiddle) && !this->parser.tok.is(Tok::TemplateTail)) {
        this->parser.cursor().error(this->parser.tok.loc(),
                                    "expected '}' to close the type in the template literal type");
        return TypeRef();
      }
      LiteralText::decodeTemplatePart(this->parser.tok, text, this->parser.diags);
      parts->texts.push_back(text);
      done = this->parser.tok.is(Tok::TemplateTail);
      this->parser.cursor().advance();
    }
    return ref;
  }
  if (this->parser.tok.is(Tok::StringLiteral)) {
    LiteralTypeRef literal;
    LiteralText::decodeStringLiteral(this->parser.tok, literal.text, this->parser.diags);
    ref.form = std::move(literal);
    ref.name = "string";
    this->parser.cursor().advance();
    return ref;
  }
  if (this->parser.tok.is(Tok::KwTrue) || this->parser.tok.is(Tok::KwFalse)) {
    double truth = this->parser.tok.is(Tok::KwTrue) ? 1 : 0;
    ref.form = LiteralTypeRef{.number = truth};
    ref.name = "boolean";
    this->parser.cursor().advance();
    return ref;
  }
  bool negative =
      this->parser.tok.is(Tok::Minus) && (this->parser.cursor().peek().is(Tok::IntLiteral) ||
                                          this->parser.cursor().peek().is(Tok::FloatLiteral));
  if (negative || this->parser.tok.is(Tok::IntLiteral) || this->parser.tok.is(Tok::FloatLiteral)) {
    if (negative) {
      this->parser.cursor().advance();
    }
    Expr *lit = this->parser.tok.is(Tok::IntLiteral) ? this->parser.primaries().parseIntLiteral()
                                                     : this->parser.primaries().parseFloatLiteral();
    LiteralTypeRef literal;
    if (auto *whole = dyn_cast<IntLitExpr>(lit)) {
      literal.number = double(whole->value);
      literal.integer = true;
    } else {
      literal.number = cast<FloatLitExpr>(lit)->value;
    }
    if (negative) {
      literal.number = -literal.number;
    }
    ref.form = std::move(literal);
    ref.name = "number";
    return ref;
  }
  if (this->parser.cursor().accept(Tok::LParen)) {
    ref = parseType();
    this->parser.cursor().expect(Tok::RParen, "to close the parenthesized type");
    return ref;
  }
  if (this->parser.tok.is(Tok::KwTypeof)) {
    ref.form = TypeOfTypeRef{};
    TypeOfTypeRef *path = ref.form.as<TypeOfTypeRef>();
    this->parser.cursor().advance();
    if (!this->parser.tok.is(Tok::Identifier)) {
      this->parser.cursor().error(
          this->parser.tok.loc(),
          llvm::Twine("expected the name of a value after 'typeof', found ") +
              TokenKinds::tokSpelling(this->parser.tok.kind));
      return ref;
    }
    path->names.push_back(this->parser.tok.text);
    this->parser.cursor().advance();
    while (this->parser.tok.is(Tok::Dot) && this->parser.cursor().peek().is(Tok::Identifier)) {
      this->parser.cursor().advance();
      path->names.push_back(this->parser.tok.text);
      this->parser.cursor().advance();
    }
    return ref;
  }
  if (!this->parser.tok.is(Tok::Identifier) && !this->parser.tok.is(Tok::KwVoid)) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                llvm::Twine("expected a type, found ") +
                                    TokenKinds::tokSpelling(this->parser.tok.kind));
    return ref;
  }
  ref.name = this->parser.tok.text;
  this->parser.cursor().advance();
  while (this->parser.tok.is(Tok::Dot) && this->parser.cursor().peek().is(Tok::Identifier)) {
    this->parser.cursor().advance();
    ref.name = this->parser.ctx.keepName(ref.name.str() + "." + this->parser.tok.text.str());
    this->parser.cursor().advance();
  }
  if (this->parser.tok.is(Tok::Less)) {
    ref.form = GenericTypeRef{};
    ref.args = parseTypeArgs();
  }

  return ref;
}

std::vector<TypeRef> TypeParser::parseTypeArgs() {
  std::vector<TypeRef> args;
  this->parser.cursor().advance();
  do {
    args.push_back(parseType());
  } while (this->parser.cursor().accept(Tok::Comma));

  switch (this->parser.tok.kind) {
    case Tok::Greater:
      this->parser.cursor().advance();
      break;
    case Tok::Shr:
    case Tok::UShr:
    case Tok::GreaterEq:
    case Tok::ShrAssign:
    case Tok::UShrAssign:
      this->parser.tok.kind = this->parser.tok.is(Tok::Shr)         ? Tok::Greater
                              : this->parser.tok.is(Tok::UShr)      ? Tok::Shr
                              : this->parser.tok.is(Tok::GreaterEq) ? Tok::Assign
                              : this->parser.tok.is(Tok::ShrAssign) ? Tok::GreaterEq
                                                                    : Tok::ShrAssign;
      this->parser.tok.text = this->parser.tok.text.drop_front();
      this->parser.tok.newlineBefore = false;
      break;
    default:
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected '>' to close the type arguments, found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      break;
  }

  return args;
}

bool TypeParser::tryTypeArgs(std::vector<TypeRef> &args, bool callOnly) {
  Parser::Snapshot start = this->parser.cursor().snapshot();
  bool muted = this->parser.diags.setMuted(true);
  this->parser.recovering = false;
  std::vector<TypeRef> parsed = parseTypeArgs();
  bool ok = !this->parser.recovering;
  switch (this->parser.tok.kind) {
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
      ok = ok && this->parser.tok.newlineBefore && !callOnly;
      break;
  }
  this->parser.diags.setMuted(muted);
  if (!ok) {
    this->parser.cursor().rewind(std::move(start));
    return false;
  }
  this->parser.recovering = start.recovering;
  args = std::move(parsed);

  return true;
}

}
