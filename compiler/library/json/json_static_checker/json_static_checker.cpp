#include "compiler/library/json/json_static_checker/json_static_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/json/json_type_checker/json_type_checker.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

struct JsonProblem {
  std::string message;
  size_t offset = 0;
};

bool mayHoldItself(const Type *type, std::vector<const Type *> &open) {
  type = type->nonOptional();
  if (type->isParam() || type->isAnyObject()) {
    return false;
  }
  if (type->isJson() || (type->isBase() && !type->classDecl()) || type->isUnknown() ||
      llvm::is_contained(open, type)) {
    return true;
  }
  open.push_back(type);
  bool may = false;
  auto fieldsHold = [&](const Type *t) {
    return llvm::any_of(t->fields(), [&](const Field &f) { return mayHoldItself(f.type, open); });
  };
  if (const ClassDecl *cls = type->classDecl()) {
    std::vector<const ClassDecl *> family{cls};
    for (size_t i = 0; i < family.size() && !may; ++i) {
      if (family[i]->type) {
        may = fieldsHold(family[i]->type);
      }
      family.insert(family.end(), family[i]->children.begin(), family[i]->children.end());
    }
  } else if (type->isArray()) {
    may = mayHoldItself(type->inner(), open);
  } else if (type->isResult()) {
    may = mayHoldItself(type->okType(), open) || mayHoldItself(type->errType(), open);
  } else if (type->isDict()) {
    may = mayHoldItself(type->valueType(), open);
  } else if (type->isObject()) {
    may = fieldsHold(type);
  } else if (type->isUnion()) {
    may = llvm::any_of(type->members(), [&](const Type *m) { return mayHoldItself(m, open); });
  }
  open.pop_back();

  return may;
}

JsonProblem checkJsonText(llvm::StringRef s) {
  size_t i = 0, n = s.size();
  auto whitespace = [&] {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      ++i;
    }
  };
  auto digits = [&] {
    size_t start = i;
    while (i < n && llvm::isDigit(s[i])) {
      ++i;
    }

    return i > start;
  };
  auto problem = [&](std::string message) { return JsonProblem{std::move(message), i}; };
  auto string = [&]() -> std::string {
    ++i;
    for (;;) {
      if (i >= n) {
        return "the string is not closed";
      }
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '"') {
        ++i;
        return "";
      }
      if (c < 0x20) {
        return "a control character in a string must be escaped, e.g. '\\n' for a line break";
      }
      if (c != '\\') {
        ++i;
        continue;
      }
      if (++i >= n) {
        return "the string is not closed";
      }
      char e = s[i];
      if (e == 'u') {
        for (int k = 0; k < 4; ++k) {
          if (++i >= n || !llvm::isHexDigit(s[i])) {
            return "'\\u' needs four hexadecimal digits";
          }
        }
      } else if (!llvm::StringRef("\"\\/bfnrt").contains(e)) {
        return "'\\" + std::string(1, e) + "' is not an escape of JSON";
      }
      ++i;
    }
  };
  auto key = [&]() -> std::string {
    whitespace();
    if (i >= n || s[i] != '"') {
      return "expected a property name in double quotes";
    }
    if (std::string m = string(); !m.empty()) {
      return m;
    }
    whitespace();
    if (i >= n || s[i] != ':') {
      return "expected ':' after a property name";
    }
    ++i;

    return "";
  };

  std::vector<char> open;
  for (;;) {
    whitespace();
    if (i >= n) {
      return problem("the text ends where a value should be");
    }
    char c = s[i];
    if (c == '"') {
      size_t start = i;
      if (std::string m = string(); !m.empty()) {
        JsonProblem p = problem(m);
        if (m == "the string is not closed") {
          p.offset = start;
        }
        return p;
      }
    } else if (c == '-' || llvm::isDigit(c)) {
      if (c == '-') {
        ++i;
      }
      if (i < n && s[i] == '0') {
        ++i;
        if (i < n && llvm::isDigit(s[i])) {
          return problem("a number cannot start with 0 followed by more digits");
        }
      } else if (!digits()) {
        return problem("expected digits after '-'");
      }
      if (i < n && s[i] == '.') {
        ++i;
        if (!digits()) {
          return problem("expected digits after the decimal point");
        }
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
          ++i;
        }
        if (!digits()) {
          return problem("expected digits in the exponent");
        }
      }
    } else if (c == '{' || c == '[') {
      ++i;
      whitespace();
      if (i < n && s[i] == (c == '{' ? '}' : ']')) {
        ++i;
      } else {
        open.push_back(c);
        if (c == '{') {
          if (std::string m = key(); !m.empty()) {
            return problem(m);
          }
        }
        continue;
      }
    } else if (s.substr(i).starts_with("true") || s.substr(i).starts_with("null")) {
      i += 4;
    } else if (s.substr(i).starts_with("false")) {
      i += 5;
    } else {
      unsigned char u = static_cast<unsigned char>(c);
      return problem(u >= 0x21 && u < 0x7F ? "expected a value, found '" + std::string(1, c) + "'"
                                           : std::string("expected a value"));
    }
    for (;;) {
      whitespace();
      if (open.empty()) {
        if (i < n) {
          return problem("unexpected text after the value");
        }
        return {};
      }
      char container = open.back();
      if (i < n && s[i] == ',') {
        ++i;
        if (container == '{') {
          if (std::string m = key(); !m.empty()) {
            return problem(m);
          }
        }
        break;
      }
      if (i < n && s[i] == (container == '{' ? '}' : ']')) {
        ++i;
        open.pop_back();
        continue;
      }
      return problem(container == '{' ? "expected ',' or '}' after a property value"
                                      : "expected ',' or ']' after an array element");
    }
  }
}

