#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

bool hasMethodFields(const ClassDecl *cls) {
  if (cls->literal()) {
    return true;
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &f) { return f.method(); })) {
      return true;
    }
  }

  return false;
}

bool isInherited(llvm::StringRef name) {
  static const char *const names[] = {"toString",      "valueOf",        "hasOwnProperty",
                                      "isPrototypeOf", "toLocaleString", "propertyIsEnumerable",
                                      "constructor",   "__proto__"};

  return llvm::is_contained(names, name);
}

std::string noField(const Type *type, llvm::StringRef name) {
  std::string message =
      ("property '" + name + "' does not exist on type '" + type->name() + "'").str();
  if (isInherited(name)) {
    message += "; objects inherit nothing from Object.prototype";
  }

  return message;
}

std::string chainText(const Expr *expr) {
  expr = unnarrowed(expr);
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (!member || member->optional) {
    return "";
  }
  std::string outer = chainText(member->object);

  return outer.empty() ? "" : outer + "." + member->name.str();
}

}

std::string fieldText(const Expr *object, llvm::StringRef field) {
  std::string text = chainText(object);
  if (text.empty()) {
    text = "o";
  }
  bool identifier = !field.empty() && !llvm::isDigit(field[0]) && llvm::all_of(field, [](char c) {
    return llvm::isAlnum(c) || c == '_' || c == '$';
  });

  return identifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

ObjectChecker::ObjectChecker(Checker &owner)
    : checker(owner) {}

Expr *ObjectChecker::checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  if (fn == "groupBy") {
    return this->checker.dictMethods().checkGroupBy(call, member, false);
  }
  if (fn == "fromEntries") {
    return this->checker.dictMethods().checkFromEntries(call, member, expected);
  }
  if (fn != "keys" && fn != "values" && fn != "entries") {
    this->checker.diags_.error(member->loc(),
                               "'Object." + fn +
                                   "' is not supported; only Object.keys, Object.values, "
                                   "Object.entries, Object.fromEntries and Object.groupBy are");
    checkArgsLoosely();
    return call;
  }
  bool rejected = this->checker.callChecker().rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->checker.argumentChecker().checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  Expr *&arg = call->args[0];
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    if (EnumDecl *decl = this->checker.scopes().lookup(name->name).enumDecl()) {
      return this->enumMembers(call, decl, fn);
    }
  }
  arg = this->checker.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (type->nonOptional()->isJson()) {
    call->target = BuiltinCall{.builtin = fn == "keys"     ? Builtin::ObjectKeys
                                          : fn == "values" ? Builtin::ObjectValues
                                                           : Builtin::ObjectEntries};
    const Type *json = Type::json();
    call->type = fn == "keys"     ? Type::array(Type::string())
                 : fn == "values" ? Type::array(json)
                                  : Type::array(Type::tuple({Type::string(), json}));
    return call;
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    this->checker.diags_.error(arg->loc(), "the object may be undefined; check it first");
    return call;
  }
  if (type->isDict()) {
    return this->checker.dictMethods().checkDictObjectCall(call, fn);
  }
  if (!type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
      type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto()) {
    this->checker.diags_.error(arg->loc(),
                               "'Object." + fn + "' needs an object, found '" + type->name() + "'");
    return call;
  }
  if (fn == "keys") {
    call->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
    call->type = Type::array(Type::string());
    return call;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
    this->checker.classDeclarer().completeClass(cls);
    if (this->checker.eventMaps().isEmitterClass(cls)) {
      this->checker.diags_.error(arg->loc(),
                                 "'Object." + fn +
                                     "' of an emitter is not supported: its fields are the "
                                     "listeners it keeps; read the fields of its class by name");
      return call;
    }
    if (hasMethodFields(cls)) {
      this->checker.diags_.error(arg->loc(), "'Object." + fn +
                                                 "' of an object with methods is not supported; "
                                                 "read the fields by name");
      return call;
    }
  }
  const Type *element = nullptr;
  std::vector<std::pair<const Type *, const Field *>> fields;
  for (const Field &f : type->fields()) {
    fields.push_back({type, &f});
  }
  for (const Type *t : this->checker.file_->subtypes) {
    if (t == type || !t->isSubtypeOf(type)) {
      continue;
    }
    if (ClassDecl *sub = ClassDeclarer::classOf(t)) {
      this->checker.classDeclarer().completeClass(sub);
    }
    for (const Field &f : t->fields()) {
      fields.push_back({t, &f});
    }
  }
  for (auto [t0, field] : fields) {
    const Field &f = *field;
    const Type *t = f.type->nonOptional();
    const Type *both = element ? Type::unionOf({element, t}) : t;
    if (std::string problem = unionProblem(both); !problem.empty()) {
      this->checker.diags_.error(arg->loc(),
                                 "'Object." + fn +
                                     "' puts the values of all the fields in one array, of the "
                                     "union of their types, and the fields of '" +
                                     t0->name() + "' have types that no union holds: " + problem +
                                     "; read the fields by name");
      return call;
    }
    element = both;
  }
  if (!element) {
    this->checker.diags_.error(arg->loc(), "'" + type->name() + "' has no fields, so 'Object." +
                                               fn + "' would always be empty");
    return call;
  }
  if (fn == "values") {
    call->target = BuiltinCall{.builtin = Builtin::ObjectValues};
    call->type = Type::array(element);
  } else {
    call->target = BuiltinCall{.builtin = Builtin::ObjectEntries};
    call->type = Type::array(Type::tuple({Type::string(), element}));
  }

  return call;
}

