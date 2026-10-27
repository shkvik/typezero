#include "compiler/check/expr/target_checker/target_checker.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/member_checker/member_checker.h"
#include "compiler/check/expr/name_checker/name_checker.h"
#include "compiler/check/functions/nested_functions/nested_functions.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

TargetChecker::TargetChecker(Checker &checker) : checker_(checker) {}

const Type *TargetChecker::checkAssignTarget(Expr *&target, bool compound) {
  if (auto *name = dyn_cast<NameExpr>(target)) {
    VarSymbol *var = checkVarTarget(name);
    return var ? var->type : Type::error();
  }

  if (auto *index = dyn_cast<IndexExpr>(target)) {
    index->object = checker_.exprChecker().checkExpr(index->object);
    const Type *type = index->object->type;
    index->type = Type::error();
    if (checker_.utilityTypes().checkGenericIndex(index)) {
      return index->type;
    }
    if (type->nonOptional()->isJson()) {
      return checker_.jsonChecker().checkJsonTarget(index);
    }
    if (type->isDict()) {
      return checker_.dictChecker().checkDictTarget(index, compound);
    }
    if (type->isTypedArray()) {
      index->index = checker_.memberChecker().checkPosition(index->index);
      index->type =
          type->bytes() == Type::Bytes::Uint8Clamped ? Type::intTy() : type->elementType();
      return index->type;
    }
    if (type->isBytes()) {
      index->index = checker_.exprChecker().checkExpr(index->index);
      checker_.diags_.error(index->loc(),
                            "type '" + type->name() + "' cannot be indexed; " +
                                (type->bytes() == Type::Bytes::DataView
                                     ? "write its bytes with setUint8(i, value) and the like"
                                     : "write its bytes through a view, e.g. 'new "
                                       "Uint8Array(buffer)[i] = value'"));
      return Type::error();
    }
    if (type->isObject()) {
      Expr *checked = checker_.keyedFieldChecker().checkKeyedField(index, true);
      if (index->keyed) {
        return index->type;
      }
      auto *member = dyn_cast<MemberExpr>(checked);
      if (!member) {
        return Type::error();
      }
      target = member;
      return checker_.classMemberChecker().checkFieldTarget(member, compound);
    }
    if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
      index->index = checker_.exprChecker().checkExpr(index->index);
      checker_.diags_.error(index->object->loc(),
                            "the value may be undefined; check it before assigning to it");
      return Type::error();
    }
    index->index = checker_.memberChecker().checkPosition(index->index);
    if (type->isError()) {
      return type;
    }
    if (type->isArray()) {
      index->type = type->inner();
      return index->type;
    }
    if (type->isString()) {
      checker_.diags_.error(index->loc(),
                            "strings are immutable; a character cannot be assigned to");
    } else if (type->isOptional() && type->inner()->isArray()) {
      checker_.diags_.error(index->object->loc(), "the array may be undefined; check it first");
    } else {
      checker_.diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    }
    return Type::error();
  }

  if (auto *member = dyn_cast<MemberExpr>(target)) {
    auto *name = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = name ? checker_.scopes().lookup(name->name).cls : nullptr) {
      return checker_.staticChecker().checkStaticTarget(target, cls);
    }
    if (name && name->name == "this") {
      checker_.thisMember_ = member->name;
    }
    member->object = checker_.exprChecker().checkExpr(member->object);
    checker_.thisMember_ = {};
    const Type *type = member->object->type;
    member->type = Type::error();
    if (type->isError()) {
      return type;
    }
    if (type->nonOptional()->isJson()) {
      return checker_.jsonChecker().checkJsonTarget(member);
    }
    if (ClassDecl *cls = const_cast<ClassDecl *>(type->classRef());
        cls && cls->type && isa<NameExpr>(member->object)) {
      return checker_.staticChecker().checkStaticTarget(target, cls);
    }
    if (type->isDict()) {
      return checker_.dictChecker().checkDictTarget(member, compound);
    }
    if (type->isOptional() && type->inner()->isDict()) {
      checker_.diags_.error(member->object->loc(),
                            "the value may be undefined; check it before assigning "
                            "to '" +
                                member->name + "'");
      return Type::error();
    }
    if (type->isUnion()) {
      if (const Type *common = checker_.unionChecker().commonBaseWith(type, member->name)) {
        member->object = checker_.typeRelations().convert(member->object, common);
        return checker_.classMemberChecker().checkFieldTarget(member, compound);
      }
      return checker_.unionChecker().checkUnionFieldTarget(member);
    }
    if (type->nonOptional()->isBytes()) {
      checker_.diags_.error(
          member->loc(), member->name == "length" || member->name == "byteLength"
                             ? "the length of '" + type->nonOptional()->name().str() +
                                   "' cannot change; make a new one of the length wanted"
                             : "property '" + member->name.str() + "' of type '" +
                                   type->nonOptional()->name().str() + "' cannot be assigned to");
      return Type::error();
    }
    if (type->isRuntimeObject()) {
      return checker_.runtimeObjectChecker().checkRuntimeTarget(member, compound);
    }
    if (type->isObject() || (type->isOptional() && type->inner()->isObject())) {
      return checker_.classMemberChecker().checkFieldTarget(member, compound);
    }
    if (type->isArray() && member->name == "length") {
      if (compound) {
        checker_.diags_.error(member->loc(), "only '=' can change the length of an array");
        return Type::error();
      }
      member->type = Type::intTy();
      return Type::intTy();
    }
    if (type->isString() && member->name == "length") {
      checker_.diags_.error(member->loc(), "the length of a string cannot be changed");
    } else {
      checker_.diags_.error(member->loc(), "property '" + member->name + "' of type '" +
                                               type->name() + "' cannot be assigned to");
    }
    return Type::error();
  }

  if (isa<ArrayLitExpr>(target)) {
    checker_.diags_.error(
        target->loc(), "destructuring assignment can only be a statement, e.g. '[a, b] = [b, a]'");
  } else if (isa<ObjectLitExpr>(target)) {
    checker_.diags_.error(
        target->loc(), "destructuring assignment can only be a statement, e.g. '({ a, b } = o)'");
  } else {
    checker_.diags_.error(target->loc(),
                          "only variables, fields, array elements and 'length' can be assigned to");
  }

  return Type::error();
}

VarSymbol *TargetChecker::checkVarTarget(NameExpr *name) {
  if (name->name == "this") {
    checker_.diags_.error(name->loc(), "cannot assign to 'this'");
    return nullptr;
  }
  Checker::ScopeEntry entry = checker_.topLevelChecker().lookupValue(name->name);
  if (!entry.var) {
    checker_.nameChecker().checkName(name);
    return nullptr;
  }
  name->var = entry.var;
  name->type = checker_.scopes().varType(entry.var);
  checker_.scopes().checkVisible(name, entry.var);
  if (entry.var->isConst) {
    checker_.diags_.error(name->loc(), "cannot assign to '" + name->name + "' because it is a " +
                                           (entry.var->function ? "function" : "constant"));
  } else {
    checker_.topLevelChecker().reportImportAssign(name, entry.var);
  }
  entry.var->reassigned = true;
  checker_.nestedFunctions().noteUse(entry.var, name->loc());

  return entry.var;
}

}