const ClassDecl *classWithToJson(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }
  if (type->isResult()) {
    if (const ClassDecl *cls = classWithToJson(type->okType(), seen)) {
      return cls;
    }
    return classWithToJson(type->errType(), seen);
  }
  if (type->isRecord()) {
    return classWithToJson(type->valueType(), seen);
  }
  if (!type->isObject() || !seen.insert(type).second) {
    return nullptr;
  }
  if (const ClassDecl *cls = type->classDecl()) {
    for (const FunctionDecl *fn : cls->methods) {
      if (fn->name == "toJSON" && !fn->isStatic) {
        return cls;
      }
    }
  }
  for (const Field &f : type->fields()) {
    if (const ClassDecl *cls = classWithToJson(f.type, seen)) {
      return cls;
    }
  }

  return nullptr;
}

bool mayStringifyToUndefined(const Type *type) {
  return type->isUndefined() || type->isOptional() || type->isFunction() || type->isNever() ||
         type->isUnknown();
}

}

JsonStaticChecker::JsonStaticChecker(Checker &checker) : checker_(checker) {}

Expr *JsonStaticChecker::checkJsonStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      if (!JsonChecker::isNullName(arg)) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
    }
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  llvm::StringRef fn = member->name;
  if (fn == "parse") {
    if (call->args.size() == 2) {
      checker_.diags_.error(call->args[1]->loc(),
                            "the reviver of JSON.parse is not supported; convert the "
                            "Json it gives, or give the type: 'JSON.parse<T>(text)'");
      checkArgsLoosely();
      return call;
    }
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    call->args[0] = checker_.argumentChecker().checkArg(call->args[0], Type::string());
    if (member->typeArgs.empty()) {
      call->builtin = Builtin::JsonParse;
      call->type = Type::result(Type::json(), checker_.prelude().builtinType("JsonError"));
      return call;
    }
    if (member->typeArgs.size() != 1) {
      checker_.diags_.error(member->loc(), "JSON.parse takes one type argument, the type to build: "
                                           "'JSON.parse<User>(text)'");
      return call;
    }
    const Type *target = checker_.typeResolver().resolveType(member->typeArgs[0], false);
    if (!checker_.jsonTypeChecker().checkFromJsonType(target, member->typeArgs[0].loc,
                                                      "JSON.parse")) {
      return call;
    }
    call->builtin = Builtin::JsonParseAs;
    call->type = Type::result(target, checker_.prelude().builtinType("JsonError"));
    return call;
  }
  if (!member->typeArgs.empty()) {
    checker_.diags_.error(member->loc(), "only JSON.parse takes a type argument");
  }
  if (fn != "stringify") {
    checker_.diags_.error(member->loc(),
                          "'JSON." + fn + "' does not exist; JSON has parse and stringify");
    checkArgsLoosely();
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), 1, 3)) {
    return call;
  }
  Expr *value = call->args[0] = checker_.exprChecker().checkExpr(call->args[0]);
  bool ok = !value->type->isError();
  if (value->type->isVoid()) {
    checker_.diags_.error(value->loc(),
                          "an expression of type 'void' has no value to write as JSON");
    ok = false;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  if (const ClassDecl *cls = ok ? classWithToJson(value->type, seen) : nullptr) {
    checker_.diags_.error(value->loc(),
                          "JSON.stringify does not call the method toJSON of class '" + cls->name +
                              "': tz calls no code by itself; call it, e.g. "
                              "'JSON.stringify(x.toJSON())'");
    ok = false;
  }
  std::vector<Expr *> args{value};
  if (call->args.size() >= 2) {
    Expr *replacer = call->args[1];
    if (!JsonChecker::isNullName(replacer) && !isa<UndefinedExpr>(replacer)) {
      replacer = checker_.exprChecker().checkExpr(replacer);
      if (!replacer->type->isError()) {
        checker_.diags_.error(replacer->loc(),
                              "a replacer is not supported: tz calls no code by itself; "
                              "build the value to write first, and pass 'null' here");
      }
      ok = false;
    }
  }
  if (call->args.size() == 3) {
    Expr *space = call->args[2];
    if (!JsonChecker::isNullName(space) && !isa<UndefinedExpr>(space)) {
      space = checker_.exprChecker().checkExpr(space);
      const Type *type = space->type;
      if (type->isNumeric()) {
        args.push_back(checker_.typeRelations().convert(space, Type::doubleTy()));
      } else if (type->isString()) {
        args.push_back(space);
      } else if (!type->isError()) {
        checker_.diags_.error(space->loc(),
                              "the space of JSON.stringify is a number of spaces or a "
                              "string, found '" +
                                  type->name() + "'");
        ok = false;
      }
    }
  }
  if (!ok) {
    return call;
  }
  call->args = std::move(args);
  call->builtin = Builtin::JsonStringify;
  call->type =
      mayStringifyToUndefined(value->type) ? Type::optional(Type::string()) : Type::string();
  std::vector<const Type *> open;
  if (!checker_.libraryCalls().inLibrary(call->loc()) && mayHoldItself(value->type, open)) {
    call->type = Type::result(call->type, checker_.prelude().builtinType("JsonError"));
  }

  return call;
}