Expr *ObjectChecker::enumMembers(CallExpr *call, EnumDecl *decl, llvm::StringRef fn) {
  const Type *values = this->checker.typeDeclarer().enumType(decl);
  if (!decl->isString && fn != "keys") {
    this->checker.diags_.error(
        call->args[0]->loc(), "'Object." + fn + "' of numeric enum '" + decl->name +
                                  "' is not supported: in JavaScript it mixes the names of the "
                                  "members with their numbers; list the members you need, e.g. '[" +
                                  decl->name + "." +
                                  (decl->members.empty() ? "A" : decl->members.front().name.str()) +
                                  "]'");
    return call;
  }
  auto *list = this->checker.ctx_.make<ArrayLitExpr>(call->loc());
  for (const EnumMember &m : decl->members) {
    auto *name = this->checker.ctx_.make<StringLitExpr>(call->loc(), utf16(m.name));
    auto *member = this->checker.ctx_.make<MemberExpr>(
        call->loc(), this->checker.ctx_.make<NameExpr>(call->loc(), decl->name), m.name);
    Expr *element = fn == "keys" ? static_cast<Expr *>(name) : member;
    if (fn == "entries") {
      auto *pair = this->checker.ctx_.make<ArrayLitExpr>(call->loc());
      pair->elements.push_back({name, false});
      pair->elements.push_back({member, false});
      element = pair;
    }
    list->elements.push_back({element, false});
  }
  const Type *element = fn == "keys"     ? Type::string()
                        : fn == "values" ? values
                                         : Type::tuple({Type::string(), values});
  Expr *checked = this->checker.exprChecker().checkExpr(list, Type::array(element));
  this->checker.file_->namedUses[checked] = {decl, call->args[0]->loc(), ""};

  return checked;
}

Expr *ObjectChecker::enumObject(NameExpr *name, const Type *expected) {
  const Type *target = expected ? expected->nonOptional() : nullptr;
  if (!target || !target->isDict()) {
    return nullptr;
  }
  EnumDecl *decl = this->checker.scopes().lookup(name->name).enumDecl();
  if (!decl) {
    return nullptr;
  }
  this->checker.typeDeclarer().resolveEnum(decl);
  auto *object = this->checker.ctx_.make<ObjectLitExpr>(name->loc());
  for (const EnumMember &m : decl->members) {
    ObjectLitExpr::Property p;
    p.name = m.name;
    p.loc = name->loc();
    p.value = this->checker.ctx_.make<MemberExpr>(
        name->loc(), this->checker.ctx_.make<NameExpr>(name->loc(), decl->name), m.name);
    object->props.push_back(p);
  }
  Expr *checked = this->checker.exprChecker().checkExpr(object, expected);
  this->checker.file_->namedUses[checked] = {decl, name->loc(), ""};

  return checked;
}

