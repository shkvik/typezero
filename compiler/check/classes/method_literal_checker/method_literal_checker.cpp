#include "compiler/check/classes/method_literal_checker/method_literal_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/infer/call_cycles/call_cycles.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

MethodLiteralChecker::MethodLiteralChecker(Sema &sema) : sema_(sema) {}

Expr *MethodLiteralChecker::checkMethodLiteral(ObjectLitExpr *lit, const Type *target) {
  lit->type = Type::error();
  if (target && target->isAnyObject()) {
    target = nullptr;
  }
  if (lit->hasSpread()) {
    sema_.diags_.error(lit->loc(), "an object literal with methods cannot take the fields of another "
                             "object with '...'; write them out");
    for (ObjectLitExpr::Property &prop : lit->props) {
      prop.value = sema_.exprChecker().checkExpr(prop.value);
    }
    return lit;
  }
  auto *cls = sema_.ctx_.make<ClassDecl>(lit->loc(), "");
  cls->literal = lit;
  cls->id = ++sema_.classCount_;
  lit->cls = cls;
  Type *type = Type::classType("", cls);
  cls->type = type;
  cls->resolved = cls->declared = true;
  sema_.classes_.push_back(cls);

  // The fields have the types of their values, in the order written.
  bool ok = true;
  std::vector<Field> fields;
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method) {
      FunctionDecl *fn = cast<FunctionExpr>(prop.value)->fn;
      fn->name = prop.name;
      fn->cls = cls;
      if (!fn->typeParams.empty()) {
        sema_.diags_.error(fn->loc(), "a method of an object literal cannot be generic; declare a class, "
                                "whose methods can");
        fn->typeParams.clear();
      }
      cls->methods.push_back(fn);
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty()) {
      fe->fn->name = prop.name;
    }
    std::string what = ("field '" + prop.name + "'").str();
    // The interface expected gives the type, as for a literal without methods.
    const Field *wanted = target ? target->field(prop.name) : nullptr;
    prop.value = sema_.exprChecker().checkExpr(prop.value, wanted ? wanted->type : nullptr);
    if (wanted) {
      prop.value = sema_.typeRelations().coerce(prop.value, wanted->type, what);
    }
    const Type *valueType =
        wanted && !prop.value->type->isError() ? wanted->type : prop.value->type;
    if (valueType->isVoid()) {
      sema_.diags_.error(prop.value->loc(),
                   "an expression of type 'void' cannot be the value of " + what);
      ok = false;
    } else if (valueType->isUndefined()) {
      sema_.diags_.error(prop.value->loc(), "cannot infer the type of " + what + " from 'undefined'");
      ok = false;
    }
    ok &= !valueType->isError();
    ClassField field;
    field.name = prop.name;
    field.loc = prop.loc;
    field.resolved = valueType;
    cls->fields.push_back(field);
    fields.push_back({prop.name.str(), valueType, false});
  }
  if (!ok) {
    return lit;
  }
  type->defineFields(fields);
  std::string name = objectTypeSpelling(fields);
  std::string methods;
  for (FunctionDecl *fn : cls->methods) {
    methods += (methods.empty() ? "" : "; ") + fn->name.str() + "()";
  }
  name = fields.empty() ? "{ " + methods + " }"
                        : name.substr(0, name.size() - 2) + "; " + methods + " }";
  type->rename(name);

  // The methods share one environment, the object: created with it, they
  // see what is visible where it is written.
  NestedGroup *group = sema_.ctx_.newGroup();
  group->owner = sema_.currentDecl();
  group->scopeDepth = sema_.scopes_.size();
  group->functionDepth = sema_.functions_.size();
  group->creationSeq = ++sema_.seq_;
  group->finished = true;
  group->members = cls->methods;
  cls->group = group;
  for (FunctionDecl *fn : cls->methods) {
    fn->group = group;
    fn->parent = sema_.currentDecl();
    fn->creationSeq = group->creationSeq;
    // A method of the interface expected gives the types of the parameters
    // and the result, as it gives them to an arrow.
    const Field *wanted = target ? target->field(fn->name) : nullptr;
    if (wanted && wanted->type->isFunction()) {
      sema_.classDeclarer().declareTyped(fn, wanted->type);
      if (fn->returnType) {
        fn->sym->ret = sema_.predicateChecker().resolveReturnType(fn, fn->sym);
      }
    } else {
      sema_.functionChecker().declareFunction(fn, nullptr, /*scoped=*/false);
    }
  }
  sema_.callCycles().findCycles(cls->methods,
             [](FunctionDecl *, llvm::StringRef) -> FunctionDecl * { return nullptr; });
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->sym->ret) {
      sema_.returnInferrer().inferReturnType(fn->sym);
    }
    if (fn->checked) {
      continue;
    }
    Sema::BodyState saved = sema_.returnInferrer().enterBody(fn);
    sema_.functionChecker().checkFunctionBody(fn);
    sema_.returnInferrer().leaveBody(saved);
  }
  lit->type = type;
  if (target && implementsLiteral(lit, target)) {
    sema_.classBases().addBase(type, target);
  }

  return lit;
}

// Whether the literal has exactly the members of interface `target`: fields
// of the same types, and methods where it has functions it cannot replace.
// Reports the first difference.
bool MethodLiteralChecker::implementsLiteral(ObjectLitExpr *lit, const Type *target) {
  const ClassDecl *cls = lit->cls;
  auto fail = [&](llvm::SMLoc loc, const std::string &why) {
    sema_.diags_.error(loc, "the object literal does not fit '" + target->name() + "': " + why);
    lit->type = Type::error();

    return false;
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    const Field *member = target->field(prop.name);
    if (!member) {
      return fail(prop.loc, "it has no field '" + prop.name.str() +
                                "'; an object must have exactly the fields of its type");
    }
    if (!prop.method) {
      continue;
    }
    FunctionDecl *fn = cast<FunctionExpr>(prop.value)->fn;
    if (!member->type->isFunction()) {
      return fail(prop.loc, "'" + prop.name.str() + "' is a field of type '" +
                                member->type->name().str() + "' there, not a method");
    }
    if (!member->readonly) {
      return fail(prop.loc, "'" + prop.name.str() +
                                "' is a field there that code may replace, "
                                "which a method cannot be; write it as a field: '" +
                                prop.name.str() + ": (...) => ...'");
    }
    const Type *type = sema_.returnInferrer().functionType(fn->sym);
    if (type != member->type && !type->isError()) {
      return fail(prop.loc, "'" + prop.name.str() + "' is '" + type->name().str() + "' here and '" +
                                member->type->name().str() + "' there");
    }
  }
  for (const Field &member : target->fields()) {
    if (!cls->field(member.name) && !llvm::any_of(cls->methods, [&](const FunctionDecl *fn) {
          return fn->name == member.name;
        })) {
      return fail(lit->loc(), "it has no '" + member.name + "'");
    }
  }
  if (!target->overloads().empty()) {
    return fail(lit->loc(), "its method '" + target->overloads()[0].name +
                                "' has several signatures, and only a class implements those, "
                                "each with a variant of its own");
  }

  return true;
}

} // namespace tz
