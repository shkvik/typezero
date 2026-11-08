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
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
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

ObjectTypeParser::ObjectTypeParser(Parser &parser)
    : parser(parser) {}

bool ObjectTypeParser::mappedTypeAhead() {
  if (!this->parser.tok.is(Tok::LBracket) || !this->parser.cursor().peek().is(Tok::Identifier)) {
    return false;
  }
  Parser::Snapshot start = this->parser.cursor().snapshot();
  this->parser.cursor().advance();
  this->parser.cursor().advance();
  bool in = this->parser.tok.is(Tok::KwIn);
  this->parser.cursor().rewind(start);

  return in;
}

TypeRef ObjectTypeParser::parseMappedType(TypeRef &object, int readonlyChange) {
  TypeRef ref;
  ref.form = MappedTypeRef{.mappedReadonly = readonlyChange};
  MappedTypeRef *mapped = ref.form.as<MappedTypeRef>();
  ref.loc = object.loc;
  this->parser.cursor().advance();
  ref.name = this->parser.tok.text;
  this->parser.cursor().advance();
  this->parser.cursor().advance();
  ref.args.push_back(this->parser.types().parseType());
  std::optional<TypeRef> rename;
  if (this->parser.cursor().accept(Tok::KwAs)) {
    rename = this->parser.types().parseType();
  }
  if (!this->parser.cursor().expect(Tok::RBracket, "after the keys of the mapped type")) {
    this->parser.cursor().skipToClosingBrace();
    return ref;
  }
  if ((this->parser.tok.is(Tok::Minus) || this->parser.tok.is(Tok::Plus)) &&
      this->parser.cursor().peek().is(Tok::Question)) {
    mapped->mappedOptional = this->parser.tok.is(Tok::Minus) ? -1 : 1;
    this->parser.cursor().advance();
    this->parser.cursor().advance();
  } else if (this->parser.cursor().accept(Tok::Question)) {
    mapped->mappedOptional = 1;
  }
  if (!this->parser.cursor().expect(Tok::Colon,
                                    "before the type of the values of the mapped type")) {
    this->parser.cursor().skipToClosingBrace();
    return ref;
  }
  ref.args.push_back(this->parser.types().parseType());
  if (rename) {
    ref.args.push_back(std::move(*rename));
  }
  if (!this->parser.cursor().accept(Tok::Semi)) {
    this->parser.cursor().accept(Tok::Comma);
  }
  if (!this->parser.tok.is(Tok::RBrace) || !object.args.empty()) {
    this->parser.cursor().error(
        this->parser.tok.loc(),
        "a mapped type cannot stand next to fields; make them an object type of their "
        "own");
    this->parser.cursor().skipToClosingBrace();
    return ref;
  }
  this->parser.cursor().advance();

  return ref;
}

bool ObjectTypeParser::parseFieldName(llvm::StringRef &name) {
  if (this->parser.classes().symbolKeyAhead()) {
    llvm::SMLoc loc = this->parser.tok.loc();
    this->parser.cursor().advance();
    this->parser.cursor().advance();
    if (!this->parser.cursor().expect(Tok::Dot, "after 'Symbol'")) {
      return false;
    }
    llvm::StringRef which = this->parser.tok.text;
    this->parser.cursor().advance();
    if (!this->parser.cursor().expect(Tok::RBracket, "to close the name")) {
      return false;
    }
    if (which == "asyncIterator") {
      name = "[Symbol.asyncIterator]";
      return true;
    }
    if (which != "iterator") {
      this->parser.cursor().report(
          loc, "'Symbol." + which +
                   "' cannot name a member: of the symbols of JavaScript, only "
                   "'Symbol.iterator' and 'Symbol.asyncIterator' can (docs/iterators.md, section "
                   "5)");
    }
    name = "[Symbol.iterator]";
    return true;
  }
  if (this->parser.tok.is(Tok::Identifier) || TokenKinds::isKeyword(this->parser.tok.kind)) {
    name = this->parser.tok.text;
    this->parser.fieldNames.insert(name);
    this->parser.cursor().advance();
    return true;
  }
  if (this->parser.tok.is(Tok::StringLiteral)) {
    std::u16string text;
    LiteralText::decodeStringLiteral(this->parser.tok, text, this->parser.diags);
    name = this->parser.ctx.keepName(ToUtf8::toUtf8(text));
    this->parser.fieldNames.insert(name);
    this->parser.cursor().advance();
    return true;
  }
  if (this->parser.tok.is(Tok::LBracket)) {
    this->parser.cursor().error(
        this->parser.tok.loc(),
        "a field name in brackets is not supported yet; write the name itself");
  } else if (this->parser.tok.is(Tok::IntLiteral) || this->parser.tok.is(Tok::FloatLiteral)) {
    this->parser.cursor().error(
        this->parser.tok.loc(),
        "a number cannot name a field; an object with numbered entries is an array");
  } else {
    this->parser.cursor().error(this->parser.tok.loc(),
                                llvm::Twine("expected a field name, found ") +
                                    TokenKinds::tokSpelling(this->parser.tok.kind));
  }

  return false;
}