bool JsonStaticChecker::readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes,
                                     std::string &shown, std::string &full) {
  Expr *arg = call->args[0];
  std::string example =
      (fn + (fn == "embedText" ? "(\"./page.html\")" : "(\"./data.json\")")).str();
  std::u16string path;
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    path = lit->value;
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(arg); tmpl && tmpl->exprs.empty()) {
    path = tmpl->quasis[0];
  } else {
    call->args[0] = checker_.exprChecker().checkExpr(arg);
    checker_.diags_.error(arg->loc(), "the path of " + fn +
                                          " must be a string literal: the file is read "
                                          "when the program is compiled, e.g. '" +
                                          example + "'");
    return false;
  }
  shown = toUtf8(path);
  if (llvm::StringRef(shown).contains('\\')) {
    checker_.diags_.error(arg->loc(), "write '/' in the path, not '\\', as on every system");
    return false;
  }
  if (shown.empty() || llvm::sys::path::is_absolute(shown) || shown[0] == '/') {
    checker_.diags_.error(arg->loc(), "the path of " + fn +
                                          " is relative to the file it is written in, "
                                          "e.g. '" +
                                          example + "'");
    return false;
  }
  llvm::SourceMgr &sources = checker_.diags_.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(call->loc());
  llvm::StringRef source = id ? sources.getMemoryBuffer(id)->getBufferIdentifier() : "";
  llvm::SmallString<256> path8(llvm::sys::path::parent_path(source));
  llvm::sys::path::append(path8, shown);
  llvm::sys::path::remove_dots(path8, true);
  llvm::sys::path::native(path8);
  full = std::string(path8);
  auto buffer = llvm::MemoryBuffer::getFile(full, false, false);
  if (!buffer) {
    checker_.diags_.error(arg->loc(),
                          "cannot read '" + shown + "': " + buffer.getError().message());
    return false;
  }
  bytes = (*buffer)->getBuffer().str();

  return true;
}

