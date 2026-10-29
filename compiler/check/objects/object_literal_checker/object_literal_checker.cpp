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
    named[i] = !prop.spread;
    if (prop.defaulted) {
      checker_.diags_.error(prop.loc,
                            "'" + prop.name +
                                " = ...' gives a default value, which only a destructuring "
                                "pattern such as '({ " +
                                prop.name + " = 1 } = o)' has; in an object literal write '" +
                                prop.name + ": ...'");
      prop.value = cast<AssignExpr>(prop.value)->value;
      prop.defaulted = false;
      ok = false;
    }
    if (!prop.key) {
      continue;
    }
    prop.key = checker_.exprChecker().checkExpr(prop.key);
    const Type *keyType = prop.key->type;
    if (std::optional<std::string> name = checker_.keyedFieldChecker().constantKey(prop.key)) {
      prop.name = checker_.ctx_.keepName(std::move(*name));
      checker_.file_->fieldNames.insert(prop.name);
      continue;
    }
    ok = false;
    named[i] = false;
    if (keyType->isError()) {
      continue;
    }
    if (keyType->nonOptional()->isNumeric()) {
      checker_.diags_.error(
          prop.key->loc(),
          "a number cannot name a field; an object with numbered entries is an array");
    } else if (keyType->isString()) {
      checker_.diags_.error(
          prop.key->loc(),
          std::string("a key in brackets must be known when compiling: a string "
                      "literal, or a constant declared with one, such as 'const key = "
                      "\"name\"'; ") +
              kRuntimeKeys);
    } else {
      checker_.diags_.error(prop.key->loc(),
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
      if (const Field *wanted = prop.method ? target->field(prop.name) : nullptr;
          wanted && wanted->type->nonOptional()->isFunction() &&
          wanted->type->nonOptional()->hasThis()) {
        prop.method = false;
      }
    }
  }
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.method; })) {
    return checker_.methodLiteralChecker().checkMethodLiteral(lit, target);
  }
  if (lit->hasSpread()) {
    return checkSpreadLit(lit, target, named, ok);
  }

  std::vector<Field> fields;
  for (size_t i = 0; i < lit->props.size(); ++i) {
    ObjectLitExpr::Property &prop = lit->props[i];
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
          checker_.diags_.error(prop.loc,
                                "type '" + target->name() + "' has no field '" + prop.name +
                                    "'; an object must have exactly the fields of its type");
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
    if (!prop.spread) {
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
    for (const IndexExpr *read : prop.omitKeys) {
      if (!read->field.empty()) {
        prop.omit.push_back(read->field);
      }
    }
    prop.omitKeys.clear();
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
      if (llvm::is_contained(prop.omit, f.name)) {
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
        if (!later.spread) {
          if (later.name == name) {
            return true;
          }
        } else if (!llvm::is_contained(later.omit, name)) {
          if (const Field *f = later.value->type->field(name); f && !f->type->isOptional()) {
            return true;
          }
        }
      }

      return false;
    };
    if (!prop.spread) {
      if (replacedLater(prop.name)) {
        prop.replaced = true;
      } else {
        prop.value = checker_.typeRelations().convert(prop.value, result->field(prop.name)->type);
      }
      continue;
    }
    for (const Field &f : prop.value->type->fields()) {
      if (!llvm::is_contained(prop.omit, f.name) && replacedLater(f.name)) {
        prop.replacedFields.push_back(f.name);
      }
    }
  }
  lit->type = result;

  return lit;
}

}
