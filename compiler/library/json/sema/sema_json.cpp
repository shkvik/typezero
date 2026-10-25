// Json values (docs/modules.md, section 5): the type Json and `null`,
// reading and writing keys, narrowing, equality, literals where a Json is
// expected, JSON.parse and JSON.stringify, embedJson and embedText, and
// JSON.parse<T> and fromJson<T> (docs/narrowing.md, section 3).
//
// A Json is read as JavaScript reads the value JSON.parse gives, which in
// TypeScript is `any`: `j.users[0].name` is a `Json | undefined`, and so is
// any key that is not there. What JavaScript throws, reading a key of
// undefined or null, throws the same TypeError (codegen_json.cpp). A Json is
// narrowed like a union: `typeof name == "string"` makes a Json variable a
// string, `Array.isArray(items)` a Json[], `x != null` (or `!= undefined`)
// takes undefined away.

#include "compiler/check/sema/sema.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::cast;
using llvm::dyn_cast;
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

// The variable a condition tests, looking through the conversion of a
// narrowed variable; null unless it is a Json (or a `Json | undefined`).
const VarSymbol *jsonVariable(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit())
    expr = conv->operand;
  auto *name = dyn_cast<NameExpr>(expr);
  return name && name->var && name->var->type->nonOptional()->isJson() ? name->var : nullptr;
}

// The literal `null`: a name that no declaration has.
bool nullName(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  return name && name->name == "null" && !name->var && !name->func;
}

// `null` or `undefined` (also `void x`), which a Json compares equal to both.
bool isNothing(const Expr *expr) {
  return nullName(expr) || (expr->type && expr->type->isUndefined());
}

// A problem with the text of a JSON file, and where it is (a byte offset).
struct JsonProblem {
  std::string message;
  size_t offset = 0;
};

