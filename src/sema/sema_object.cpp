// Objects: literals, fields, and the fields known to hold a value.
//
// Object types are exact: an object fits a type only with the same fields of
// the same types (see Type). A literal takes the type expected where it is
// written, `const p: Point = { x: 1, y: 2 }`, and then its values convert to
// the types of the fields, so x and y are doubles there; without an expected
// type the literal has the types of its values, `{ x: 1 }` is `{ x: int }`.
//
// A field of type `T | undefined` that a check showed to hold a value is
// narrowed as a variable is (`if (q.limit != undefined) use(q.limit)`), as
// long as nothing may have changed it: Sema forgets it on any assignment to a
// field of that name in any object, on an assignment to the variable it is
// read through, and on a call of a function if the program assigns to a field
// of that name anywhere (any code may run in the call).

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// What JavaScript objects get from Object.prototype, which tz objects do not
// have.
bool isInherited(llvm::StringRef name) {
  static const char *const names[] = {"toString",      "valueOf",        "hasOwnProperty",
                                      "isPrototypeOf", "toLocaleString", "propertyIsEnumerable",
                                      "constructor",   "__proto__"};
  return llvm::is_contained(names, name);
}

std::string noField(const Type *type, llvm::StringRef name) {
  std::string message = ("property '" + name + "' does not exist on type '" + type->name() + "'").str();
  if (isInherited(name))
    message += "; objects inherit nothing from Object.prototype";
  return message;
}

// The expression a conversion that narrows a variable or a field converts.
const Expr *unnarrowed(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit() && !conv->shortCircuit &&
                                              conv->operand->type->isOptional() &&
                                              conv->type == conv->operand->type->inner())
    return conv->operand;
  return expr;
}

} // namespace

Expr *Sema::checkObjectLit(ObjectLitExpr *lit, const Type *expected) {
  const Type *target = expected ? expected->nonOptional() : nullptr;
  if (target && (!target->isObject() || target->isIncomplete()))
    target = nullptr;
  bool ok = true;
  for (size_t i = 0; i < lit->props.size(); ++i)
    for (size_t j = 0; j < i; ++j)
      if (lit->props[j].name == lit->props[i].name) {
        diags_.error(lit->props[i].loc, "duplicate field '" + lit->props[i].name + "' in the object literal");
        ok = false;
        break;
      }

  std::vector<Field> fields;
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty())
      fe->fn->name = prop.name; // as JavaScript names it
    std::string what = ("field '" + prop.name + "'").str();
    if (target) {
      const Field *field = target->field(prop.name);
      if (!field) {
        prop.value = checkExpr(prop.value);
        diags_.error(prop.loc, "type '" + target->name() + "' has no field '" + prop.name +
                                   "'; an object must have exactly the fields of its type");
        continue;
      }
      prop.value = coerce(checkExpr(prop.value, field->type), field->type, what);
      continue;
    }
    prop.value = checkExpr(prop.value);
    const Type *type = prop.value->type;
    if (type->isError()) {
      ok = false;
    } else if (type->isVoid()) {
      diags_.error(prop.value->loc(), "an expression of type 'void' cannot be the value of " + what);
      ok = false;
    } else if (type->isUndefined()) {
      diags_.error(prop.value->loc(), "cannot infer the type of " + what +
                                          " from 'undefined'; declare the type of the object, e.g. "
                                          "'const o: { " + prop.name + "?: int } = ...'");
      ok = false;
    }
    fields.push_back({prop.name.str(), type, false});
  }

  if (target) {
    for (const Field &f : target->fields()) {
      bool given = llvm::any_of(lit->props, [&](const ObjectLitExpr::Property &p) { return p.name == f.name; });
      if (!given && !f.type->isOptional())
        diags_.error(lit->loc(), "field '" + f.name + "' is missing; type '" + target->name() + "' requires it");
    }
    lit->type = target;
    return lit;
  }
  lit->type = ok ? Type::object(std::move(fields)) : Type::error();
  return lit;
}