TypeRef ObjectTypeParser::parseObjectType() {
  TypeRef ref;
  ref.form = ObjectTypeRef{};
  ObjectTypeRef *object = ref.form.as<ObjectTypeRef>();
  ref.loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    llvm::SMLoc loc = this->parser.tok.loc();
    bool readonly = false;
    int readonlyChange = 0;
    if ((this->parser.tok.is(Tok::Minus) || this->parser.tok.is(Tok::Plus)) &&
        this->parser.cursor().peek().is(Tok::Identifier) &&
        this->parser.cursor().peek().text == "readonly") {
      readonlyChange = this->parser.tok.is(Tok::Minus) ? -1 : 1;
      this->parser.cursor().advance();
    }
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "readonly" &&
        !this->parser.cursor().peek().is(Tok::Colon) &&
        !this->parser.cursor().peek().is(Tok::Question) &&
        !this->parser.cursor().peek().is(Tok::LParen)) {
      readonly = true;
      readonlyChange = readonlyChange ? readonlyChange : 1;
      this->parser.cursor().advance();
      loc = this->parser.tok.loc();
    }
    if (this->parser.tok.is(Tok::LBracket) && mappedTypeAhead()) {
      return parseMappedType(ref, readonlyChange);
    }
    if (this->parser.tok.is(Tok::LBracket) && !this->parser.classes().symbolKeyAhead()) {
      this->parser.cursor().advance();
      if (!this->parser.tok.is(Tok::Identifier) || !this->parser.cursor().peek().is(Tok::Colon)) {
        this->parser.cursor().error(
            loc, "expected an index signature, '{ [key: string]: T }', or a mapped type, "
                 "'{ [K in Keys]: T }'");
        this->parser.cursor().skipToClosingBrace();
        return ref;
      }
      this->parser.cursor().advance();
      this->parser.cursor().advance();
      TypeRef key = this->parser.types().parseType();
      if (!this->parser.cursor().expect(Tok::RBracket, "after the type of the keys") ||
          !this->parser.cursor().expect(Tok::Colon, "before the type of the values")) {
        this->parser.cursor().skipToClosingBrace();
        return ref;
      }
      TypeRef value = this->parser.types().parseType();
      if (!this->parser.cursor().accept(Tok::Semi)) {
        this->parser.cursor().accept(Tok::Comma);
      }
      if (!ref.args.empty() || !this->parser.tok.is(Tok::RBrace)) {
        this->parser.cursor().error(
            loc, "an index signature cannot stand next to fields: an object has a fixed set of "
                 "fields, and a dictionary only keys; make the fields an object of their own, "
                 "or a dictionary 'Record<string, T>' alone");
        this->parser.cursor().skipToClosingBrace();
        return ref;
      }
      this->parser.cursor().advance();
      TypeRef record;
      record.form = GenericTypeRef{};
      record.name = "Record";
      record.loc = ref.loc;
      record.args.push_back(std::move(key));
      record.args.push_back(std::move(value));
      return record;
    }
    if (this->parser.tok.is(Tok::LParen) || this->parser.tok.is(Tok::Less) ||
        this->parser.tok.is(Tok::KwNew)) {
      this->parser.cursor().error(
          loc,
          "call signatures are not supported; a field can hold a function: 'f: (x: int) => int'");
      this->parser.cursor().skipToClosingBrace();
      return ref;
    }
    llvm::StringRef name;
    if (!parseFieldName(name)) {
      this->parser.cursor().skipToClosingBrace();
      return ref;
    }
    bool optional = this->parser.cursor().accept(Tok::Question);
    if (this->parser.tok.is(Tok::Less)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  "generic methods in types are not supported yet");
      this->parser.cursor().skipToClosingBrace();
      return ref;
    }
    if (this->parser.tok.is(Tok::LParen)) {
      TypeRef fn;
      if (!this->parser.functionTypes().parseSignatureParams(fn) || this->parser.recovering) {
        this->parser.cursor().skipToClosingBrace();
        return ref;
      }
      if (!this->parser.cursor().accept(Tok::Colon)) {
        this->parser.cursor().error(this->parser.tok.loc(), "method '" + name +
                                                                "' needs its return type, e.g. '" +
                                                                name + "(): void'");
        this->parser.cursor().skipToClosingBrace();
        return ref;
      }
      fn.args.push_back(this->parser.types().parseType());
      ref.args.push_back(std::move(fn));
      object->names.push_back(name);
      object->locs.push_back(loc);
      object->optional.push_back(optional);
      object->readonly.push_back(true);
      object->methods.resize(ref.args.size() - 1);
      object->methods.push_back(true);
      if (this->parser.cursor().accept(Tok::Semi) || this->parser.cursor().accept(Tok::Comma)) {
        continue;
      }
      if (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.newlineBefore) {
        this->parser.cursor().error(this->parser.tok.loc(),
                                    llvm::Twine("expected ';' after the method, found ") +
                                        TokenKinds::tokSpelling(this->parser.tok.kind));
        this->parser.cursor().skipToClosingBrace();
        return ref;
      }
      continue;
    }
    if (!this->parser.tok.is(Tok::Colon)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  "field '" + name + "' needs a type, e.g. '" + name + ": int'");
      this->parser.cursor().skipToClosingBrace();
      return ref;
    }
    this->parser.cursor().advance();
    ref.args.push_back(this->parser.types().parseType());
    object->names.push_back(name);
    object->locs.push_back(loc);
    object->optional.push_back(optional);
    object->readonly.push_back(readonly);
    if (this->parser.cursor().accept(Tok::Semi) || this->parser.cursor().accept(Tok::Comma)) {
      continue;
    }
    if (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.newlineBefore) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected ';' after the field, found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      this->parser.cursor().skipToClosingBrace();
      return ref;
    }
  }
  this->parser.cursor().expect(Tok::RBrace, "to close the object type");

  return ref;
}

TypeRef ObjectTypeParser::parseTupleType() {
  TypeRef ref;
  ref.form = TupleTypeRef{};
  TupleTypeRef *tuple = ref.form.as<TupleTypeRef>();
  ref.loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::RBracket) && !this->parser.tok.is(Tok::Eof)) {
    bool spread = this->parser.cursor().accept(Tok::Ellipsis);
    if (this->parser.tok.is(Tok::Identifier) && this->parser.cursor().peek().is(Tok::Colon)) {
      this->parser.cursor().advance();
      this->parser.cursor().advance();
    }
    ref.args.push_back(this->parser.types().parseType());
    tuple->readonly.push_back(false);
    tuple->spread.push_back(spread);
    if (this->parser.tok.is(Tok::Question)) {
      this->parser.cursor().report(
          this->parser.tok.loc(),
          "optional elements in a tuple type are not supported: a tuple has a fixed "
          "length; give the element the type 'T | undefined'");
      this->parser.cursor().advance();
    }
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RBracket, "to close the tuple type");

  return ref;
}

}