// Checks the text of a file that embedJson reads, with the grammar of
// JSON.parse (runtime/library/json/json/json.c), so that the program cannot
// fail to parse it.
JsonProblem checkJsonText(llvm::StringRef s) {
  size_t i = 0, n = s.size();
  auto whitespace = [&] {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
      ++i;
  };
  auto digits = [&] {
    size_t start = i;
    while (i < n && llvm::isDigit(s[i]))
      ++i;
    return i > start;
  };
  auto problem = [&](std::string message) { return JsonProblem{std::move(message), i}; };
  // A string from its opening quote; empty on success.
  auto string = [&]() -> std::string {
    ++i;
    for (;;) {
      if (i >= n)
        return "the string is not closed";
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '"') {
        ++i;
        return "";
      }
      if (c < 0x20)
        return "a control character in a string must be escaped, e.g. '\\n' for a line break";
      if (c != '\\') {
        ++i;
        continue;
      }
      if (++i >= n)
        return "the string is not closed";
      char e = s[i];
      if (e == 'u') {
        for (int k = 0; k < 4; ++k)
          if (++i >= n || !llvm::isHexDigit(s[i]))
            return "'\\u' needs four hexadecimal digits";
      } else if (!llvm::StringRef("\"\\/bfnrt").contains(e)) {
        return "'\\" + std::string(1, e) + "' is not an escape of JSON";
      }
      ++i;
    }
  };
  auto key = [&]() -> std::string {
    whitespace();
    if (i >= n || s[i] != '"')
      return "expected a property name in double quotes";
    if (std::string m = string(); !m.empty())
      return m;
    whitespace();
    if (i >= n || s[i] != ':')
      return "expected ':' after a property name";
    ++i;
    return "";
  };

  std::vector<char> open; // '[' and '{' around the value being read
  for (;;) {
    whitespace();
    if (i >= n)
      return problem("the text ends where a value should be");
    char c = s[i];
    if (c == '"') {
      size_t start = i;
      if (std::string m = string(); !m.empty()) {
        JsonProblem p = problem(m);
        if (m == "the string is not closed")
          p.offset = start;
        return p;
      }
    } else if (c == '-' || llvm::isDigit(c)) {
      if (c == '-')
        ++i;
      if (i < n && s[i] == '0') {
        ++i;
        if (i < n && llvm::isDigit(s[i]))
          return problem("a number cannot start with 0 followed by more digits");
      } else if (!digits()) {
        return problem("expected digits after '-'");
      }
      if (i < n && s[i] == '.') {
        ++i;
        if (!digits())
          return problem("expected digits after the decimal point");
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-'))
          ++i;
        if (!digits())
          return problem("expected digits in the exponent");
      }
    } else if (c == '{' || c == '[') {
      ++i;
      whitespace();
      if (i < n && s[i] == (c == '{' ? '}' : ']')) {
        ++i;
      } else {
        open.push_back(c);
        if (c == '{')
          if (std::string m = key(); !m.empty())
            return problem(m);
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
    // The value ends the arrays and objects it completes.
    for (;;) {
      whitespace();
      if (open.empty()) {
        if (i < n)
          return problem("unexpected text after the value");
        return {};
      }
      char container = open.back();
      if (i < n && s[i] == ',') {
        ++i;
        if (container == '{')
          if (std::string m = key(); !m.empty())
            return problem(m);
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

// Why JSON.parse<T> cannot build a value of `type`, or empty if it can. A
// class is made through its constructor, with `classProblem` (fromJson(cls,
// j)); JSON.parse<T> and fromJson<T> make none.
using ClassProblem = llvm::function_ref<std::string(const ClassDecl *)>;
std::string fromJsonProblem(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen,
                            ClassProblem classProblem = nullptr) {
  switch (type->kind()) {
  case Type::Kind::Int:
  case Type::Kind::Long:
  case Type::Kind::Double:
  case Type::Kind::Decimal:
  case Type::Kind::Bool:
  case Type::Kind::String:
  case Type::Kind::Json:
    return "";
  case Type::Kind::Optional:
  case Type::Kind::Array:
    return fromJsonProblem(type->inner(), seen, classProblem);
  case Type::Kind::Object: {
    if (const ClassDecl *cls = type->classDecl(); cls && !cls->literal && classProblem)
      return seen.insert(type).second ? classProblem(cls) : "";
    if (type->isClass())
      return "'" + type->name().str() +
             "' is a class, and only its constructor makes one; make it with 'fromJson(" +
             type->name().str() + ", j)', which calls the constructor with the keys of the Json "
             "named as its parameters (docs/decorators.md, section 6)";
    if (type->isTuple())
      return "tuples are not supported yet; read an array, or an object with named fields";
    // Only a regex literal, `new RegExp` and a match make them.
    if (type->isRegExp())
      return "a RegExp comes only from a literal or 'new RegExp'; read the pattern as a string "
             "and pass it to 'new RegExp(pattern)'";
    if (type->isMatch())
      return "a match comes only from a regex; read the text and match it";
    // JSON has no dates: JSON.stringify writes one as its ISO text, and
    // JSON.parse gives that string back.
    if (type->isDate())
      return "JSON has no dates, and JSON.parse in JavaScript gives the string JSON.stringify "
             "wrote; read it as a 'string' and make the date with 'new Date(text)'";
    if (type->isIntl())
      return type->intlKind() == IntlKind::Segments
                 ? "Intl.Segments come only from 'segment(text)' of an Intl.Segmenter"
                 : "an " + type->name().str() + " comes only from 'new " + type->name().str() +
                       "(...)'";
    if (type->isCrypto())
      return "a " + type->name().str() + " comes only from the functions of 'node:crypto'";
    // JSON.stringify writes a Uint8Array as an object with the indices as
    // keys, and JSON.parse gives that object back.
    if (type->isBytes())
      return "JSON has no binary data; read the bytes as a 'Json' or an 'int[]' and make the '" +
             type->name().str() + "' from them";
    if (type->isAnyObject() || type->isIncomplete())
      return "the type is not known here";
    if (!seen.insert(type).second)
      return "";
    for (const Field &f : type->fields())
      if (std::string problem = fromJsonProblem(f.type, seen, classProblem); !problem.empty())
        return problem;
    return "";
  }
  case Type::Kind::Dict:
    if (type->isRecord())
      return fromJsonProblem(type->valueType(), seen, classProblem);
    return "JSON has objects, not a " + std::string(type->isMap() ? "Map" : "Set") +
           "; read a " + (type->isMap() ? "'Record<string, T>'" : "'T[]'") + " and make the " +
           (type->isMap() ? "Map of it, 'new Map(Object.entries(r))'" : "Set of it, 'new Set(xs)'");
  case Type::Kind::Param:
    return "a type parameter is not known until the function is called; give the type itself";
  case Type::Kind::Literal:
    return "";
  case Type::Kind::Union: {
    // The member a value goes to is told by what kind of JSON value it is, by
    // the value itself for a literal, and among objects by a tag field.
    std::vector<const Type *> numbers, objects;
    for (const Type *m : type->members()) {
      if (std::string problem = fromJsonProblem(m, seen); !problem.empty())
        return problem;
      const Type *base = m->isLiteral() ? m->literalBase() : m;
      if ((base->isNumeric() || base->isDecimal()) && !m->isLiteral())
        numbers.push_back(m);
      if (m->isObject() || m->isDict())
        objects.push_back(m);
    }
    if (numbers.size() > 1)
      return "'" + numbers[0]->name().str() + "' and '" + numbers[1]->name().str() +
             "' are both numbers in JSON, so a number could be either; keep one of them";
    if (objects.size() > 1) {
      auto record = llvm::find_if(objects, [](const Type *m) { return m->isDict(); });
      if (record != objects.end())
        return "a Record is an object in JSON, as '" +
               (*record == objects[0] ? objects[1] : objects[0])->name().str() +
               "' is, so an object could be either; keep one of them";
      if (jsonTagField(objects).empty())
        return "'" + objects[0]->name().str() + "' and '" + objects[1]->name().str() +
               "' are both objects in JSON, and no field tells them apart; give each a field "
               "of a literal type with its own value, e.g. 'kind: \"circle\"'";
    }
    return "";
  }
  default:
    return "a value of type '" + type->name().str() + "' cannot come from JSON";
  }
}

// A class among `type`, its arrays and its fields with a method toJSON,
// which JSON.stringify in JavaScript would call; null if none.
const ClassDecl *classWithToJson(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  while (type->isOptional() || type->isArray())
    type = type->inner();
  if (type->isResult()) {
    if (const ClassDecl *cls = classWithToJson(type->okType(), seen))
      return cls;
    return classWithToJson(type->errType(), seen);
  }
  // A Record writes its values; a Map and a Set write `{}`.
  if (type->isRecord())
    return classWithToJson(type->valueType(), seen);
  if (!type->isObject() || !seen.insert(type).second)
    return nullptr;
  if (const ClassDecl *cls = type->classDecl())
    for (const FunctionDecl *fn : cls->methods)
      if (fn->name == "toJSON" && !fn->isStatic)
        return cls;
  for (const Field &f : type->fields())
    if (const ClassDecl *cls = classWithToJson(f.type, seen))
      return cls;
  return nullptr;
}

// Whether JSON.stringify of a value of `type` may give undefined: for
// undefined itself, a function, or a `T | undefined`.
bool mayStringifyToUndefined(const Type *type) {
  return type->isUndefined() || type->isOptional() || type->isFunction() || type->isNever() ||
         type->isUnknown();
}

} // namespace

bool Sema::holdsJson(const Type *type) {
  while (type->isOptional() || type->isArray())
    type = type->inner();
  return type->isJson();
}

bool Sema::isNullName(const Expr *expr) { return nullName(expr); }

Expr *Sema::checkNull(NameExpr *name, const Type *expected) {
  if (!isNullName(name) || !expected || !expected->nonOptional()->isJson())
    return nullptr;
  name->type = Type::json();
  return name;
}

//===----------------------------------------------------------------------===//
// Reading and writing keys
//===----------------------------------------------------------------------===//

Expr *Sema::checkJsonMember(MemberExpr *member) {
  member->type = Type::optional(Type::json());
  return member;
}

// A key is a string, or a number, which JavaScript turns into its text:
// `j[1]` of an object reads "1".
Expr *Sema::checkJsonIndex(IndexExpr *index) {
  index->index = checkExpr(index->index);
  index->type = Type::optional(Type::json());
  const Type *key = index->index->type;
  if (key->isError() || key->isString())
    return index;
  if (key->isNumeric()) {
    index->index = convert(index->index, Type::doubleTy());
    return index;
  }
  diags_.error(index->index->loc(), "a key of a Json is a string or a number, found '" +
                                        key->name() + "'");
  index->type = Type::error();
  return index;
}

const Type *Sema::checkJsonTarget(Expr *target) {
  if (auto *index = dyn_cast<IndexExpr>(target)) {
    if (checkJsonIndex(index)->type->isError())
      return Type::error();
  } else {
    checkJsonMember(cast<MemberExpr>(target));
  }
  return Type::optional(Type::json());
}

Expr *Sema::reportJsonMethod(CallExpr *call, MemberExpr *member) {
  const Expr *object = member->object;
  if (auto *conv = dyn_cast<CastExpr>(object); conv && conv->isImplicit())
    object = conv->operand;
  std::string message = "a Json has no method '" + member->name.str() +
                        "' until a check shows what it holds";
  std::string call_ = "." + member->name.str() + "(...)";
  if (auto *name = dyn_cast<NameExpr>(object); name && name->var) {
    std::string x = name->name.str();
    message += ", e.g. 'if (Array.isArray(" + x + ")) " + x + call_ + "' or 'if (typeof " + x +
               " == \"string\") " + x + call_ + "'";
  } else {
    message += "; put it in a variable and check that, e.g. 'const items = j.items; if "
               "(Array.isArray(items)) items" +
               call_ + "'";
  }
  diags_.error(member->loc(), message);
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

//===----------------------------------------------------------------------===//
// Literals
//===----------------------------------------------------------------------===//

// `{ phone: null, tags: ["a"] }` where a Json is expected: a Json object.
// A value that is undefined leaves its key out, as JSON.stringify does, and
// a key in brackets may be any string.
Expr *Sema::checkJsonObjectLit(ObjectLitExpr *lit) {
  const Type *json = Type::json();
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.method) {
      diags_.error(prop.loc, prop.spread ? "a spread is not supported in a Json literal; write the "
                                           "keys one by one"
                                         : "a Json holds no functions");
      prop.value = checkExpr(prop.value);
      continue;
    }
    if (prop.defaulted) {
      diags_.error(prop.loc, "'" + prop.name +
                                 " = ...' belongs to a destructuring pattern; write '" + prop.name +
                                 ": ...'");
      prop.value = checkExpr(cast<AssignExpr>(prop.value)->value);
      prop.defaulted = false;
      continue;
    }
    if (prop.key) {
      prop.key = checkExpr(prop.key);
      if (!prop.key->type->isString() && !prop.key->type->isError())
        diags_.error(prop.key->loc(), "a key of a Json is a string, found '" +
                                          prop.key->type->name() + "'");
    }
    Expr *value = checkExpr(prop.value, json);
    const Type *type = value->type;
    prop.value = coerce(value, type->isOptional() || type->isUndefined() ? Type::optional(json)
                                                                         : json,
                        "key '" + prop.name.str() + "'");
  }
  lit->type = json;
  return lit;
}

// `[1, "a", null]` where a Json is expected: a Json[], which becomes the Json.
Expr *Sema::checkJsonArrayLit(ArrayLitExpr *lit) {
  Expr *array = checkArrayLit(lit, Type::array(Type::json()));
  if (array->type->isError())
    return array;
  return coerce(array, Type::json());
}

//===----------------------------------------------------------------------===//
// Equality and narrowing
//===----------------------------------------------------------------------===//

// `x == null` is true for null and undefined, as in JavaScript, and only a
// Json holds null.
Expr *Sema::checkNullEquality(BinaryExpr *bin) {
  bool lhsNull = isNullName(bin->lhs), rhsNull = isNullName(bin->rhs);
  if (!lhsNull && !rhsNull)
    return nullptr;
  bin->type = Type::boolTy();
  Expr *&null = lhsNull ? bin->lhs : bin->rhs;
  Expr *&other = lhsNull ? bin->rhs : bin->lhs;
  const Type *type = Type::undefinedTy();
  if (!(lhsNull && rhsNull)) {
    other = checkExpr(other);
    type = other->type;
  }
  null->type = Type::json();
  if (type->isError() || type->nonOptional()->isJson())
    return bin;
  diags_.error(null->loc(), "'null' exists only in Json values, and a value of type '" +
                                type->name() +
                                "' is never null; the one value for nothing is 'undefined': '" +
                                opSpelling(bin->op) + " undefined'");
  null->type = Type::error();
  return bin;
}

Expr *Sema::checkJsonEquality(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bool lhsJson = l->nonOptional()->isJson(), rhsJson = r->nonOptional()->isJson();
  const Type *json =
      l->isOptional() || r->isOptional() ? Type::optional(Type::json()) : Type::json();
  if (lhsJson && rhsJson) {
    bin->lhs = convert(bin->lhs, json);
    bin->rhs = convert(bin->rhs, json);
    return bin;
  }
  // `j == "admin"`, `j.count == 3`: as `===`, true for a Json of that kind
  // and value.
  Expr *&other = lhsJson ? bin->rhs : bin->lhs;
  const Type *type = other->type;
  if (type->isString() || type->isBool())
    return bin;
  if (type->isNumeric()) {
    other = convert(other, Type::doubleTy());
    return bin;
  }
  if (type->isOptional() && isImplicitlyConvertible(type->inner(), Type::json())) {
    bin->lhs = convert(bin->lhs, json);
    bin->rhs = convert(bin->rhs, json);
    return bin;
  }
  reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
  return bin;
}

void Sema::addJsonFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  // `Array.isArray(x)`: x is a Json[].
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin == Builtin::ArrayIsArray) {
    if (const VarSymbol *var = jsonVariable(call->args[0])) {
      whenTrue.narrowed.push_back({var, Type::array(Type::json())});
      if (var->type->isOptional())
        whenTrue.defined.insert(var);
    }
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin || (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne))
    return;
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    // `x != null`, `x != undefined`: neither, so not undefined.
    if (isNothing(other)) {
      if (const VarSymbol *var = jsonVariable(side); var && var->type->isOptional())
        different.defined.insert(var);
      continue;
    }
    // `typeof x == "string"`: a string, a number (a double) or a boolean.
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text)
      continue;
    const VarSymbol *var = jsonVariable(typeOf->operand);
    if (!var)
      continue;
    const Type *narrowed = text->value == u"string"    ? Type::string()
                           : text->value == u"number"  ? Type::doubleTy()
                           : text->value == u"boolean" ? Type::boolTy()
                                                       : nullptr;
    if (narrowed)
      equal.narrowed.push_back({var, narrowed});
    if (var->type->isOptional()) {
      if (narrowed || text->value == u"object")
        equal.defined.insert(var);
      else if (text->value == u"undefined")
        different.defined.insert(var);
    }
  }
}