Expr *Sema::checkField(MemberExpr *member) {
  const Type *type = member->object->type;
  const Field *field = type->field(member->name);
  if (!field) {
    diags_.error(member->loc(), noField(type, member->name));
    member->type = Type::error();
    return member;
  }
  member->type = field->type;
  if (field->type->isOptional())
    if (std::optional<FieldPath> path = fieldPath(member); path && isFieldDefined(*path))
      return convert(member, field->type->inner());
  return member;
}

// `o.f(x)` calls the function in the field f; there are no methods yet.
Expr *Sema::checkFieldCall(CallExpr *call, MemberExpr *member) {
  call->callee = checkField(member);
  const Type *type = call->callee->type;
  if (type->isFunction())
    return checkValueCall(call, type);
  if (type->isOptional() && type->inner()->isFunction())
    diags_.error(member->loc(), "the function may be undefined; check it first, or call it with '?.()', "
                                "which gives undefined then");
  else if (!type->isError())
    diags_.error(member->loc(), "field '" + member->name + "' is not a function; it has type '" +
                                    type->name() + "'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  call->type = Type::error();
  return call;
}

const Type *Sema::checkFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isOptional()) {
    diags_.error(member->object->loc(), "the value may be undefined; check it before assigning to '" +
                                            member->name + "'");
    return Type::error();
  }
  const Field *field = type->field(member->name);
  if (!field) {
    diags_.error(member->loc(), noField(type, member->name));
    return Type::error();
  }
  if (field->readonly)
    diags_.error(member->loc(), "cannot assign to '" + member->name + "' because it is a read-only field of '" +
                                    type->name() + "'");
  member->type = field->type;
  return field->type;
}

std::string Sema::objectMismatch(const Type *from, const Type *to) const {
  for (const Field &f : from->fields())
    if (!to->field(f.name))
      return "; an object must have exactly the fields of its type, and this one has the extra field '" +
             f.name + "'";
  for (const Field &t : to->fields())
    if (!from->field(t.name))
      return "; it has no field '" + t.name + "'";
  for (const Field &f : from->fields()) {
    const Field *t = to->field(f.name);
    if (t->type != f.type) {
      std::string message = ("; field '" + f.name + "' is '" + f.type->name() + "' here and '" +
                             t->type->name() + "' in '" + to->name() + "'")
                                .str();
      // `const p = { x: 1 }` made x an int where a double is wanted.
      if (f.type->nonOptional()->isNumeric() && t->type->nonOptional()->isNumeric())
        message += ": give the object its type where it is created, e.g. 'const p: " + to->name().str() +
                   " = { ... }'";
      return message;
    }
    if (f.readonly && !t->readonly)
      return "; field '" + f.name + "' is read-only here, and '" + to->name().str() + "' would let it be changed";
  }
  return "";
}

//===----------------------------------------------------------------------===//
// Fields that hold a value
//===----------------------------------------------------------------------===//

std::optional<Sema::FieldPath> Sema::fieldPath(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(unnarrowed(expr));
  if (!member || member->optional || !member->object->type || !member->object->type->isObject())
    return std::nullopt;
  const Expr *object = unnarrowed(member->object);
  FieldPath path;
  if (auto *name = dyn_cast<NameExpr>(object)) {
    if (!name->var)
      return std::nullopt;
    path.var = name->var;
  } else if (std::optional<FieldPath> outer = fieldPath(object)) {
    path = std::move(*outer);
  } else {
    return std::nullopt;
  }
  path.names.push_back(member->name);
  return path;
}

bool Sema::isFieldDefined(const FieldPath &path) const {
  return llvm::is_contained(flow_.definedFields, path);
}

void Sema::defineField(Flow &flow, const FieldPath &path) {
  if (!llvm::is_contained(flow.definedFields, path))
    flow.definedFields.push_back(path);
}

void Sema::fieldsAssigned(llvm::StringRef name) {
  // Any object with a field of this name may be the one assigned to.
  llvm::erase_if(flow_.definedFields,
                 [&](const FieldPath &path) { return llvm::is_contained(path.names, name); });
}

void Sema::forgetFieldsOf(Flow &flow, const VarSymbol *var) {
  llvm::erase_if(flow.definedFields, [&](const FieldPath &path) { return path.var == var; });
}

} // namespace tz
