#include "compiler/check/objects/object_literal_checker/object_literal_checker.h"
#include "compiler/check/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/objects/record_literal_checker/record_literal_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/json/json_checker/json_checker.h"
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

const char kRuntimeKeys[] =
    "keys computed when the program runs need a dictionary: declare it as one, e.g. "
    "'const d: Record<string, int> = {}'";

}

ObjectLiteralChecker::ObjectLiteralChecker(Checker &checker) : checker_(checker) {}

Expr *ObjectLiteralChecker::checkObjectLit(ObjectLitExpr *lit, const Type *expected) {
  if (expected && expected->nonOptional()->isJson()) {
    return checker_.jsonChecker().checkJsonObjectLit(lit);
  }
  if (expected && expected->nonOptional()->isRecord()) {
    return checker_.recordLiteralChecker().checkRecordLit(lit, expected->nonOptional());
  }
  const Type *target = expected ? expected->nonOptional() : nullptr;
  const Type *expectedUnion = target && target->isUnion() ? target : nullptr;
  auto usable = [](const Type *t) {
    return t && t->isObject() && !t->isIncomplete() && !t->isTuple() && !t->isClass() &&
           !t->isRegExp() && !t->isMatch() && !t->isDate() && !t->isIntl() && !t->isBytes() &&
           !t->isCrypto() && !t->isAnyObject();
  };
  if (!usable(target)) {
    target = nullptr;
  }
  bool ok = true;

  llvm::SmallVector<bool, 8> named(lit->props.size(), true);
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    named[i] = !prop.spread();
    if (prop.defaulted()) {
      checker_.diags_.error(prop.loc,
                            "'" + prop.name +
                                " = ...' gives a default value, which only a destructuring "
                                "pattern such as '({ " +
                                prop.name + " = 1 } = o)' has; in an object literal write '" +
                                prop.name + ": ...'");
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.form = ValueProperty{};
      ok = false;
    }
    ComputedProperty *computed = prop.form.as<ComputedProperty>();
    if (!computed) {
      continue;
    }
    computed->key = checker_.exprChecker().checkExpr(computed->key);
    const Type *keyType = prop.key()->type;
    if (std::optional<std::string> name = checker_.keyedFieldChecker().constantKey(prop.key())) {
      prop.name = checker_.ctx_.keepName(std::move(*name));
      checker_.file_->fieldNames.insert(prop.name);
      continue;
    }
    named[i] = false;
    llvm::ArrayRef<const Type *> keyNames =
        keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType);
    if (llvm::all_of(keyNames, [](const Type *n) { return n->isStringLiteral(); })) {
      computed->keyed = true;
      continue;
    }
    ok = false;
    if (keyType->isError()) {
      continue;
    }
    if (keyType->nonOptional()->isNumeric()) {
      checker_.diags_.error(
          prop.key()->loc(),
          "a number cannot name a field; an object with numbered entries is an array");
    } else if (keyType->isString()) {
      checker_.diags_.error(
          prop.key()->loc(),
          std::string("a key in brackets must be known when compiling: a string "
                      "literal, or a constant declared with one, such as 'const key = "
                      "\"name\"'; ") +
              kRuntimeKeys);
    } else {
      checker_.diags_.error(prop.key()->loc(),
                            "a key must be a string, found '" + keyType->name() + "'");
    }
  }

  for (size_t i = 0; i < lit->props.size(); ++i) {
    for (size_t j = 0; j < i && named[i]; ++j) {
      if (named[j] && lit->props[j].name == lit->props[i].name) {
        checker_.diags_.error(lit->props[i].loc,
                              "duplicate field '" + lit->props[i].name + "' in the object literal");
        ok = false;
        break;
      }
    }
  }
  if (expectedUnion) {
    target = checker_.literalTypes().literalMember(lit, expectedUnion);
    if (!usable(target)) {
      target = nullptr;
    }
    if (!target) {
      for (const Type *m : expectedUnion->members()) {
        if (m->isRecord()) {
          return checker_.recordLiteralChecker().checkRecordLit(lit, m);
        }
      }
    }
  }
  if (target) {
    for (ObjectLitExpr::Property &prop : lit->props) {
      if (const Field *wanted = prop.method() ? target->field(prop.name) : nullptr;
          wanted && wanted->type->nonOptional()->isFunction() &&
          wanted->type->nonOptional()->hasThis()) {
        prop.form = ValueProperty{};
      }
    }
  }
  bool inferred = false;
  if (!target && !lit->props.empty() &&
      llvm::all_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.keyed(); })) {
    std::vector<Field> fields;
    for (ObjectLitExpr::Property &prop : lit->props) {
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      const Type *value = prop.value->type;
      if (value->isError()) {
        lit->type = Type::error();
        return lit;
      }
      if (value->isVoid() || value->isUndefined()) {
        checker_.diags_.error(prop.value->loc(),
                              "cannot infer the type of the fields from '" + value->name() +
                                  "'; declare the type of the object, e.g. 'const o: { a?: int "
                                  "} = ...'");
        lit->type = Type::error();
        return lit;
      }
      const Type *keyType = prop.key()->type;
      for (const Type *n :
           keyType->isUnion() ? keyType->members() : llvm::ArrayRef<const Type *>(keyType)) {
        std::string name;
        llvm::convertUTF16ToUTF8String(
            llvm::ArrayRef<llvm::UTF16>(
                reinterpret_cast<const llvm::UTF16 *>(n->literalText().data()),
                n->literalText().size()),
            name);
        auto same = llvm::find_if(fields, [&](const Field &f) { return f.name == name; });
        if (same == fields.end()) {
          fields.push_back({name, Type::optional(value), false});
        } else if (same->type != Type::optional(value)) {
          checker_.diags_.error(prop.key()->loc(), "field '" + name +
                                                       "' gets values of different types; declare "
                                                       "the type of the object");
          lit->type = Type::error();
          return lit;
        }
      }
    }
    target = Type::object(std::move(fields));
    inferred = true;
  }
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.keyed()) {
      continue;
    }
    const Type *keyType = prop.key()->type;
    if (lit->hasSpread() ||
        llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method(); })) {
      checker_.diags_.error(prop.key()->loc(),
                            "a key of type '" + keyType->name() +
                                "' in brackets works in an object literal without '...' "
                                "and methods; assign it after: 'o[key] = value'");
      return lit;
    }
    if (!target) {
      checker_.diags_.error(prop.key()->loc(),
                            "a key of type '" + keyType->name() +
                                "' in brackets needs the type of the object, which says "
                                "the fields it may name; write it, e.g. 'const o: T = { [key]: "
                                "value }'");
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      lit->type = Type::error();
      return lit;
    }
    const Type *field = KeyedFieldChecker::keyedFieldType(target, keyType);
    if (!field) {
      checker_.diags_.error(prop.key()->loc(), "the fields that '" + keyType->name() +
                                                   "' names must all be in '" + target->name() +
                                                   "' and have one type");
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      lit->type = Type::error();
      return lit;
    }
    if (!field->isOptional()) {
      checker_.diags_.error(prop.key()->loc(),
                            "the fields that '" + keyType->name() + "' names are of type '" +
                                field->name() +
                                "', not optional; the object gets only one of them, so the "
                                "others would be missing");
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      lit->type = Type::error();
      return lit;
    }
    std::string what = "the field that '" + keyType->name().str() + "' names";
    prop.value = checker_.typeRelations().coerce(
        inferred ? prop.value : checker_.exprChecker().checkExpr(prop.value, field), field, what);
  }
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method(); })) {
    return checker_.methodLiteralChecker().checkMethodLiteral(lit, target);
  }
  if (lit->hasSpread()) {
    return checkSpreadLit(lit, target, named, ok);
  }

  std::vector<Field> fields;
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (prop.keyed()) {
      continue;
    }
    if (!named[i]) {
      prop.value = checker_.exprChecker().checkExpr(prop.value);
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    std::string what = ("field '" + prop.name + "'").str();
    if (target) {
      const Field *field = target->field(prop.name);
      if (!field) {
        if (!checker_.prechecked_.erase(prop.value)) {
          prop.value = checker_.exprChecker().checkExpr(prop.value);
        }
        if (!target->overload(prop.name)) {
          std::string message = "type '" + target->name().str() + "' has no field '" +
                                prop.name.str() +
                                "'; an object must have exactly the fields of its type";
          if (target->name() == "StreamError") {
            message += "; an error with fields of its own has a type of its own, e.g. 'interface "
                       "MyError extends StreamError { ... }', and the stream names it: "
                       "'new ReadableStream<int, MyError>(...)', 'class Lines extends "
                       "Readable<string, MyError>'";
          }
          checker_.diags_.error(prop.loc, message);
        }
        continue;
      }
      Expr *value = checker_.prechecked_.erase(prop.value)
                        ? prop.value
                        : checker_.exprChecker().checkExpr(prop.value, field->type);
      prop.value = checker_.typeRelations().coerce(value, field->type, what);
      continue;
    }
    if (!checker_.prechecked_.erase(prop.value)) {
      prop.value = checker_.exprChecker().checkExpr(prop.value);
    }
    const Type *type = prop.value->type;
    if (type->isError()) {
      ok = false;
    } else if (type->isVoid()) {
      checker_.diags_.error(prop.value->loc(),
                            "an expression of type 'void' cannot be the value of " + what);
      ok = false;
    } else if (type->isUndefined()) {
      checker_.diags_.error(prop.value->loc(),
                            "cannot infer the type of " + what +
                                " from 'undefined'; declare the type of the object, e.g. "
                                "'const o: { " +
                                prop.name + "?: int } = ...'");
      ok = false;
    }
    fields.push_back({prop.name.str(), type, false});
  }

  if (target) {
    for (const Field &f : target->fields()) {
      bool given = llvm::any_of(lit->props,
                                [&](const ObjectLitExpr::Property &p) { return p.name == f.name; });
      if (!given && !f.type->isOptional()) {
        checker_.diags_.error(lit->loc(), "field '" + f.name + "' is missing; type '" +
                                              target->name() + "' requires it");
      }
    }
    for (const Overload &o : target->overloads()) {
      checker_.diags_.error(lit->loc(), "an object literal cannot be of type '" + target->name() +
                                            "': its method '" + o.name +
                                            "' has several signatures, and only a class implements "
                                            "those, each with a variant of its own");
    }
    lit->type = target;
    return lit;
  }
  lit->type = ok ? Type::object(std::move(fields)) : Type::error();

  return lit;
}