//===----------------------------------------------------------------------===//
// JSON, embedJson, embedText, fromJson
//===----------------------------------------------------------------------===//

std::string Sema::jsonProblem(const Type *type, ClassProblem classProblem) {
  llvm::SmallPtrSet<const Type *, 8> seen;
  return fromJsonProblem(type, seen, classProblem);
}

bool Sema::checkFromJsonType(const Type *type, llvm::SMLoc loc, llvm::StringRef fn) {
  if (type->isError())
    return false;
  llvm::SmallPtrSet<const Type *, 8> seen;
  std::string problem = fromJsonProblem(type, seen);
  if (problem.empty())
    return true;
  diags_.error(loc,
               fn + "<" + type->name() + "> cannot build a '" + type->name() + "': " + problem);
  return false;
}

Expr *Sema::checkJsonStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      if (!isNullName(arg))
        arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;
  llvm::StringRef fn = member->name;
  if (fn == "parse") {
    if (call->args.size() == 2) {
      diags_.error(call->args[1]->loc(), "the reviver of JSON.parse is not supported; convert the "
                                         "Json it gives, or give the type: 'JSON.parse<T>(text)'");
      checkArgsLoosely();
      return call;
    }
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    call->args[0] = checkArg(call->args[0], Type::string());
    if (member->typeArgs.empty()) {
      call->builtin = Builtin::JsonParse;
      call->type = Type::json();
      return call;
    }
    if (member->typeArgs.size() != 1) {
      diags_.error(member->loc(), "JSON.parse takes one type argument, the type to build: "
                                  "'JSON.parse<User>(text)'");
      return call;
    }
    const Type *target = resolveType(member->typeArgs[0], /*allowVoid=*/false);
    if (!checkFromJsonType(target, member->typeArgs[0].loc, "JSON.parse"))
      return call;
    call->builtin = Builtin::JsonParseAs;
    call->type = target;
    return call;
  }
  if (!member->typeArgs.empty())
    diags_.error(member->loc(), "only JSON.parse takes a type argument");
  if (fn != "stringify") {
    diags_.error(member->loc(), "'JSON." + fn + "' does not exist; JSON has parse and stringify");
    checkArgsLoosely();
    return call;
  }
  if (!checkArity(call, member->loc(), 1, 3))
    return call;
  // JSON.stringify(value, replacer, space): the replacer may only be null or
  // undefined, and is dropped; the space is a number or a string.
  Expr *value = call->args[0] = checkExpr(call->args[0]);
  bool ok = !value->type->isError();
  if (value->type->isVoid()) {
    diags_.error(value->loc(), "an expression of type 'void' has no value to write as JSON");
    ok = false;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  if (const ClassDecl *cls = ok ? classWithToJson(value->type, seen) : nullptr) {
    diags_.error(value->loc(), "JSON.stringify does not call the method toJSON of class '" +
                                   cls->name + "': tz calls no code by itself; call it, e.g. "
                                               "'JSON.stringify(x.toJSON())'");
    ok = false;
  }
  std::vector<Expr *> args{value};
  if (call->args.size() >= 2) {
    Expr *replacer = call->args[1];
    if (!isNullName(replacer) && !isa<UndefinedExpr>(replacer)) {
      replacer = checkExpr(replacer);
      if (!replacer->type->isError())
        diags_.error(replacer->loc(), "a replacer is not supported: tz calls no code by itself; "
                                      "build the value to write first, and pass 'null' here");
      ok = false;
    }
  }
  if (call->args.size() == 3) {
    Expr *space = call->args[2];
    if (!isNullName(space) && !isa<UndefinedExpr>(space)) {
      space = checkExpr(space);
      const Type *type = space->type;
      if (type->isNumeric())
        args.push_back(convert(space, Type::doubleTy()));
      else if (type->isString())
        args.push_back(space);
      else if (!type->isError()) {
        diags_.error(space->loc(), "the space of JSON.stringify is a number of spaces or a "
                                   "string, found '" + type->name() + "'");
        ok = false;
      }
    }
  }
  if (!ok)
    return call;
  call->args = std::move(args);
  call->builtin = Builtin::JsonStringify;
  call->type =
      mayStringifyToUndefined(value->type) ? Type::optional(Type::string()) : Type::string();
  return call;
}

