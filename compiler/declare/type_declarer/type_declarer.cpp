#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <string>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

TypeDeclarer::TypeDeclarer(Checker &owner)
    : checker(owner) {}

std::u16string TypeDeclarer::toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::string TypeDeclarer::formatNumber(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

TypeDeclarer::ResultAliasIsOwnType TypeDeclarer::builtinAlias(const TypeAliasDecl *alias) {
  llvm::StringRef name = alias->name;
  bool builtin = Type::fromName(name) || name == "Array";
  bool integer = builtin && Type::fromName(name)->isNumeric();
  bool forTsc =
      integer && alias->type.kind() == TypeRefKind::NAME_REF && alias->type.name == "number";
  forTsc |= builtin && name == "Json" && alias->type.kind() == TypeRefKind::UNION_REF;
  if (forTsc) {
    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::DECLARED_FOR_TSC,
        .message = "",
    });
  }
  if (integer) {
    std::string message = ErrorTextBuiltinAlias::number(name);

    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::REDEFINES_NUMBER,
        .message = message,
    });
  }
  if (builtin) {
    std::string message = ErrorTextBuiltinAlias::builtin(name);

    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::REDEFINES_BUILTIN,
        .message = message,
    });
  }

  return ok(alias);
}

TypeDeclarer::ResultTypeNameIsFree TypeDeclarer::freeTypeName(llvm::StringRef name) const {
  if (this->checker.typeScopes_.back().count(name)) {
    std::string message = ErrorTextFreeTypeName::duplicate(name);

    return err(TypeDeclarerFreeTypeNameError{
        .code = TypeDeclarerFreeTypeNameErrorCode::DUPLICATE_TYPE,
        .message = message,
    });
  }

  return ok(name);
}

std::vector<Stmt *> TypeDeclarer::declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve) {
  std::vector<Stmt *> declared;
  for (Stmt *stmt : stmts) {
    llvm::StringRef name;
    auto *alias = dyn_cast<TypeAliasDecl>(stmt);
    auto *decl = dyn_cast<EnumDecl>(stmt);
    auto *cls = dyn_cast<ClassDecl>(stmt);
    if (alias) {
      name = alias->name;
      ResultAliasIsOwnType own = TypeDeclarer::builtinAlias(alias);
      bool forTsc =
          own.isErr() && own.error().code == TypeDeclarerBuiltinAliasErrorCode::DECLARED_FOR_TSC;
      if (own.isErr() && !forTsc) {
        this->checker.diags_.error(alias->loc(), own.error().message);
      }
      if (own.isErr()) {
        continue;
      }
    } else if (decl) {
      name = decl->name;
      this->checker.scopes().declare(name, decl->loc(), {EnumEntry{.enumDecl = decl}});
    } else if (cls && this->checker.scopes_.size() == 1) {
      name = cls->name;
      this->checker.classDeclarer().declareClass(cls);
    } else {
      continue;
    }
    ResultTypeNameIsFree unused = this->freeTypeName(name);
    if (unused.isErr()) {
      Stmt *previous = this->checker.typeScopes_.back().find(name)->second;
      std::string previousText = ErrorTextFreeTypeName::previous();
      this->checker.diags_.error(stmt->loc(), unused.error().message);
      this->checker.diags_.note(previous->loc(), previousText);
      continue;
    }
    this->checker.typeScopes_.back().try_emplace(name, stmt);
    declared.push_back(stmt);
  }
  if (resolve) {
    this->resolveTypes(declared);
  }

  return declared;
}

bool TypeDeclarer::mentionsTypeOf(const TypeRef &ref) {
  return ref.kind() == TypeRefKind::TYPE_OF_REF ||
         llvm::any_of(ref.args, TypeDeclarer::mentionsTypeOf);
}

void TypeDeclarer::resolveTypes(llvm::ArrayRef<Stmt *> declared) {
  for (Stmt *stmt : declared) {
    auto *alias = dyn_cast<TypeAliasDecl>(stmt);
    auto *cls = dyn_cast<ClassDecl>(stmt);
    if (alias && TypeDeclarer::mentionsTypeOf(alias->type)) {
      continue;
    }
    if (alias && !alias->typeParams.empty()) {
      std::vector<const Type *> params = this->checker.aliasInstantiator().aliasTypeParams(alias);
      this->checker.aliasInstantiator().instantiateAlias(alias, std::move(params), alias->loc());
    } else if (alias) {
      this->resolveAlias(alias);
    } else if (cls) {
      if (!cls->baseExpr) {
        this->checker.classDeclarer().resolveClass(cls);
      }
    } else {
      this->resolveEnum(cast<EnumDecl>(stmt));
    }
  }
}

