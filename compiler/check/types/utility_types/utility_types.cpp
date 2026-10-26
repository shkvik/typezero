#include "compiler/check/types/utility_types/utility_types.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/conditional_types/conditional_types.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

}

UtilityTypes::UtilityTypes(Checker &checker) : checker_(checker) {}

const Type *UtilityTypes::getterType(ClassDecl *cls, llvm::StringRef name) {
  if (!cls || name.starts_with("#")) {
    return nullptr;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  for (FunctionDecl *fn : methods) {
    if (fn->accessor == FunctionDecl::Accessor::Getter && fn->sym && fn->access == Access::Public) {
      return checker_.returnInferrer().functionType(fn->sym)->ret();
    }
  }

  return nullptr;
}

const Type *UtilityTypes::importedValueType(Module *from, llvm::StringRef name) {
  for (Stmt *item : from->file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item); fn && fn->name == name) {
      return fn->sym && !fn->sym->isGeneric() ? checker_.returnInferrer().functionType(fn->sym)
                                              : nullptr;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->name == name) {
      return cls->isGeneric() ? nullptr : checker_.classValueChecker().classValueType(cls);
    }
    if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &d : decl->decls) {
        if (d.name == name) {
          return d.sym ? d.sym->type : nullptr;
        }
      }
    }
  }

  return nullptr;
}

const Type *UtilityTypes::resolveUtilityType(const TypeRef &ref) {
  static const struct {
    llvm::StringLiteral name;
    TypeOp op;
    size_t count;
  } utilities[] = {
      {"Exclude", TypeOp::Exclude, 2},           {"Extract", TypeOp::Extract, 2},
      {"NonNullable", TypeOp::NonNullable, 1},   {"ReturnType", TypeOp::ReturnType, 1},
      {"Parameters", TypeOp::Parameters, 1},     {"Uppercase", TypeOp::Uppercase, 1},
      {"Lowercase", TypeOp::Lowercase, 1},       {"Capitalize", TypeOp::Capitalize, 1},
      {"Uncapitalize", TypeOp::Uncapitalize, 1},
  };
  for (const auto &u : utilities) {
    if (ref.name != u.name || checker_.typeDeclarer().lookupType(ref.name)) {
      continue;
    }
    if (ref.args.size() != u.count) {
      checker_.diags_.error(ref.loc, "'" + ref.name + "' takes " + std::to_string(u.count) +
                                         " type argument" + (u.count == 1 ? "" : "s"));
      return Type::error();
    }
    bool function = u.op == TypeOp::ReturnType || u.op == TypeOp::Parameters;
    bool text = u.op == TypeOp::Uppercase || u.op == TypeOp::Lowercase ||
                u.op == TypeOp::Capitalize || u.op == TypeOp::Uncapitalize;
    std::vector<const Type *> operands;
    for (const TypeRef &arg : ref.args) {
      const Type *signature =
          function ? checker_.conditionalTypes().signatureForUtility(arg) : nullptr;
      operands.push_back(signature ? signature
                                   : checker_.conditionalTypes().resolveTypeArgument(arg));
      if (operands.back()->isError()) {
        return operands.back();
      }
    }
    if (function && !mentionsParam(operands[0]) && !operands[0]->nonOptional()->isFunction()) {
      checker_.diags_.error(ref.loc, "'" + ref.name + "' takes a function type, not '" +
                                         operands[0]->name() + "'");

      return Type::error();
    }
    if (text && !mentionsParam(operands[0])) {
      for (const Type *m : distributedMembers(operands[0])) {
        if (!m->isString() && !m->isStringLiteral()) {
          checker_.diags_.error(ref.loc,
                                "'" + ref.name +
                                    "' takes a string type, such as a string literal type, "
                                    "not '" +
                                    m->name() + "'");
          return Type::error();
        }
      }
    }
    return checker_.typeOperators().evalTypeOp(Type::typeOp(u.op, operands), operands);
  }

  return nullptr;
}

Expr *UtilityTypes::checkGenericIndex(IndexExpr *index) {
  const Type *object = index->object->type;
  auto genericKey = [&](const Type *key) {
    return key->isParam() && !key->isListParam() && wantsLiteral(key);
  };
  bool open = object->isParam() && !object->isListParam() && !object->iterated();
  if (!open && !(object->isObject() && isa<NameExpr>(index->index))) {
    return nullptr;
  }
  if (!open) {
    auto *name = cast<NameExpr>(index->index);
    VarSymbol *var = checker_.scopes().lookup(name->name).var;
    if (!var || !var->type || !genericKey(var->type)) {
      return nullptr;
    }
  }
  index->index = checker_.exprChecker().checkExpr(index->index);
  const Type *key = index->index->type;
  if (key->isError()) {
    return index;
  }
  if (!key->isStringLiteral() && !genericKey(key)) {
    checker_.diags_.error(index->index->loc(),
                          "'" + object->name() +
                              "' is indexed by its keys, such as 'k: K' with 'K "
                              "extends keyof " +
                              object->name() + "', not by '" + key->name() + "'");
    return index;
  }
  index->type =
      checker_.typeOperators().evalTypeOp(Type::indexedAccess(object, key), {object, key});

  return index;
}

bool UtilityTypes::wantsLiteral(const Type *param) const {
  const Type *constraint = param->isParam() ? param->constraint() : nullptr;
  if (!constraint) {
    return false;
  }
  if (constraint->typeOp() == TypeOp::KeyOf) {
    return true;
  }
  const Type *c = constraint;

  return llvm::all_of(membersOf(c), [](const Type *m) { return m->isStringLiteral(); });
}

}
