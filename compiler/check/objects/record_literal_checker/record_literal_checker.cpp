#include "compiler/check/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

RecordLiteralChecker::RecordLiteralChecker(Checker &checker) : checker_(checker) {}

Expr *RecordLiteralChecker::checkRecordLit(ObjectLitExpr *lit, const Type *record) {
  const Type *value = record->valueType();
  bool ok = true;
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.defaulted) {
      checker_.diags_.error(prop.loc, "'" + prop.name +
                                          " = ...' gives a default value, which only a "
                                          "destructuring pattern has; in an object literal "
                                          "write '" +
                                          prop.name + ": ...'");
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.defaulted = false;
      ok = false;
    }
    if (prop.method) {
      checker_.diags_.error(prop.loc, "a Record holds values, not methods; store a function, '" +
                                          prop.name + ": (...) => ...'");
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      ok = false;
      continue;
    }
    if (prop.spread) {
      prop.value = checker_.exprChecker().checkExpr(prop.value, record);
      const Type *type = prop.value->type;
      if (type->isError()) {
        ok = false;
        continue;
      }
      if (!checkRecordSpread(prop, value)) {
        ok = false;
      }
      continue;
    }
    if (prop.key) {
      prop.key = checker_.dictChecker().checkRecordKey(prop.key);
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    prop.value =
        checker_.typeRelations().coerce(checker_.exprChecker().checkExpr(prop.value, value), value,
                                        prop.key ? "the value" : ("key '" + prop.name + "'").str());
  }
  lit->type = ok ? record : Type::error();

  return lit;
}

bool RecordLiteralChecker::checkRecordSpread(ObjectLitExpr::Property &prop, const Type *value) {
  const Type *type = prop.value->type;
  if (type->isOptional() && (type->inner()->isRecord() || type->inner()->isObject())) {
    checker_.diags_.error(prop.value->loc(),
                          "the object may be undefined; check it before spreading it");
    return false;
  }
  if (!prop.omitKeys.empty()) {
    checker_.diags_.error(prop.loc,
                          "the rest of a pattern that reads a key in brackets is not supported "
                          "for a Record; name the keys it leaves out");
    return false;
  }
  if (type->isRecord()) {
    if (!TypeRelations::isImplicitlyConvertible(type->valueType(), value)) {
      checker_.diags_.error(prop.value->loc(),
                            "the values of '" + type->name() + "' are not '" + value->name() + "'");
      return false;
    }
    return true;
  }
  if (type->isObject() && !type->isTuple() && !type->isRegExp() && !type->isMatch() &&
      !type->isAnyObject()) {
    if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
      checker_.classDeclarer().completeClass(cls);
    }
    for (const Field &f : type->fields()) {
      if (llvm::StringRef(f.name).starts_with("#") || llvm::is_contained(prop.omit, f.name)) {
        continue;
      }
      const Type *t = f.type->isOptional() && !value->isOptional() ? f.type->inner() : f.type;
      if (!TypeRelations::isImplicitlyConvertible(t, value)) {
        checker_.diags_.error(prop.value->loc(), "field '" + f.name + "' of '" + type->name() +
                                                     "' is '" + f.type->name() +
                                                     "', and the values here are '" +
                                                     value->name() + "'");
        return false;
      }
    }
    // Methods are no own properties of an instance in JavaScript, but the
    // methods of a literal and arrows in fields are.
    bool methods = false;
    for (const ClassDecl *c = ClassDeclarer::classOf(type); c; c = c->parent) {
      methods |=
          c->literal || llvm::any_of(c->fields, [](const ClassField &f) { return f.method; });
    }
    if (methods) {
      checker_.diags_.error(prop.value->loc(),
                            "an object with methods cannot be spread into a Record; copy "
                            "its fields by name");
      return false;
    }
    return true;
  }
  std::string message =
      "only objects and Records can be spread into a Record, found '" + type->name().str() + "'";
  if (type->isMap() || type->isSet()) {
    message += "; in JavaScript a Map or a Set has no own properties, so this would add nothing";
  }
  checker_.diags_.error(prop.value->loc(), message);

  return false;
}

Expr *RecordLiteralChecker::recordFromSpreads(ObjectLitExpr *lit) {
  // The type of the values: of every named value, of the values of the
  // Records spread and of the fields of the objects spread.
  const Type *value = nullptr;
  bool ok = true;
  auto meet = [&](const Type *t, llvm::SMLoc loc) {
    if (!t || t->isError()) {
      ok = false;
      return;
    }
    const Type *next = value ? TypeRelations::unify(value, t) : t;
    if (!next || next->isError()) {
      checker_.diags_.error(
          loc, "spreading a Record makes a Record, whose values have one type; found '" +
                   value->name() + "' and '" + t->name() +
                   "'; declare its type, e.g. 'const r: Record<string, " + value->name() +
                   "> = { ... }'");
      ok = false;
      return;
    }
    value = next;
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    const Type *type = prop.value->type;
    if (!prop.spread) {
      meet(type, prop.value->loc());
      continue;
    }
    if (type->isRecord()) {
      meet(type->valueType(), prop.value->loc());
    } else if (type->isObject()) {
      for (const Field &f : type->fields()) {
        if (!llvm::StringRef(f.name).starts_with("#") && !llvm::is_contained(prop.omit, f.name)) {
          meet(f.type->isOptional() ? f.type->inner() : f.type, prop.value->loc());
        }
      }
    }
  }
  if (!ok || !value) {
    lit->type = Type::error();
    return lit;
  }
  if (value->isUndefined() || value->isVoid()) {
    checker_.diags_.error(lit->loc(),
                          "cannot infer the type of the values from 'undefined'; declare it, "
                          "e.g. 'const r: Record<string, int | undefined> = { ... }'");
    lit->type = Type::error();
    return lit;
  }
  const Type *record = Type::dict(Type::DictKind::Record, Type::string(), value);
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread) {
      prop.value = checker_.typeRelations().convert(prop.value, value);
      continue;
    }
    if (!checkRecordSpread(prop, value)) {
      ok = false;
    }
  }
  lit->type = ok ? record : Type::error();

  return lit;
}

} // namespace tz