Expr *ObjectLiteralChecker::checkSpreadLit(ObjectLitExpr *lit, const Type *target,
                                           llvm::ArrayRef<bool> named, bool ok) {
  bool records = false;
  std::vector<Field> fields;
  auto slot = [&](llvm::StringRef name) -> Field * {
    for (Field &f : fields) {
      if (f.name == name) {
        return &f;
      }
    }

    return nullptr;
  };
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    if (!prop.spread()) {
      if (!named[i]) {
        prop.value = checker_.exprChecker().checkExpr(prop.value);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
        fe->fn->name = prop.name;
      }
      const Field *wanted = target ? target->field(prop.name) : nullptr;
      prop.value = checker_.exprChecker().checkExpr(prop.value, wanted ? wanted->type : nullptr);
      const Type *type = prop.value->type;
      Field *field = slot(prop.name);
      std::string what = ("field '" + prop.name + "'").str();
      if (type->isError()) {
        ok = false;
        continue;
      }
      if (type->isVoid()) {
        checker_.diags_.error(prop.value->loc(),
                              "an expression of type 'void' cannot be the value of " + what);
        ok = false;
        continue;
      }
      if (type->isUndefined()) {
        if (!field) {
          checker_.diags_.error(prop.value->loc(),
                                "cannot infer the type of " + what +
                                    " from 'undefined'; declare the type of the object, e.g. "
                                    "'const o: { " +
                                    prop.name + "?: int } = ...'");
          ok = false;
          continue;
        }
        type = Type::optional(field->type);
      }
      if (field) {
        field->type = type;
      } else {
        fields.push_back({prop.name.str(), type, false});
      }
      continue;
    }

    prop.value = checker_.exprChecker().checkExpr(prop.value);
    const Type *type = prop.value->type;
    if (type->isError()) {
      ok = false;
      continue;
    }
    if (type->isRecord() && !target) {
      records = true;
      continue;
    }
    if (type->isOptional() && type->inner()->isObject() && !type->inner()->isTuple()) {
      checker_.diags_.error(prop.value->loc(),
                            "the object may be undefined; check it before spreading it");
      ok = false;
      continue;
    }
    if (!type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
        type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto()) {
      std::string message =
          "only objects can be spread into an object literal, found '" + type->name().str() + "'";
      if (type->nonOptional()->isArray() || type->nonOptional()->isTuple() || type->isString()) {
        message += "; JavaScript would make its indices the keys";
      }
      checker_.diags_.error(prop.value->loc(), message);
      ok = false;
      continue;
    }
    SpreadProperty *spread = prop.form.as<SpreadProperty>();
    for (const IndexExpr *read : spread->omitKeys) {
      if (!read->field().empty()) {
        spread->omit.push_back(read->field());
      }
    }
    spread->omitKeys.clear();
    if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
      checker_.classDeclarer().completeClass(cls);
      if (hasMethodFields(cls)) {
        checker_.diags_.error(prop.value->loc(),
                              "an object with methods cannot be spread into an object "
                              "literal yet; copy the fields by name");
        ok = false;
        continue;
      }
    }
    for (const Field &f : type->fields()) {
      if (llvm::is_contained(prop.omit(), f.name)) {
        continue;
      }
      Field *field = slot(f.name);
      if (!field) {
        fields.push_back({f.name, f.type, false});
        continue;
      }
      if (!f.type->isOptional()) {
        field->type = f.type;
        continue;
      }
      const Type *base = TypeRelations::unify(field->type->nonOptional(), f.type->inner());
      if (!base || base->isError()) {
        checker_.diags_.error(prop.loc, "field '" + f.name + "' is '" + field->type->name() +
                                            "' before the spread and '" + f.type->name() +
                                            "' in '" + type->name() + "'; a field has one type");
        ok = false;
        continue;
      }
      field->type = field->type->isOptional() ? Type::optional(base) : base;
    }
  }
  if (records && ok) {
    return checker_.recordLiteralChecker().recordFromSpreads(lit);
  }
  if (!ok) {
    lit->type = Type::error();
    return lit;
  }

  const Type *result = Type::object(fields);
  if (target) {
    bool fits = llvm::all_of(fields, [&](const Field &f) {
      const Field *t = target->field(f.name);

      return t && TypeRelations::isImplicitlyConvertible(f.type, t->type);
    });
    fits &= llvm::all_of(target->fields(),
                         [&](const Field &t) { return t.type->isOptional() || slot(t.name); });
    if (fits) {
      result = target;
    }
  }

  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
    auto replacedLater = [&](llvm::StringRef name) {
      for (size_t j = i + 1; j < lit->props.size(); ++j) {
        const ObjectLitExpr::Property &later = lit->props[j];
        if (!later.spread()) {
          if (later.name == name) {
            return true;
          }
        } else if (!llvm::is_contained(later.omit(), name)) {
          if (const Field *f = later.value->type->field(name); f && !f->type->isOptional()) {
            return true;
          }
        }
      }

      return false;
    };
    if (!prop.spread()) {
      if (replacedLater(prop.name)) {
        prop.replaced = true;
      } else {
        prop.value = checker_.typeRelations().convert(prop.value, result->field(prop.name)->type);
      }
      continue;
    }
    SpreadProperty *spread = prop.form.as<SpreadProperty>();
    for (const Field &f : prop.value->type->fields()) {
      if (!llvm::is_contained(spread->omit, f.name) && replacedLater(f.name)) {
        spread->replacedFields.push_back(f.name);
      }
    }
  }
  lit->type = result;

  return lit;
}

}