Expr *JsonStaticChecker::checkJsonGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"embedJson", "embedText", "embedBytes", "fromJson"};
  if (!llvm::any_of(
          names, [&](llvm::StringRef n) { return checker_.callChecker().isGlobalName(name, n); })) {
    return nullptr;
  }
  llvm::StringRef fn = name->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }

  if (fn == "fromJson") {
    if (name->typeArgs.empty() && call->args.size() == 2) {
      return checker_.reflectChecker().checkFromJsonClass(call, name);
    }
    if (name->typeArgs.size() != 1) {
      checker_.diags_.error(name->loc(),
                            "fromJson needs the type to build, e.g. 'fromJson<User>(j)', or "
                            "the class to make an object of, 'fromJson(User, j)'");
      checkArgsLoosely();
      return call;
    }
    if (!checker_.argumentChecker().checkArity(call, name->loc(), 1, 1)) {
      return call;
    }
    Expr *arg = checker_.exprChecker().checkExpr(call->args[0], Type::json());
    call->args[0] = checker_.typeRelations().coerce(
        arg, arg->type->isOptional() || arg->type->isUndefined() ? Type::optional(Type::json())
                                                                 : Type::json());
    const Type *target = checker_.typeResolver().resolveType(name->typeArgs[0], false);
    if (!checker_.jsonTypeChecker().checkFromJsonType(target, name->typeArgs[0].loc, "fromJson")) {
      return call;
    }
    call->builtin = Builtin::FromJson;
    call->type = Type::result(target, checker_.prelude().builtinType("JsonError"));
    return call;
  }
  if (!name->typeArgs.empty()) {
    checker_.diags_.error(name->loc(), "'" + fn + "' takes no type arguments");
  }

  if (fn == "embedBytes") {
    return checker_.bufferStaticChecker().checkEmbedBytes(call, name);
  }
  if (!checker_.argumentChecker().checkArity(call, name->loc(), 1, 1)) {
    return call;
  }
  std::string bytes, shown, full;
  if (!readEmbedded(call, fn, bytes, shown, full)) {
    return call;
  }
  llvm::StringRef text(bytes);
  if (fn == "embedJson") {
    text.consume_front("\xEF\xBB\xBF");
  }
  llvm::SmallVector<llvm::UTF16, 64> units;
  if (!llvm::convertUTF8ToUTF16String(text, units)) {
    checker_.diags_.error(call->args[0]->loc(), "'" + shown + "' is not text in UTF-8");
    return call;
  }
  std::u16string content(units.begin(), units.end());

  if (fn == "embedText") {
    auto *lit = checker_.ctx_.make<StringLitExpr>(call->loc(), std::move(content));
    lit->type = Type::string();
    return lit;
  }
  if (JsonProblem problem = checkJsonText(text); !problem.message.empty()) {
    size_t line = 1, column = 1;
    for (size_t i = 0; i < problem.offset && i < text.size(); ++i) {
      if (text[i] == '\n') {
        ++line;
        column = 1;
      } else if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
        ++column;
      }
    }
    checker_.diags_.error(call->args[0]->loc(),
                          "'" + shown + "' is not valid JSON: " + problem.message + " (line " +
                              std::to_string(line) + ", column " + std::to_string(column) + ")");
    llvm::SourceMgr &sources = checker_.diags_.sourceMgr();
    unsigned id =
        sources.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(text, full), llvm::SMLoc());
    const char *start = sources.getMemoryBuffer(id)->getBufferStart();
    checker_.diags_.note(llvm::SMLoc::getFromPointer(start + std::min(problem.offset, text.size())),
                         "the JSON is wrong here");
    return call;
  }
  llvm::SMLoc loc = call->args[0]->loc();
  auto *lit = checker_.ctx_.make<StringLitExpr>(loc, std::move(content));
  lit->type = Type::string();
  llvm::SmallVector<llvm::UTF16, 64> path;
  llvm::convertUTF8ToUTF16String(full, path);
  auto *file = checker_.ctx_.make<StringLitExpr>(loc, std::u16string(path.begin(), path.end()));
  file->type = Type::string();
  call->args = {lit, file};
  call->builtin = Builtin::EmbedJson;
  call->type = Type::json();

  return call;
}

}