void ObjectChecker::checkForIn(ForOfStmt *stmt) {
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool env = this->checker.processChecker().isProcessMember(stmt->iterable, "env");
  Expr *object = this->checker.exprChecker().checkExpr(stmt->iterable);
  const Type *type = object->type;
  if (type->isRecord() && (variable || env) && !isa<VarDeclStmt>(stmt->body) &&
      !isa<FunctionDecl>(stmt->body)) {
    llvm::SMLoc loc = stmt->body->loc();
    Expr *again = variable
                      ? static_cast<Expr *>(this->checker.ctx_.make<NameExpr>(loc, variable->name))
                      : this->checker.ctx_.make<MemberExpr>(
                            loc, this->checker.ctx_.make<NameExpr>(loc, "process"), "env");
    auto *has = this->checker.ctx_.make<BinaryExpr>(
        loc, BinaryOp::In, this->checker.ctx_.make<NameExpr>(loc, stmt->binding.name), again);
    auto *skip = this->checker.ctx_.make<IfStmt>(
        loc, this->checker.ctx_.make<UnaryExpr>(loc, UnaryOp::Not, has),
        this->checker.ctx_.make<ContinueStmt>(loc, ""), nullptr);
    auto *block = dyn_cast<BlockStmt>(stmt->body);
    if (!block) {
      block = this->checker.ctx_.make<BlockStmt>(loc);
      block->body.push_back(stmt->body);
      stmt->body = block;
    }
    block->body.insert(block->body.begin(), skip);
  }
  std::string text = type->isString() ? "s" : "a";
  if (auto *name = dyn_cast<NameExpr>(unnarrowed(object))) {
    text = name->name.str();
  }
  bool ok = false;
  if (type->isArray() || type->isString() || type->isTuple()) {
    const char *what = type->isArray() ? "an array" : type->isString() ? "a string" : "a tuple";
    this->checker.diags_.error(
        object->loc(),
        std::string("'for...in' over ") + what +
            " gives its indices as strings ('0', '1'...), so 'i + 1' would be '01'; walk "
            "the elements with 'for (const x of " +
            text + ")', or the indices with 'for (let i = 0; i < " + text + ".length; i++)'" +
            (type->isArray() ? " or '" + text + ".forEach((x, i) => ...)'" : ""));
  } else if (type->isOptional() && type->inner()->isObject()) {
    this->checker.diags_.error(object->loc(),
                               "the object may be undefined; check it before walking its keys");
  } else if (type->nonOptional()->isJson()) {
    ok = true;
  } else if (type->isRecord()) {
    ok = true;
  } else if (type->isDict()) {
    this->checker.diags_.error(
        object->loc(), std::string("'for...in' walks the properties of an object, and a ") +
                           (type->isMap() ? "Map" : "Set") + " has none in JavaScript; walk its " +
                           (type->isMap() ? "keys" : "values") + " with 'for (const x of " + text +
                           (type->isMap() ? ".keys()" : "") + ")'");
  } else if (type->isOptional() && type->inner()->isRecord()) {
    this->checker.diags_.error(object->loc(),
                               "the object may be undefined; check it before walking its keys");
  } else if ((!type->isObject() || type->isRegExp() || type->isMatch() || type->isDate() ||
              type->isIntl() || type->isBytes() || type->isCrypto()) &&
             !type->isError()) {
    this->checker.diags_.error(object->loc(), "'for...in' walks the keys of an object, found '" +
                                                  type->name() + "'");
  } else {
    ok = !type->isError();
  }
  llvm::SMLoc loc = object->loc();
  auto *callee = this->checker.ctx_.make<MemberExpr>(
      loc, this->checker.ctx_.make<NameExpr>(loc, "Object"), "keys");
  auto *keys = this->checker.ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->target = BuiltinCall{.builtin = Builtin::ObjectKeys};
  keys->type = ok ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;
  stmt->iterable = keys;
}