bool Sema::readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes, std::string &shown,
                        std::string &full) {
  Expr *arg = call->args[0];
  std::string example =
      (fn + (fn == "embedText" ? "(\"./page.html\")" : "(\"./data.json\")")).str();
  std::u16string path;
  if (auto *lit = dyn_cast<StringLitExpr>(arg))
    path = lit->value;
  else if (auto *tmpl = dyn_cast<TemplateExpr>(arg); tmpl && tmpl->exprs.empty())
    path = tmpl->quasis[0];
  else {
    call->args[0] = checkExpr(arg);
    diags_.error(arg->loc(), "the path of " + fn + " must be a string literal: the file is read "
                                                    "when the program is compiled, e.g. '" +
                                 example + "'");
    return false;
  }
  shown = toUtf8(path);
  if (llvm::StringRef(shown).contains('\\')) {
    diags_.error(arg->loc(), "write '/' in the path, not '\\', as on every system");
    return false;
  }
  if (shown.empty() || llvm::sys::path::is_absolute(shown) || shown[0] == '/') {
    diags_.error(arg->loc(), "the path of " + fn + " is relative to the file it is written in, "
                                                    "e.g. '" + example + "'");
    return false;
  }
  llvm::SourceMgr &sources = diags_.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(call->loc());
  llvm::StringRef source = id ? sources.getMemoryBuffer(id)->getBufferIdentifier() : "";
  llvm::SmallString<256> path8(llvm::sys::path::parent_path(source));
  llvm::sys::path::append(path8, shown);
  llvm::sys::path::remove_dots(path8, /*remove_dot_dot=*/true);
  llvm::sys::path::native(path8);
  full = std::string(path8);
  auto buffer =
      llvm::MemoryBuffer::getFile(full, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!buffer) {
    diags_.error(arg->loc(), "cannot read '" + shown + "': " + buffer.getError().message());
    return false;
  }
  bytes = (*buffer)->getBuffer().str();
  return true;
}

