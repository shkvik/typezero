#include "compiler/load/object_type_parser/object_type_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/function_type_parser/function_type_parser.h"
#include "compiler/load/literal_text/literal_text.h"
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

ObjectTypeParser::ObjectTypeParser(Parser &parser) : parser_(parser) {}

bool ObjectTypeParser::mappedTypeAhead() {
  if (!parser_.tok_.is(Tok::LBracket) || !parser_.cursor().peek().is(Tok::Identifier)) {
    return false;
  }
  Parser::Snapshot start = parser_.cursor().snapshot();
  parser_.cursor().advance();
  parser_.cursor().advance();
  bool in = parser_.tok_.is(Tok::KwIn);
  parser_.cursor().rewind(start);

  return in;
}

TypeRef ObjectTypeParser::parseMappedType(TypeRef &object, int readonlyChange) {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Mapped;
  ref.loc = object.loc;
  ref.mappedReadonly = readonlyChange;
  parser_.cursor().advance();
  ref.name = parser_.tok_.text;
  parser_.cursor().advance();
  parser_.cursor().advance();
  ref.args.push_back(parser_.types().parseType());
  std::optional<TypeRef> rename;
  if (parser_.cursor().accept(Tok::KwAs)) {
    rename = parser_.types().parseType();
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
  ref.args.push_back(parser_.types().parseType());
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
  parser_.cursor().advance();

  return ref;
}

bool ObjectTypeParser::parseFieldName(llvm::StringRef &name) {
  if (parser_.classes().symbolKeyAhead()) {
    llvm::SMLoc loc = parser_.tok_.loc();
    parser_.cursor().advance();
    parser_.cursor().advance();
    if (!parser_.cursor().expect(Tok::Dot, "after 'Symbol'")) {
      return false;
    }
    llvm::StringRef which = parser_.tok_.text;
    parser_.cursor().advance();
    if (!parser_.cursor().expect(Tok::RBracket, "to close the name")) {
      return false;
    }
    if (which == "asyncIterator") {
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

TypeRef ObjectTypeParser::parseObjectType() {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Object;
  ref.loc = parser_.tok_.loc();
  parser_.cursor().advance();
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    llvm::SMLoc loc = parser_.tok_.loc();
    bool readonly = false;
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
      parser_.cursor().advance();
      if (!parser_.tok_.is(Tok::Identifier) || !parser_.cursor().peek().is(Tok::Colon)) {
        parser_.cursor().error(
            loc, "expected an index signature, '{ [key: string]: T }', or a mapped type, "
                 "'{ [K in Keys]: T }'");
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      parser_.cursor().advance();
      parser_.cursor().advance();
      TypeRef key = parser_.types().parseType();
      if (!parser_.cursor().expect(Tok::RBracket, "after the type of the keys") ||
          !parser_.cursor().expect(Tok::Colon, "before the type of the values")) {
        parser_.cursor().skipToClosingBrace();
        return ref;
      }
      TypeRef value = parser_.types().parseType();
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
      parser_.cursor().advance();
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
      TypeRef fn;
      if (!parser_.functionTypes().parseSignatureParams(fn) || parser_.recovering_) {
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
      fn.args.push_back(parser_.types().parseType());
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
    ref.args.push_back(parser_.types().parseType());
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

TypeRef ObjectTypeParser::parseTupleType() {
  TypeRef ref;
  ref.kind = TypeRef::Kind::Tuple;
  ref.loc = parser_.tok_.loc();
  parser_.cursor().advance();
  while (!parser_.tok_.is(Tok::RBracket) && !parser_.tok_.is(Tok::Eof)) {
    bool spread = parser_.cursor().accept(Tok::Ellipsis);
    if (parser_.tok_.is(Tok::Identifier) && parser_.cursor().peek().is(Tok::Colon)) {
      parser_.cursor().advance();
      parser_.cursor().advance();
    }
    ref.args.push_back(parser_.types().parseType());
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

}
