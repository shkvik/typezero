#include "compiler/check/objects/object_checker/object_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/objects/dict_methods/dict_methods.h"
#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

bool hasMethodFields(const ClassDecl *cls) {
  if (cls->literal) {
    return true;
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (llvm::any_of(c->fields, [](const ClassField &f) { return f.method; })) {
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

ObjectChecker::ObjectChecker(Checker &checker) : checker_(checker) {}

Expr *ObjectChecker::checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (fn == "groupBy") {
    return checker_.dictMethods().checkGroupBy(call, member, false);
  }
  if (fn == "fromEntries") {
    return checker_.dictMethods().checkFromEntries(call, member, expected);
  }
  if (fn != "keys" && fn != "values" && fn != "entries") {
    checker_.diags_.error(member->loc(),
                          "'Object." + fn +
                              "' is not supported; only Object.keys, Object.values, "
                              "Object.entries, Object.fromEntries and Object.groupBy are");
    checkArgsLoosely();
    return call;
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
    return call;
  }
  Expr *&arg = call->args[0];
  arg = checker_.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (type->nonOptional()->isJson()) {
    call->builtin = fn == "keys"     ? Builtin::ObjectKeys
                    : fn == "values" ? Builtin::ObjectValues
                                     : Builtin::ObjectEntries;
    const Type *json = Type::json();
    call->type = fn == "keys"     ? Type::array(Type::string())
                 : fn == "values" ? Type::array(json)
                                  : Type::array(Type::tuple({Type::string(), json}));
    return call;
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    checker_.diags_.error(arg->loc(), "the object may be undefined; check it first");
    return call;
  }
  if (type->isDict()) {
    return checker_.dictMethods().checkDictObjectCall(call, fn);
  }
  if (!type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
      type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto()) {
    checker_.diags_.error(arg->loc(),
                          "'Object." + fn + "' needs an object, found '" + type->name() + "'");
    return call;
  }
  if (fn == "keys") {
    call->builtin = Builtin::ObjectKeys;
    call->type = Type::array(Type::string());
    return call;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
    checker_.classDeclarer().completeClass(cls);
    if (checker_.eventMaps().isEmitterClass(cls)) {
      checker_.diags_.error(arg->loc(),
                            "'Object." + fn +
                                "' of an emitter is not supported: its fields are the "
                                "listeners it keeps; read the fields of its class by name");
      return call;
    }
    if (hasMethodFields(cls)) {
      checker_.diags_.error(arg->loc(), "'Object." + fn +
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
  for (const Type *t : checker_.file_->subtypes) {
    if (t == type || !t->isSubtypeOf(type)) {
      continue;
    }
    if (ClassDecl *sub = ClassDeclarer::classOf(t)) {
      checker_.classDeclarer().completeClass(sub);
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
      checker_.diags_.error(arg->loc(),
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
    checker_.diags_.error(arg->loc(), "'" + type->name() + "' has no fields, so 'Object." + fn +
                                          "' would always be empty");
    return call;
  }
  if (fn == "values") {
    call->builtin = Builtin::ObjectValues;
    call->type = Type::array(element);
  } else {
    call->builtin = Builtin::ObjectEntries;
    call->type = Type::array(Type::tuple({Type::string(), element}));
  }

  return call;
}

void ObjectChecker::checkForIn(ForOfStmt *stmt) {
  auto *variable = dyn_cast<NameExpr>(stmt->iterable);
  bool env = checker_.processChecker().isProcessMember(stmt->iterable, "env");
  Expr *object = checker_.exprChecker().checkExpr(stmt->iterable);
  const Type *type = object->type;
  if (type->isRecord() && (variable || env) && !isa<VarDeclStmt>(stmt->body) &&
      !isa<FunctionDecl>(stmt->body)) {
    llvm::SMLoc loc = stmt->body->loc();
    Expr *again = variable ? static_cast<Expr *>(checker_.ctx_.make<NameExpr>(loc, variable->name))
                           : checker_.ctx_.make<MemberExpr>(
                                 loc, checker_.ctx_.make<NameExpr>(loc, "process"), "env");
    auto *has = checker_.ctx_.make<BinaryExpr>(
        loc, BinaryOp::In, checker_.ctx_.make<NameExpr>(loc, stmt->binding.name), again);
    auto *skip =
        checker_.ctx_.make<IfStmt>(loc, checker_.ctx_.make<UnaryExpr>(loc, UnaryOp::Not, has),
                                   checker_.ctx_.make<ContinueStmt>(loc, ""), nullptr);
    auto *block = dyn_cast<BlockStmt>(stmt->body);
    if (!block) {
      block = checker_.ctx_.make<BlockStmt>(loc);
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
    checker_.diags_.error(
        object->loc(),
        std::string("'for...in' over ") + what +
            " gives its indices as strings ('0', '1'...), so 'i + 1' would be '01'; walk "
            "the elements with 'for (const x of " +
            text + ")', or the indices with 'for (let i = 0; i < " + text + ".length; i++)'" +
            (type->isArray() ? " or '" + text + ".forEach((x, i) => ...)'" : ""));
  } else if (type->isOptional() && type->inner()->isObject()) {
    checker_.diags_.error(object->loc(),
                          "the object may be undefined; check it before walking its keys");
  } else if (type->nonOptional()->isJson()) {
    ok = true;
  } else if (type->isRecord()) {
    ok = true;
  } else if (type->isDict()) {
    checker_.diags_.error(
        object->loc(), std::string("'for...in' walks the properties of an object, and a ") +
                           (type->isMap() ? "Map" : "Set") + " has none in JavaScript; walk its " +
                           (type->isMap() ? "keys" : "values") + " with 'for (const x of " + text +
                           (type->isMap() ? ".keys()" : "") + ")'");
  } else if (type->isOptional() && type->inner()->isRecord()) {
    checker_.diags_.error(object->loc(),
                          "the object may be undefined; check it before walking its keys");
  } else if ((!type->isObject() || type->isRegExp() || type->isMatch() || type->isDate() ||
              type->isIntl() || type->isBytes() || type->isCrypto()) &&
             !type->isError()) {
    checker_.diags_.error(object->loc(),
                          "'for...in' walks the keys of an object, found '" + type->name() + "'");
  } else {
    ok = !type->isError();
  }
  llvm::SMLoc loc = object->loc();
  auto *callee =
      checker_.ctx_.make<MemberExpr>(loc, checker_.ctx_.make<NameExpr>(loc, "Object"), "keys");
  auto *keys = checker_.ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{object});
  keys->builtin = Builtin::ObjectKeys;
  keys->type = ok ? Type::array(Type::string()) : Type::error();
  keys->forIn = true;
  stmt->iterable = keys;
}

Expr *ObjectChecker::checkField(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isClass()) {
    return checker_.classMemberChecker().checkClassMember(member, nullptr);
  }
  const Field *field = type->field(member->name);
  if (!field) {
    checker_.diags_.error(member->loc(), noField(type, member->name));
    member->type = Type::error();
    return member;
  }
  member->type = field->type;
  if (Expr *narrowed = checker_.fieldFacts().narrowedField(member); narrowed != member) {
    return narrowed;
  }
  if (field->type->isOptional()) {
    if (std::optional<Checker::FieldPath> path = checker_.fieldFacts().fieldPath(member);
        path && checker_.fieldFacts().isFieldDefined(*path)) {
      return checker_.typeRelations().convert(member, field->type->inner());
    }
  }

  return member;
}

Expr *ObjectChecker::checkFieldCall(CallExpr *call, MemberExpr *member) {
  if (member->object->type->overload(member->name)) {
    return checker_.overloadResolver().checkOverloadCall(call, member);
  }
  call->callee = checkField(member);
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    return checker_.callChecker().checkValueCall(call, type);
  }
  if (type->isOptional() && type->inner()->isFunction()) {
    checker_.diags_.error(member->loc(),
                          "the function may be undefined; check it first, or call it with '?.()', "
                          "which gives undefined then");
  } else if (!type->isError()) {
    checker_.diags_.error(member->loc(), "field '" + member->name +
                                             "' is not a function; it has type '" + type->name() +
                                             "'");
  }
  for (Expr *&arg : call->args) {
    arg = checker_.exprChecker().checkExpr(arg);
  }
  call->type = Type::error();

  return call;
}

const Type *ObjectChecker::checkObjectFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isOptional()) {
    checker_.diags_.error(member->object->loc(),
                          "the value may be undefined; check it before assigning to '" +
                              member->name + "'");
    return Type::error();
  }
  if (type->isTuple() && member->name == "length") {
    checker_.diags_.error(member->loc(), "a tuple has a fixed length, which cannot be changed");
    return Type::error();
  }
  const Field *field = type->field(member->name);
  if (!field) {
    if (type->overload(member->name)) {
      checker_.diags_.error(member->loc(), "'" + member->name + "' is a method of '" +
                                               type->name() +
                                               "' with several signatures and cannot be replaced");
    } else {
      checker_.diags_.error(member->loc(), noField(type, member->name));
    }
    return Type::error();
  }
  if (field->readonly) {
    checker_.diags_.error(member->loc(), "cannot assign to '" + member->name +
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
