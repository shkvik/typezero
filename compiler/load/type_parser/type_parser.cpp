#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TypeParser::TypeParser(Parser &parser) : parser_(parser) {}

// type := unionType ('extends' unionType '?' type ':' type)?
TypeRef TypeParser::parseType() {
  const char *start = parser_.tok_.text.data();
  bool noConditional = std::exchange(parser_.noConditional_, false);
  bool outer = std::exchange(parser_.disallowConditional_, noConditional);
  TypeRef check = parseUnionType();
  parser_.disallowConditional_ = outer;
  if (noConditional || !parser_.tok_.is(Tok::Identifier) || parser_.tok_.text != "extends") {
    return check;
  }
  // `T extends string ? A : B`: a conditional type (docs/generics.md,
  // section 10).
  TypeRef ref;
  ref.kind = TypeRef::Kind::Conditional;
  ref.loc = check.loc;
  parser_.cursor().advance(); // 'extends'
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

// unionType := '|'? intersectionType ('|' intersectionType)*
TypeRef TypeParser::parseUnionType() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().accept(Tok::Pipe); // a leading `|` is allowed, as in TypeScript
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

// intersectionType := '&'? arrayType ('&' arrayType)*
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

// returnType := ('asserts'? (name | 'this') 'is' type) | 'asserts' (name | 'this') | type
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

// arrayType := 'readonly'? 'keyof'? primaryType ('[' type? ']')*
TypeRef TypeParser::parseArrayType() {
  // `keyof T`: the names of the fields of T.
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
  // `readonly T[]` is for tsc: the compiler does not stop changes to the array.
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "readonly" &&
      (parser_.cursor().peek().is(Tok::Identifier) || parser_.cursor().peek().is(Tok::LParen) ||
       parser_.cursor().peek().is(Tok::LBracket))) {
    llvm::SMLoc loc = parser_.tok_.loc();
    parser_.cursor().advance();
    TypeRef ref = parseArrayType();
    if (ref.kind == TypeRef::Kind::Tuple) {
      ref.readonly.assign(ref.args.size(), true); // its elements cannot be assigned to
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
    // `T[K]`: the type of the member K of T.
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

// primaryType := name ('<' type (',' type)* '>')? | '(' type ')' | objectType
TypeRef TypeParser::parsePrimaryType() {
  TypeRef ref;
  ref.loc = parser_.tok_.loc();
  if (parser_.tok_.is(Tok::LBrace)) {
    return parseObjectType();
  }
  if (parser_.tok_.is(Tok::LBracket)) {
    return parseTupleType();
  }
  if (parser_.tok_.is(Tok::Less)) {
    // `<X>(x: X) => X`, a function that is generic itself. Which copy of a
    // generic function runs is decided when compiling, so a value must have
    // its types fixed; the function that takes one can be generic instead.
    std::vector<TypeParamDecl> params = parser_.functions().parseTypeParams();
    const char *start = parser_.tok_.text.data();
    if (parser_.tok_.is(Tok::LParen)) {
      parseFunctionType();
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
    return TypeRef(); // an empty name: an error already reported
  }
  if (parser_.tok_.is(Tok::LParen) && functionTypeAhead()) {
    return parseFunctionType();
  }
  // `infer U` in the pattern of a conditional type, `infer N extends int`
  // with a constraint. As in TypeScript, `extends` after it starts a
  // conditional type if `?` follows, unless conditional types cannot be here.
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "infer" &&
      parser_.cursor().peek().is(Tok::Identifier)) {
    ref.kind = TypeRef::Kind::Infer;
    parser_.cursor().advance(); // 'infer'
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
  // Template literal types: `` `on${K}` ``, and `` `text` ``, a string
  // literal type.
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
  // Literal types: `"circle"`, `0`, `-1.5`, `true`.
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
    Expr *lit = parser_.tok_.is(Tok::IntLiteral) ? parser_.exprs().parseIntLiteral()
                                                 : parser_.exprs().parseFloatLiteral();
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
  // `typeof defaults`, `typeof config.server`: the type of a value.
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
  // `NodeJS.Timeout`, as the types of Node name the objects of timers.
  if (ref.name == "NodeJS" && parser_.tok_.is(Tok::Dot) &&
      parser_.cursor().peek().is(Tok::Identifier)) {
    parser_.cursor().advance();
    ref.name = parser_.ctx_.keepName(("NodeJS." + parser_.tok_.text).str());
    parser_.cursor().advance();
  }
  // `Intl.NumberFormat`: a type of the namespace Intl, by its dotted name.
  if (ref.name == "Intl" && parser_.tok_.is(Tok::Dot) &&
      parser_.cursor().peek().is(Tok::Identifier)) {
    parser_.cursor().advance();
    ref.name = parser_.ctx_.keepName("Intl." + parser_.tok_.text.str());
    parser_.cursor().advance();
  }
  if (parser_.tok_.is(Tok::Less)) {
    ref.kind = TypeRef::Kind::Generic;
    ref.args = parseTypeArgs();
  }

  return ref;
}

bool TypeParser::mappedTypeAhead() {
  if (!parser_.tok_.is(Tok::LBracket) || !parser_.cursor().peek().is(Tok::Identifier)) {
    return false;
  }
  Parser::Snapshot start = parser_.cursor().snapshot();
  parser_.cursor().advance(); // '['
  parser_.cursor().advance(); // the name
  bool in = parser_.tok_.is(Tok::KwIn);
  parser_.cursor().rewind(start);

  return in;
}

// `{ [K in Keys]: T }`, with `readonly`, `-readonly`, `?` and `-?`; the only
// member of its object type.
TypeRef TypeParser::parseMappedType(TypeRef &object, int readonlyChange) {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Mapped;
  ref.loc = object.loc;
  ref.mappedReadonly = readonlyChange;
  parser_.cursor().advance(); // '['
  ref.name = parser_.tok_.text;
  parser_.cursor().advance(); // the name of the key
  parser_.cursor().advance(); // 'in'
  ref.args.push_back(parseType());
  // `[K in keyof T as `get${Capitalize<K>}`]`: the name each key gives. The
  // `as` here is a part of the type, not the operator, which tz has not.
  std::optional<TypeRef> rename;
  if (parser_.cursor().accept(Tok::KwAs)) {
    rename = parseType();
  }
  if (!parser_.cursor().expect(Tok::RBracket, "after the keys of the mapped type")) {
    parser_.cursor().skipToClosingBrace();
    return ref;
  }
  if ((parser_.tok_.is(Tok::Minus) || parser_.tok_.is(Tok::Plus)) &&
      parser_.cursor().peek().is(Tok::Question)) {
    ref.mappedOptional = parser_.tok_.is(Tok::Minus) ? -1 : 1;
    parser_.cursor().advance();
    parser_.cursor().advance();
  } else if (parser_.cursor().accept(Tok::Question)) {
    ref.mappedOptional = 1;
  }
  if (!parser_.cursor().expect(Tok::Colon, "before the type of the values of the mapped type")) {
    parser_.cursor().skipToClosingBrace();
    return ref;
  }
  ref.args.push_back(parseType());
  if (rename) {
    ref.args.push_back(std::move(*rename));
  }
  if (!parser_.cursor().accept(Tok::Semi)) {
    parser_.cursor().accept(Tok::Comma);
  }
  if (!parser_.tok_.is(Tok::RBrace) || !object.args.empty()) {
    parser_.cursor().error(
        parser_.tok_.loc(),
        "a mapped type cannot stand next to fields; make them an object type of their "
        "own");
    parser_.cursor().skipToClosingBrace();
    return ref;
  }
  parser_.cursor().advance(); // '}'

  return ref;
}

bool TypeParser::parseFieldName(llvm::StringRef &name) {
  // `[Symbol.iterator]`, the one name of a member in brackets
  // (docs/iterators.md, section 5).
  if (parser_.classes().symbolKeyAhead()) {
    llvm::SMLoc loc = parser_.tok_.loc();
    parser_.cursor().advance(); // '['
    parser_.cursor().advance(); // 'Symbol'
    if (!parser_.cursor().expect(Tok::Dot, "after 'Symbol'")) {
      return false;
    }
    llvm::StringRef which = parser_.tok_.text;
    parser_.cursor().advance();
    if (!parser_.cursor().expect(Tok::RBracket, "to close the name")) {
      return false;
    }
    if (which == "asyncIterator") {
      // What `for await` iterates (docs/iterators.md, step 4).
      name = "[Symbol.asyncIterator]";
      return true;
    }
    if (which != "iterator") {
      parser_.cursor().report(
          loc, "'Symbol." + which +
                   "' cannot name a member: of the symbols of JavaScript, only "
                   "'Symbol.iterator' and 'Symbol.asyncIterator' can (docs/iterators.md, section "
                   "5)");
    }
    name = "[Symbol.iterator]";
    return true;
  }
  if (parser_.tok_.is(Tok::Identifier) || isKeyword(parser_.tok_.kind)) {
    name = parser_.tok_.text;
    parser_.fieldNames_.insert(name);
    parser_.cursor().advance();
    return true;
  }
  if (parser_.tok_.is(Tok::StringLiteral)) {
    // `"content-type": ...` names the field by the text of the string.
    std::u16string text;
    decodeStringLiteral(parser_.tok_, text, parser_.diags_);
    name = parser_.ctx_.keepName(toUtf8(text));
    parser_.fieldNames_.insert(name);
    parser_.cursor().advance();
    return true;
  }
  if (parser_.tok_.is(Tok::LBracket)) {
    parser_.cursor().error(parser_.tok_.loc(),
                           "a field name in brackets is not supported yet; write the name itself");
  } else if (parser_.tok_.is(Tok::IntLiteral) || parser_.tok_.is(Tok::FloatLiteral)) {
    parser_.cursor().error(
        parser_.tok_.loc(),
        "a number cannot name a field; an object with numbered entries is an array");
  } else {
    parser_.cursor().error(parser_.tok_.loc(), llvm::Twine("expected a field name, found ") +
                                                   tokSpelling(parser_.tok_.kind));
  }

  return false;
}

// `{ x: int; readonly y?: string }`. Fields are separated by `;`, `,` or line
// breaks.
TypeRef TypeParser::parseObjectType() {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Object;
  ref.loc = parser_.tok_.loc();
  parser_.cursor().advance(); // '{'
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    llvm::SMLoc loc = parser_.tok_.loc();
    bool readonly = false;
    // `-readonly` and `+readonly` before a mapped type.
    int readonlyChange = 0;
    if ((parser_.tok_.is(Tok::Minus) || parser_.tok_.is(Tok::Plus)) &&
        parser_.cursor().peek().is(Tok::Identifier) && parser_.cursor().peek().text == "readonly") {
      readonlyChange = parser_.tok_.is(Tok::Minus) ? -1 : 1;
      parser_.cursor().advance();
    }
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "readonly" &&
        !parser_.cursor().peek().is(Tok::Colon) && !parser_.cursor().peek().is(Tok::Question) &&
        !parser_.cursor().peek().is(Tok::LParen)) {
      readonly = true;
      readonlyChange = readonlyChange ? readonlyChange : 1;
      parser_.cursor().advance();
      loc = parser_.tok_.loc();
    }
    if (parser_.tok_.is(Tok::LBracket) && mappedTypeAhead()) {
      return parseMappedType(ref, readonlyChange);
    }
    if (parser_.tok_.is(Tok::LBracket) && !parser_.classes().symbolKeyAhead()) {
      // `{ [key: string]: T }` is a dictionary, `Record<string, T>`
      // (docs/objects.md, section 9): an object has a fixed set of fields.
      parser_.cursor().advance(); // '['
      if (!parser_.tok_.is(Tok::Identifier) || !parser_.cursor().peek().is(Tok::Colon)) {
        parser_.cursor().error(
            loc, "expected an index signature, '{ [key: string]: T }', or a mapped type, "
                 "'{ [K in Keys]: T }'");
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      parser_.cursor().advance(); // the name of the key
      parser_.cursor().advance(); // ':'
      TypeRef key = parseType();
      if (!parser_.cursor().expect(Tok::RBracket, "after the type of the keys") ||
          !parser_.cursor().expect(Tok::Colon, "before the type of the values")) {
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      TypeRef value = parseType();
      if (!parser_.cursor().accept(Tok::Semi)) {
        parser_.cursor().accept(Tok::Comma);
      }
      if (!ref.args.empty() || !parser_.tok_.is(Tok::RBrace)) {
        parser_.cursor().error(
            loc, "an index signature cannot stand next to fields: an object has a fixed set of "
                 "fields, and a dictionary only keys; make the fields an object of their own, "
                 "or a dictionary 'Record<string, T>' alone");
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      parser_.cursor().advance(); // '}'
      TypeRef record;
      record.kind = TypeRef::Kind::Generic;
      record.name = "Record";
      record.loc = ref.loc;
      record.args.push_back(std::move(key));
      record.args.push_back(std::move(value));
      return record;
    }
    if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less) || parser_.tok_.is(Tok::KwNew)) {
      parser_.cursor().error(
          loc,
          "call signatures are not supported; a field can hold a function: 'f: (x: int) => int'");
      parser_.cursor().skipToClosingBrace();
      return ref;
    }
    llvm::StringRef name;
    if (!parseFieldName(name)) {
      parser_.cursor().skipToClosingBrace();
      return ref;
    }
    bool optional = parser_.cursor().accept(Tok::Question);
    if (parser_.tok_.is(Tok::Less)) {
      parser_.cursor().error(parser_.tok_.loc(), "generic methods in types are not supported yet");
      parser_.cursor().skipToClosingBrace();
      return ref;
    }
    if (parser_.tok_.is(Tok::LParen)) {
      // `area(): double`: a read-only field that holds a function.
      TypeRef fn;
      if (!parseSignatureParams(fn) || parser_.recovering_) {
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      if (!parser_.cursor().accept(Tok::Colon)) {
        parser_.cursor().error(parser_.tok_.loc(), "method '" + name +
                                                       "' needs its return type, e.g. '" + name +
                                                       "(): void'");
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      fn.args.push_back(parseType());
      ref.args.push_back(std::move(fn));
      ref.names.push_back(name);
      ref.locs.push_back(loc);
      ref.optional.push_back(optional);
      ref.readonly.push_back(true);
      ref.methods.resize(ref.args.size() - 1);
      ref.methods.push_back(true);
      if (parser_.cursor().accept(Tok::Semi) || parser_.cursor().accept(Tok::Comma)) {
        continue;
      }
      if (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.newlineBefore) {
        parser_.cursor().error(parser_.tok_.loc(),
                               llvm::Twine("expected ';' after the method, found ") +
                                   tokSpelling(parser_.tok_.kind));
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      continue;
    }
    if (!parser_.tok_.is(Tok::Colon)) {
      parser_.cursor().error(parser_.tok_.loc(),
                             "field '" + name + "' needs a type, e.g. '" + name + ": int'");
      parser_.cursor().skipToClosingBrace();
      return ref;
    }
    parser_.cursor().advance();
    ref.args.push_back(parseType());
    ref.names.push_back(name);
    ref.locs.push_back(loc);
    ref.optional.push_back(optional);
    ref.readonly.push_back(readonly);
    if (parser_.cursor().accept(Tok::Semi) || parser_.cursor().accept(Tok::Comma)) {
      continue;
    }
    if (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.newlineBefore) {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected ';' after the field, found ") +
                                 tokSpelling(parser_.tok_.kind));
      parser_.cursor().skipToClosingBrace();
      return ref;
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the object type");

  return ref;
}

// `[string, int]`, and `[]`, the arguments of an event without any
// (docs/events.md). Labels, as in `[name: string, age: int]`, are allowed
// and change nothing.
TypeRef TypeParser::parseTupleType() {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Tuple;
  ref.loc = parser_.tok_.loc();
  parser_.cursor().advance(); // '['
  while (!parser_.tok_.is(Tok::RBracket) && !parser_.tok_.is(Tok::Eof)) {
    // `...A`: the types a list of them stands for, which Sema checks.
    bool spread = parser_.cursor().accept(Tok::Ellipsis);
    if (parser_.tok_.is(Tok::Identifier) && parser_.cursor().peek().is(Tok::Colon)) {
      parser_.cursor().advance(); // the label
      parser_.cursor().advance(); // ':'
    }
    ref.args.push_back(parseType());
    ref.readonly.push_back(false);
    ref.spread.push_back(spread);
    if (parser_.tok_.is(Tok::Question)) {
      parser_.cursor().report(
          parser_.tok_.loc(),
          "optional elements in a tuple type are not supported: a tuple has a fixed "
          "length; give the element the type 'T | undefined'");
      parser_.cursor().advance();
    }
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBracket, "to close the tuple type");

  return ref;
}

// At `(`: a function type if a parameter list (names, optionally with `?`,
// `...` and types) and `=>` follow; otherwise a parenthesized type.
bool TypeParser::functionTypeAhead() {
  Parser::Snapshot start = parser_.cursor().snapshot();
  bool muted = parser_.diags_.setMuted(true);
  parser_.recovering_ = false;
  parser_.cursor().advance(); // '('
  bool ok = true;
  while (ok && !parser_.tok_.is(Tok::RParen)) {
    parser_.cursor().accept(Tok::Ellipsis);
    ok = parser_.cursor().accept(Tok::Identifier);
    parser_.cursor().accept(Tok::Question);
    if (ok && parser_.cursor().accept(Tok::Colon)) {
      parseType();
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

TypeRef TypeParser::parseFunctionType() {
  TypeRef ref;
  if (!parseSignatureParams(ref)) {
    return ref;
  }
  parser_.cursor().expect(Tok::Arrow, "after the parameters of the function type");
  ref.args.push_back(parseReturnType());

  return ref;
}

bool TypeParser::parseSignatureParams(TypeRef &ref) {
  ref.kind = TypeRef::Kind::Function;
  ref.loc = parser_.tok_.loc();
  parser_.cursor().advance(); // '('
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
      param = parseType();
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

// `<T, U>`. A closing `>>` (as in `Array<Array<int>>`) is split in two.
std::vector<TypeRef> TypeParser::parseTypeArgs() {
  std::vector<TypeRef> args;
  parser_.cursor().advance(); // '<'
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
    // Keep the rest of the token: `>>` leaves `>`, `>=` leaves `=`.
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
  // As in TypeScript: a call, or a token that ends the expression (an
  // instantiation `first<int>` used as a value). After anything else, as in
  // `a < b > c`, the `<` compares.
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

} // namespace tz