Stmt *TypeDeclarer::lookupType(llvm::StringRef name) const {
  for (auto scope = this->checker.typeScopes_.rbegin(); scope != this->checker.typeScopes_.rend();
       ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }
  TypeAliasDecl *alias = this->checker.builtinAliases_.lookup(name);
  if (alias) {
    return alias;
  }
  const Checker::NodeName *imported = this->checker.builtinModuleChecker().nodeName(name);
  if (imported) {
    return this->checker.builtinModuleTypes().exported(imported->module, imported->name);
  }
  if (this->builtinDepth > 0) {
    return this->checker.builtinModuleTypes().alias(name);
  }

  return nullptr;
}

TypeDeclarer::ResultAliasNotCyclic TypeDeclarer::aliasCycle(const TypeAliasDecl *alias) {
  if (alias->resolving && !alias->self) {
    std::string message = ErrorTextAliasCycle::itself(alias->name);

    return err(TypeDeclarerAliasCycleError{
        .code = TypeDeclarerAliasCycleErrorCode::REFERS_TO_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

TypeDeclarer::ResultUnionRecursion TypeDeclarer::recursiveUnion(const TypeAliasDecl *alias,
                                                                const Type *type) {
  if (alias->recursive && !type->isError() && !type->isUnion()) {
    std::string message = ErrorTextRecursiveUnion::notUnion(alias->name);

    return err(TypeDeclarerRecursiveUnionError{
        .code = TypeDeclarerRecursiveUnionErrorCode::NOT_A_UNION,
        .message = message,
    });
  }

  return ok(type);
}

const Type *TypeDeclarer::resolveAlias(TypeAliasDecl *alias) {
  if (alias->resolved) {
    return alias->resolved;
  }
  Module *home = this->checker.declModules_.lookup(alias);
  bool elsewhere = home && home != this->checker.module_ && this->checker.scopes_.size() == 1;
  if (elsewhere) {
    Module *here = this->checker.module_;
    this->checker.moduleDeclarer().useModule(home);
    const Type *type = this->resolveAlias(alias);
    this->checker.moduleDeclarer().useModule(here);
    return type;
  }
  if (alias->resolving && alias->self) {
    alias->recursive = true;
    return alias->self;
  }
  ResultAliasNotCyclic cycle = TypeDeclarer::aliasCycle(alias);
  if (cycle.isErr()) {
    this->checker.diags_.error(alias->loc(), cycle.error().message);
    alias->resolved = Type::error();
    return alias->resolved;
  }

  alias->resolving = true;
  bool builtinModule = this->checker.builtinModuleTypes().alias(alias->name) == alias;
  this->builtinDepth += builtinModule ? 1 : 0;
  const Type *type = nullptr;
  if (alias->type.kind() == TypeRefKind::OBJECT_REF) {
    alias->self = Type::declareObject(alias->name);
    std::vector<const Type *> bases;
    std::vector<Overload> overloads;
    std::vector<Field> fields =
        alias->bases.empty() ? this->checker.typeResolver().resolveFields(alias->type, &overloads)
                             : this->checker.classBases().interfaceFields(alias, bases, overloads);
    if (alias->recursive) {
      alias->self->defineFields(std::move(fields), std::move(overloads));
      type = alias->self;
    } else {
      type = Type::object(std::move(fields), alias->name, std::move(overloads));
    }
    for (const Type *base : bases) {
      if (!type->isError()) {
        this->checker.classBases().addBase(type, base);
      }
    }
  } else if (alias->type.kind() == TypeRefKind::UNION_REF) {
    alias->self = Type::declareUnion(alias->name);
    type = this->checker.typeResolver().resolveTypeInner(alias->type);
    ResultUnionRecursion recursion = TypeDeclarer::recursiveUnion(alias, type);
    if (recursion.isErr()) {
      this->checker.diags_.error(alias->loc(), recursion.error().message);
      type = Type::error();
    } else if (alias->recursive && !type->isError()) {
      alias->self->defineMembers(type);
      type = alias->self;
    }
  } else {
    type = this->checker.typeResolver().resolveTypeInner(alias->type);
  }
  alias->resolving = false;
  this->builtinDepth -= builtinModule ? 1 : 0;
  if (!alias->resolved) {
    alias->resolved = type;
  }
  bool builtin = this->checker.builtinAliases_.lookup(alias->name) == alias ||
                 this->checker.builtinModuleTypes().alias(alias->name) == alias;
  if (builtin && alias->type.kind() == TypeRefKind::UNION_REF) {
    Type::nameUnion(alias->resolved, alias->name);
  }

  return alias->resolved;
}

TypeDeclarer::ResultEnumIsResolved TypeDeclarer::enumCycle(const EnumDecl *decl) {
  if (decl->stage == EnumStage::RESOLVING) {
    std::string message = ErrorTextEnumCycle::itself(decl->name);

    return err(TypeDeclarerEnumCycleError{
        .code = TypeDeclarerEnumCycleErrorCode::DEPENDS_ON_ITSELF,
        .message = message,
    });
  }

  return ok(decl);
}

TypeDeclarer::ResultMemberIsUnique TypeDeclarer::uniqueMember(const EnumMember &m,
                                                              const EnumMember &other) {
  if (other.name == m.name) {
    std::string message = ErrorTextUniqueMember::duplicate(m.name);

    return err(TypeDeclarerUniqueMemberError{
        .code = TypeDeclarerUniqueMemberErrorCode::DUPLICATE_MEMBER,
        .message = message,
    });
  }

  return ok(&m);
}

TypeDeclarer::ResultMemberConstant
TypeDeclarer::memberConstant(std::optional<Checker::Constant> constant) {
  if (!constant) {
    std::string message = ErrorTextMemberConstant::notConst();

    return err(TypeDeclarerMemberConstantError{
        .code = TypeDeclarerMemberConstantErrorCode::NOT_CONSTANT,
        .message = message,
    });
  }

  return ok(*constant);
}

TypeDeclarer::ResultMemberHasValue TypeDeclarer::countedMember(const EnumMember &m, bool counting) {
  if (!m.init && !counting) {
    std::string message = ErrorTextCountedMember::noValue(m.name);

    return err(TypeDeclarerCountedMemberError{
        .code = TypeDeclarerCountedMemberErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&m);
}

TypeDeclarer::ResultMemberIntValue TypeDeclarer::wholeMember(const EnumMember &m, double value) {
  if (value != std::trunc(value) || std::fabs(value) > 9007199254740992.0) {
    std::string shown = TypeDeclarer::formatNumber(value);
    std::string message = ErrorTextWholeMember::notWhole(m.name, shown);

    return err(TypeDeclarerWholeMemberError{
        .code = TypeDeclarerWholeMemberErrorCode::NOT_WHOLE,
        .message = message,
    });
  }

  return ok(value);
}

TypeDeclarer::ResultEnumKindIsSame TypeDeclarer::enumKinds(const EnumDecl *decl, bool numbers,
                                                           bool strings) {
  if (numbers && strings) {
    std::string message = ErrorTextEnumKinds::mixed(decl->name);

    return err(TypeDeclarerEnumKindsError{
        .code = TypeDeclarerEnumKindsErrorCode::MIXED,
        .message = message,
    });
  }

  return ok(decl);
}

void TypeDeclarer::resolveEnum(EnumDecl *decl) {
  if (decl->stage != EnumStage::UNRESOLVED) {
    ResultEnumIsResolved resolved = TypeDeclarer::enumCycle(decl);
    if (resolved.isErr()) {
      this->checker.diags_.error(decl->loc(), resolved.error().message);
    }
    return;
  }
  Module *home = this->checker.declModules_.lookup(decl);
  bool elsewhere = home && home != this->checker.module_ && this->checker.scopes_.size() == 1;
  if (elsewhere) {
    Module *here = this->checker.module_;
    this->checker.moduleDeclarer().useModule(home);
    this->resolveEnum(decl);
    this->checker.moduleDeclarer().useModule(here);
    return;
  }
  decl->stage = EnumStage::RESOLVING;
  double next = 0;
  bool counting = true;
  bool numbers = false;
  bool strings = false;
  for (size_t i = 0; i < decl->members.size(); ++i) {
    EnumMember &m = decl->members[i];
    for (size_t j = 0; j < i; ++j) {
      ResultMemberIsUnique unique = TypeDeclarer::uniqueMember(m, decl->members[j]);
      if (unique.isErr()) {
        this->checker.diags_.error(m.loc, unique.error().message);
      }
    }
    double value = next;
    if (m.init) {
      std::optional<Checker::Constant> found =
          this->checker.constantFolder().evalConstant(m.init, decl, i);
      ResultMemberConstant constant = TypeDeclarer::memberConstant(found);
      if (constant.isErr()) {
        this->checker.diags_.error(m.init->loc(), constant.error().message);
        counting = false;
        continue;
      }
      const Checker::Constant &c = constant.value();
      if (c.isString) {
        m.isString = true;
        m.text = c.text;
        strings = true;
        counting = false;
        continue;
      }
      value = c.number;
    }
    ResultMemberHasValue valued = TypeDeclarer::countedMember(m, counting);
    if (valued.isErr()) {
      this->checker.diags_.error(m.loc, valued.error().message);
      continue;
    }
    ResultMemberIntValue whole = TypeDeclarer::wholeMember(m, value);
    if (whole.isErr()) {
      llvm::SMLoc at = m.init ? m.init->loc() : m.loc;
      this->checker.diags_.error(at, whole.error().message);
      counting = false;
      continue;
    }
    m.number = static_cast<int64_t>(value);
    numbers = true;
    next = value + 1;
    counting = true;
  }
  ResultEnumKindIsSame same = TypeDeclarer::enumKinds(decl, numbers, strings);
  if (same.isErr()) {
    this->checker.diags_.error(decl->loc(), same.error().message);
  }
  decl->isString = strings && !numbers;
  decl->stage = EnumStage::RESOLVED;
  if (decl->isString) {
    for (const EnumMember &m : decl->members) {
      this->checker.assignedFields_.insert(toUtf8(m.text));
    }
  }
}

const Type *TypeDeclarer::enumType(EnumDecl *decl) {
  this->resolveEnum(decl);
  if (!decl->isString) {
    return Type::intTy();
  }
  std::vector<const Type *> values;
  for (const EnumMember &m : decl->members) {
    if (m.isString) {
      values.push_back(Type::stringLiteral(m.text));
    }
  }
  if (values.empty()) {
    return Type::string();
  }
  const Type *type = Type::unionOf(values);
  Type::nameUnion(type, decl->name);

  return type;
}

TypeDeclarer::ResultEnumMemberRead TypeDeclarer::checkEnumMember(EnumDecl *decl, MemberExpr *member,
                                                                 const Type *expected) {
  this->resolveEnum(decl);
  const EnumMember *m = decl->find(member->name);
  if (!m) {
    std::string message = ErrorTextEnumMember::missing(member->name, decl->name);

    return err(TypeDeclarerCheckEnumMemberError{
        .code = TypeDeclarerCheckEnumMemberErrorCode::NO_SUCH_MEMBER,
        .message = message,
    });
  }

  bool written = member->object && member->object->loc() != member->loc();
  if (m->isString) {
    auto *lit = this->checker.ctx_.make<StringLitExpr>(member->loc(), m->text);
    if (written) {
      this->checker.file_->namedUses[lit] = {decl, member->object->loc(), member->name};
    }
    const Type *literal = this->checker.literalTypes().stringLiteralFor(expected, m->text);
    lit->type = Type::stringLiteral(m->text);
    if (literal) {
      return ok(lit);
    }
    const Type *type = this->enumType(decl);
    Expr *converted = this->checker.typeRelations().convert(lit, type);

    return ok(converted);
  }
  int64_t value = m->number;
  auto magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  Expr *lit = this->checker.ctx_.make<IntLitExpr>(member->loc(), magnitude);
  if (written) {
    this->checker.file_->namedUses[lit] = {decl, member->object->loc(), member->name};
  }
  if (value < 0) {
    lit = this->checker.ctx_.make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
  }
  Expr *checked = this->checker.exprChecker().checkExpr(lit, expected);

  return ok(checked);
}

TypeDeclarer::ResultEnumIndexValue TypeDeclarer::checkEnumIndex(EnumDecl *decl, IndexExpr *index) {
  this->resolveEnum(decl);
  index->type = Type::error();
  index->index = this->checker.exprChecker().checkExpr(index->index, Type::intTy());
  const Type *type = index->index->type;
  if (type->isError()) {
    return err(TypeDeclarerCheckEnumIndexError{
        .code = TypeDeclarerCheckEnumIndexErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (decl->isString) {
    std::string message = ErrorTextEnumIndex::strings(decl->name);

    return err(TypeDeclarerCheckEnumIndexError{
        .code = TypeDeclarerCheckEnumIndexErrorCode::STRING_ENUM,
        .message = message,
    });
  }
  if (!type->isInteger()) {
    std::string message = ErrorTextEnumIndex::notInteger(type->name());

    return err(TypeDeclarerCheckEnumIndexError{
        .code = TypeDeclarerCheckEnumIndexErrorCode::NOT_AN_INTEGER,
        .message = message,
    });
  }

  index->index = this->checker.typeRelations().convert(index->index, Type::intTy());
  std::optional<Linear> value = this->checker.indexProofs().linear(index->index);
  if (value && value->term.isZero()) {
    const EnumMember *found = nullptr;
    for (const EnumMember &m : decl->members) {
      if (m.number == value->offset) {
        found = &m;
      }
    }
    if (!found) {
      auto *undefined = this->checker.ctx_.make<UndefinedExpr>(index->loc());
      undefined->type = Type::undefinedTy();
      return ok(undefined);
    }
    std::u16string name = TypeDeclarer::toUtf16(found->name);
    auto *lit = this->checker.ctx_.make<StringLitExpr>(index->loc(), name);
    lit->type = Type::string();
    return ok(lit);
  }
  index->form = EnumNameIndex{.enumDecl = decl};
  index->type = Type::optional(Type::string());

  return ok(index);
}

}