Expr *Sema::checkJsonGlobal(CallExpr *call, NameExpr *name) {
  static const llvm::StringLiteral names[] = {"embedJson", "embedText", "embedBytes", "fromJson"};
  if (!llvm::any_of(names, [&](llvm::StringRef n) { return isGlobalName(name, n); }))
    return nullptr;
  llvm::StringRef fn = name->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;

  if (fn == "fromJson") {
    // fromJson(User, j): through the constructor of a class.
    if (name->typeArgs.empty() && call->args.size() == 2)
      return checkFromJsonClass(call, name);
    if (name->typeArgs.size() != 1) {
      diags_.error(name->loc(), "fromJson needs the type to build, e.g. 'fromJson<User>(j)', or "
                                "the class to make an object of, 'fromJson(User, j)'");
      checkArgsLoosely();
      return call;
    }
    if (!checkArity(call, name->loc(), 1, 1))
      return call;
    // A `Json | undefined` too: undefined is for an optional type to take.
    Expr *arg = checkExpr(call->args[0], Type::json());
    call->args[0] = coerce(arg, arg->type->isOptional() || arg->type->isUndefined()
                                    ? Type::optional(Type::json())
                                    : Type::json());
    const Type *target = resolveType(name->typeArgs[0], /*allowVoid=*/false);
    if (!checkFromJsonType(target, name->typeArgs[0].loc, "fromJson"))
      return call;
    call->builtin = Builtin::FromJson;
    call->type = target;
    return call;
  }
  if (!name->typeArgs.empty())
    diags_.error(name->loc(), "'" + fn + "' takes no type arguments");

  if (fn == "embedBytes")
    return checkEmbedBytes(call, name);
  if (!checkArity(call, name->loc(), 1, 1))
    return call;
  std::string bytes, shown, full;
  if (!readEmbedded(call, fn, bytes, shown, full))
    return call;
  llvm::StringRef text(bytes);
  // A file saved with a byte order mark: require() of a JSON file skips it
  // in node too.
  if (fn == "embedJson")
    text.consume_front("\xEF\xBB\xBF");
  llvm::SmallVector<llvm::UTF16, 64> units;
  if (!llvm::convertUTF8ToUTF16String(text, units)) {
    diags_.error(call->args[0]->loc(), "'" + shown + "' is not text in UTF-8");
    return call;
  }
  std::u16string content(units.begin(), units.end());

  if (fn == "embedText") {
    auto *lit = ctx_.make<StringLitExpr>(call->loc(), std::move(content));
    lit->type = Type::string();
    return lit;
  }
  if (JsonProblem problem = checkJsonText(text); !problem.message.empty()) {
    // The line and the column in the file, which a note shows.
    size_t line = 1, column = 1;
    for (size_t i = 0; i < problem.offset && i < text.size(); ++i) {
      if (text[i] == '\n') {
        ++line;
        column = 1;
      } else if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
        ++column;
      }
    }
    diags_.error(call->args[0]->loc(), "'" + shown + "' is not valid JSON: " + problem.message +
                                           " (line " + std::to_string(line) + ", column " +
                                           std::to_string(column) + ")");
    llvm::SourceMgr &sources = diags_.sourceMgr();
    unsigned id =
        sources.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(text, full), llvm::SMLoc());
    const char *start = sources.getMemoryBuffer(id)->getBufferStart();
    diags_.note(llvm::SMLoc::getFromPointer(start + std::min(problem.offset, text.size())),
                "the JSON is wrong here");
    return call;
  }
  // The text, and the file it came from: one file gives one value, whichever
  // file of the program embeds it (see CodeGen::emitJsonCall).
  llvm::SMLoc loc = call->args[0]->loc();
  auto *lit = ctx_.make<StringLitExpr>(loc, std::move(content));
  lit->type = Type::string();
  llvm::SmallVector<llvm::UTF16, 64> path;
  llvm::convertUTF8ToUTF16String(full, path);
  auto *file = ctx_.make<StringLitExpr>(loc, std::u16string(path.begin(), path.end()));
  file->type = Type::string();
  call->args = {lit, file};
  call->builtin = Builtin::EmbedJson;
  call->type = Type::json();
  return call;
}

Expr *Sema::checkArrayIsArray(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 1))
    return call;
  call->args[0] = checkExpr(call->args[0]);
  if (call->args[0]->type->isVoid()) {
    diags_.error(call->args[0]->loc(), "an expression of type 'void' has no value to test");
    return call;
  }
  call->builtin = Builtin::ArrayIsArray;
  call->type = Type::boolTy();
  return call;
}

} // namespace tz
