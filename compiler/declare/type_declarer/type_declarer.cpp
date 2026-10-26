#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::u16string toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::string formatNumber(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

}

TypeDeclarer::TypeDeclarer(Checker &checker) : checker_(checker) {}

std::vector<Stmt *> TypeDeclarer::declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve) {
  std::vector<Stmt *> declared;
  for (Stmt *stmt : stmts) {
    llvm::StringRef name;
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt)) {
      name = alias->name;
      if (Type::fromName(name) || name == "Array") {
        bool integer = name == "int" || name == "long" || name == "double";
        bool forTsc =
            integer && alias->type.kind == TypeRef::Kind::Name && alias->type.name == "number";
        forTsc |= name == "Json" && alias->type.kind == TypeRef::Kind::Union;
        if (!forTsc) {
          checker_.diags_.error(
              alias->loc(),
              "'" + name + "' is a built-in type and cannot be redefined" +
                  (integer ? "; only 'type " + name.str() + " = number', for tsc, may declare it"
                           : std::string()));
        }
        continue;
      }
    } else if (auto *decl = dyn_cast<EnumDecl>(stmt)) {
      name = decl->name;
      checker_.scopes().declare(name, decl->loc(), {nullptr, nullptr, decl});
    } else if (auto *cls = dyn_cast<ClassDecl>(stmt); cls && checker_.scopes_.size() == 1) {
      name = cls->name;
      checker_.classDeclarer().declareClass(cls);
    } else {
      continue;
    }
    auto [it, inserted] = checker_.typeScopes_.back().try_emplace(name, stmt);
    if (!inserted) {
      checker_.diags_.error(stmt->loc(), "duplicate declaration of type '" + name + "'");
      checker_.diags_.note(it->second->loc(), "previously declared here");
      continue;
    }
    declared.push_back(stmt);
  }
  if (resolve) {
    resolveTypes(declared);
  }

  return declared;
}

void TypeDeclarer::resolveTypes(llvm::ArrayRef<Stmt *> declared) {
  std::function<bool(const TypeRef &)> typeOf = [&](const TypeRef &ref) {
    return ref.kind == TypeRef::Kind::TypeOf || llvm::any_of(ref.args, typeOf);
  };
  for (Stmt *stmt : declared) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt); alias && typeOf(alias->type)) {
      continue;
    }
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt); alias && !alias->typeParams.empty()) {
      checker_.aliasInstantiator().instantiateAlias(
          alias, checker_.aliasInstantiator().aliasTypeParams(alias), alias->loc());
    } else if (alias) {
      resolveAlias(alias);
    } else if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      if (!cls->baseExpr) {
        checker_.classDeclarer().resolveClass(cls);
      }
    } else {
      resolveEnum(cast<EnumDecl>(stmt));
    }
  }
}

Stmt *TypeDeclarer::lookupType(llvm::StringRef name) const {
  for (auto scope = checker_.typeScopes_.rbegin(); scope != checker_.typeScopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }
  if (TypeAliasDecl *alias = checker_.builtinAliases_.lookup(name)) {
    return alias;
  }

  return checker_.throwChecker().errorKindClass(name);
}

const Type *TypeDeclarer::resolveAlias(TypeAliasDecl *alias) {
  if (alias->resolved) {
    return alias->resolved;
  }
  if (Module *home = checker_.declModules_.lookup(alias);
      home && home != checker_.module_ && checker_.scopes_.size() == 1) {
    Module *here = checker_.module_;
    checker_.moduleDeclarer().useModule(home);
    const Type *type = resolveAlias(alias);
    checker_.moduleDeclarer().useModule(here);
    return type;
  }
  if (alias->resolving) {
    if (alias->self) {
      alias->recursive = true;
      return alias->self;
    }
    checker_.diags_.error(alias->loc(), "type alias '" + alias->name + "' refers to itself");
    alias->resolved = Type::error();
    return alias->resolved;
  }
  alias->resolving = true;
  const Type *type;
  if (alias->type.kind == TypeRef::Kind::Object) {
    alias->self = Type::declareObject(alias->name);
    std::vector<const Type *> bases;
    std::vector<Overload> overloads;
    std::vector<Field> fields =
        alias->bases.empty() ? checker_.typeResolver().resolveFields(alias->type, &overloads)
                             : checker_.classBases().interfaceFields(alias, bases, overloads);
    if (alias->recursive) {
      alias->self->defineFields(std::move(fields), std::move(overloads));
      type = alias->self;
    } else {
      type = Type::object(std::move(fields), alias->name, std::move(overloads));
    }
    for (const Type *base : bases) {
      if (!type->isError()) {
        checker_.classBases().addBase(type, base);
      }
    }
  } else if (alias->type.kind == TypeRef::Kind::Union) {
    alias->self = Type::declareUnion(alias->name);
    type = checker_.typeResolver().resolveTypeInner(alias->type);
    if (alias->recursive && !type->isError()) {
      if (type->isUnion()) {
        alias->self->defineMembers(type);
        type = alias->self;
      } else {
        checker_.diags_.error(alias->loc(),
                              "type alias '" + alias->name +
                                  "' refers to itself, and only a union of several types "
                                  "without undefined can: make a field optional instead, "
                                  "e.g. 'next?: " +
                                  alias->name + "'");
        type = Type::error();
      }
    }
  } else {
    type = checker_.typeResolver().resolveTypeInner(alias->type);
  }
  alias->resolving = false;
  if (!alias->resolved) {
    alias->resolved = type;
  }
  if (checker_.builtinAliases_.lookup(alias->name) == alias) {
    Type::nameUnion(alias->resolved, alias->name);
  }

  return alias->resolved;
}