Expr *ObjectChecker::checkField(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isClass()) {
    return this->checker.classMemberChecker().checkClassMember(member, nullptr);
  }
  const Field *field = type->field(member->name);
  if (!field) {
    this->checker.diags_.error(member->loc(), noField(type, member->name));
    member->type = Type::error();
    return member;
  }
  member->type = field->type;
  if (Expr *narrowed = this->checker.fieldFacts().narrowedField(member); narrowed != member) {
    return narrowed;
  }
  if (field->type->isOptional()) {
    if (std::optional<Checker::FieldPath> path = this->checker.fieldFacts().fieldPath(member);
        path && this->checker.fieldFacts().isFieldDefined(*path)) {
      return this->checker.typeRelations().convert(member, field->type->inner());
    }
  }

  return member;
}

Expr *ObjectChecker::checkFieldCall(CallExpr *call, MemberExpr *member) {
  if (member->object->type->overload(member->name)) {
    return this->checker.overloadResolver().checkOverloadCall(call, member);
  }
  call->callee = this->checkField(member);
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    return this->checker.callChecker().checkValueCall(call, type);
  }
  if (type->isOptional() && type->inner()->isFunction()) {
    this->checker.diags_.error(
        member->loc(), "the function may be undefined; check it first, or call it with '?.()', "
                       "which gives undefined then");
  } else if (!type->isError()) {
    this->checker.diags_.error(member->loc(), "field '" + member->name +
                                                  "' is not a function; it has type '" +
                                                  type->name() + "'");
  }
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }
  call->type = Type::error();

  return call;
}

const Type *ObjectChecker::checkObjectFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isOptional()) {
    this->checker.diags_.error(member->object->loc(),
                               "the value may be undefined; check it before assigning to '" +
                                   member->name + "'");
    return Type::error();
  }
  if (type->isTuple() && member->name == "length") {
    this->checker.diags_.error(member->loc(),
                               "a tuple has a fixed length, which cannot be changed");
    return Type::error();
  }
  const Field *field = type->field(member->name);
  if (!field) {
    if (type->overload(member->name)) {
      this->checker.diags_.error(member->loc(),
                                 "'" + member->name + "' is a method of '" + type->name() +
                                     "' with several signatures and cannot be replaced");
    } else {
      this->checker.diags_.error(member->loc(), noField(type, member->name));
    }
    return Type::error();
  }
  if (field->readonly) {
    this->checker.diags_.error(member->loc(), "cannot assign to '" + member->name +
                                                  "' because it is a read-only field of '" +
                                                  type->name() + "'");
  }
  member->type = field->type;

  return field->type;
}

std::string ObjectChecker::objectMismatch(const Type *from, const Type *to, bool conflict) const {
  for (const Field &f : from->fields()) {
    if (!to->field(f.name)) {
      return "; an object must have exactly the fields of its type, and this one has the extra "
             "field '" +
             f.name + "'";
    }
  }
  for (const Field &t : to->fields()) {
    if (!from->field(t.name)) {
      return "; it has no field '" + t.name + "'";
    }
  }
  for (const Field &f : from->fields()) {
    const Field *t = to->field(f.name);
    if (t->type != f.type) {
      std::string message = ("; field '" + f.name + "' is '" + f.type->name() + "' here and '" +
                             t->type->name() + "' in '" + to->name() + "'")
                                .str();
      if (!conflict && f.type->nonOptional()->isNumeric() && t->type->nonOptional()->isNumeric()) {
        message +=
            ": give the object its type where it is created, e.g. 'const p: " + to->name().str() +
            " = { ... }'";
      }
      return message;
    }
    if (f.readonly && !t->readonly) {
      return "; field '" + f.name + "' is read-only here, and '" + to->name().str() +
             "' would let it be changed";
    }
  }

  return "";
}

}