void TypeDeclarer::resolveEnum(EnumDecl *decl) {
  if (decl->resolved || decl->resolving) {
    if (decl->resolving) {
      checker_.diags_.error(decl->loc(),
                            "the members of enum '" + decl->name + "' depend on themselves");
    }
    return;
  }
  if (Module *home = checker_.declModules_.lookup(decl);
      home && home != checker_.module_ && checker_.scopes_.size() == 1) {
    Module *here = checker_.module_;
    checker_.moduleDeclarer().useModule(home);
    resolveEnum(decl);
    checker_.moduleDeclarer().useModule(here);
    return;
  }
  decl->resolving = true;
  double next = 0;
  bool counting = true;
  bool numbers = false, strings = false;
  for (size_t i = 0; i < decl->members.size(); ++i) {
    EnumMember &m = decl->members[i];
    for (size_t j = 0; j < i; ++j) {
      if (decl->members[j].name == m.name) {
        checker_.diags_.error(m.loc, "duplicate enum member '" + m.name + "'");
      }
    }
    double value = next;
    if (m.init) {
      std::optional<Checker::Constant> c = checker_.constantFolder().evalConstant(m.init, decl, i);
      if (!c) {
        checker_.diags_.error(m.init->loc(),
                              "the value of an enum member must be a constant: a number, a "
                              "string, or an expression of them and of other members");
        counting = false;
        continue;
      }
      if (c->isString) {
        m.isString = true;
        m.text = c->text;
        strings = true;
        counting = false;
        continue;
      }
      value = c->number;
    } else if (!counting) {
      checker_.diags_.error(m.loc,
                            "enum member '" + m.name +
                                "' needs a value, since the member before it is not a number");
      continue;
    }
    if (value != std::trunc(value) || value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      checker_.diags_.error(m.init ? m.init->loc() : m.loc, "the value of enum member '" + m.name +
                                                                "' must be an integer that fits in "
                                                                "'int', found " +
                                                                formatNumber(value));
      counting = false;
      continue;
    }
    m.number = static_cast<int32_t>(value);
    numbers = true;
    next = value + 1;
    counting = true;
  }
  if (numbers && strings) {
    checker_.diags_.error(decl->loc(), "enum '" + decl->name + "' cannot mix numbers and strings");
  }
  decl->isString = strings && !numbers;
  decl->resolving = false;
  decl->resolved = true;
}

Expr *TypeDeclarer::checkEnumMember(EnumDecl *decl, MemberExpr *member, const Type *expected) {
  resolveEnum(decl);
  const EnumMember *m = decl->find(member->name);
  if (!m) {
    checker_.diags_.error(member->loc(), "property '" + member->name +
                                             "' does not exist on enum '" + decl->name + "'");
    member->type = Type::error();
    return member;
  }
  if (m->isString) {
    auto *lit = checker_.ctx_.make<StringLitExpr>(member->loc(), m->text);
    lit->type = Type::string();
    return lit;
  }
  int64_t value = m->number;
  Expr *lit = checker_.ctx_.make<IntLitExpr>(member->loc(),
                                             static_cast<uint64_t>(value < 0 ? -value : value));
  if (value < 0) {
    lit = checker_.ctx_.make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
  }

  return checker_.exprChecker().checkExpr(lit, expected);
}

Expr *TypeDeclarer::checkEnumIndex(EnumDecl *decl, IndexExpr *index) {
  resolveEnum(decl);
  index->type = Type::error();
  index->index = checker_.exprChecker().checkExpr(index->index, Type::longTy());
  const Type *type = index->index->type;
  if (type->isError()) {
    return index;
  }
  if (decl->isString) {
    checker_.diags_.error(index->loc(),
                          "enum '" + decl->name +
                              "' has string values, and only numbers map back to names");
    return index;
  }
  if (!type->isInteger()) {
    checker_.diags_.error(index->index->loc(),
                          "the value of an enum member is an integer, found '" + type->name() +
                              "'");
    return index;
  }
  index->index = checker_.typeRelations().convert(index->index, Type::longTy());
  if (std::optional<Linear> value = checker_.indexProofs().linear(index->index);
      value && value->term.isZero()) {
    const EnumMember *found = nullptr;
    for (const EnumMember &m : decl->members) {
      if (m.number == value->offset) {
        found = &m;
      }
    }
    if (!found) {
      auto *undefined = checker_.ctx_.make<UndefinedExpr>(index->loc());
      undefined->type = Type::undefinedTy();
      return undefined;
    }
    auto *lit = checker_.ctx_.make<StringLitExpr>(index->loc(), toUtf16(found->name));
    lit->type = Type::string();
    return lit;
  }
  index->enumDecl = decl;
  index->type = Type::optional(Type::string());

  return index;
}

}
