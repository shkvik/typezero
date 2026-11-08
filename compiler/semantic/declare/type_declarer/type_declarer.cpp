#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <string>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

TypeDeclarer::TypeDeclarer(SemanticState &owner)
    : state(owner) {}

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
  if (this->state.getScopeStack().hasTypeInCurrentScope(name)) {
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
        this->state.getDiagnostics().error(alias->loc(), own.error().message);
      }
      if (own.isErr()) {
        continue;
      }
    } else if (decl) {
      name = decl->name;
      Scopes::ResultEntryDeclared added =
          Scopes(this->state).declare(name, {EnumEntry{.enumDecl = decl}});
      if (added.isErr()) {
        this->state.getDiagnostics().error(decl->loc(), added.error().message);
      }
      if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
        ScopeEntry previousEntry = Scopes(this->state).lookup(name);
        std::string declaredHere = ErrorTextDeclare::previous();
        this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
      }
    } else if (cls && this->state.getScopeStack().isTopLevel()) {
      name = cls->name;
      ClassDeclarer(this->state).declareClass(cls);
    } else {
      continue;
    }
    ResultTypeNameIsFree unused = this->freeTypeName(name);
    if (unused.isErr()) {
      Stmt *previous = this->state.getScopeStack().findTypeInCurrentScope(name);
      std::string previousText = ErrorTextFreeTypeName::previous();
      this->state.getDiagnostics().error(stmt->loc(), unused.error().message);
      this->state.getDiagnostics().note(previous->loc(), previousText);
      continue;
    }
    this->state.getScopeStack().addType(name, stmt);
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
      std::vector<const Type *> params = AliasInstantiator(this->state).aliasTypeParams(alias);
      AliasInstantiator(this->state).instantiateAlias(alias, std::move(params), alias->loc());
    } else if (alias) {
      this->resolveAlias(alias);
    } else if (cls) {
      if (!cls->baseExpr) {
        ClassDeclarer(this->state).resolveClass(cls);
      }
    } else {
      this->resolveEnum(cast<EnumDecl>(stmt));
    }
  }
}

Stmt *TypeDeclarer::lookupType(llvm::StringRef name) const {
  if (Stmt *found = this->state.getScopeStack().findType(name)) {
    return found;
  }
  TypeAliasDecl *alias = this->state.getAliasStore().findBuiltinAlias(name);
  if (alias) {
    return alias;
  }
  const NodeName *imported = BuiltinModuleChecker(this->state).nodeName(name);
  if (imported) {
    return BuiltinModuleTypes(this->state).exported(imported->module, imported->name);
  }
  if (this->state.getTypeContext().isInsideBuiltinModule()) {
    return BuiltinModuleTypes(this->state).alias(name);
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
  Module *home = this->state.getModuleStates().findDeclModule(alias);
  bool elsewhere = home && home != this->state.getModuleStates().getCurrentModule() &&
                   this->state.getScopeStack().isTopLevel();
  if (elsewhere) {
    Module *here = this->state.getModuleStates().getCurrentModule();
    ModuleDeclarer(this->state).useModule(home);
    const Type *type = this->resolveAlias(alias);
    ModuleDeclarer(this->state).useModule(here);
    return type;
  }
  if (alias->resolving && alias->self) {
    alias->recursive = true;
    return alias->self;
  }
  ResultAliasNotCyclic cycle = TypeDeclarer::aliasCycle(alias);
  if (cycle.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), cycle.error().message);
    alias->resolved = Type::error();
    return alias->resolved;
  }

  alias->resolving = true;
  bool builtinModule = BuiltinModuleTypes(this->state).alias(alias->name) == alias;
  if (builtinModule) {
    this->state.getTypeContext().enterBuiltinModule();
  }
  const Type *type = nullptr;
  if (alias->type.kind() == TypeRefKind::OBJECT_REF) {
    alias->self = Type::declareObject(alias->name);
    std::vector<const Type *> bases;
    std::vector<Overload> overloads;
    std::vector<Field> fields =
        alias->bases.empty() ? TypeResolver(this->state).resolveFields(alias->type, &overloads)
                             : ClassBases(this->state).interfaceFields(alias, bases, overloads);
    if (alias->recursive) {
      alias->self->defineFields(std::move(fields), std::move(overloads));
      type = alias->self;
    } else {
      type = Type::object(std::move(fields), alias->name, std::move(overloads));
    }
    for (const Type *base : bases) {
      if (!type->isError()) {
        ClassBases(this->state).addBase(type, base);
      }
    }
  } else if (alias->type.kind() == TypeRefKind::UNION_REF) {
    alias->self = Type::declareUnion(alias->name);
    type = TypeResolver(this->state).resolveTypeInner(alias->type);
    ResultUnionRecursion recursion = TypeDeclarer::recursiveUnion(alias, type);
    if (recursion.isErr()) {
      this->state.getDiagnostics().error(alias->loc(), recursion.error().message);
      type = Type::error();
    } else if (alias->recursive && !type->isError()) {
      alias->self->defineMembers(type);
      type = alias->self;
    }
  } else {
    type = TypeResolver(this->state).resolveTypeInner(alias->type);
  }
  alias->resolving = false;
  if (builtinModule) {
    this->state.getTypeContext().leaveBuiltinModule();
  }
  if (!alias->resolved) {
    alias->resolved = type;
  }
  bool builtin = this->state.getAliasStore().findBuiltinAlias(alias->name) == alias ||
                 BuiltinModuleTypes(this->state).alias(alias->name) == alias;
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
TypeDeclarer::memberConstant(std::optional<ConstantValue> constant) {
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
      this->state.getDiagnostics().error(decl->loc(), resolved.error().message);
    }
    return;
  }
  Module *home = this->state.getModuleStates().findDeclModule(decl);
  bool elsewhere = home && home != this->state.getModuleStates().getCurrentModule() &&
                   this->state.getScopeStack().isTopLevel();
  if (elsewhere) {
    Module *here = this->state.getModuleStates().getCurrentModule();
    ModuleDeclarer(this->state).useModule(home);
    this->resolveEnum(decl);
    ModuleDeclarer(this->state).useModule(here);
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
        this->state.getDiagnostics().error(m.loc, unique.error().message);
      }
    }
    double value = next;
    if (m.init) {
      std::optional<ConstantValue> found =
          ConstantFolder(this->state).evalConstant(m.init, decl, i);
      ResultMemberConstant constant = TypeDeclarer::memberConstant(found);
      if (constant.isErr()) {
        this->state.getDiagnostics().error(m.init->loc(), constant.error().message);
        counting = false;
        continue;
      }
      const ConstantValue &c = constant.value();
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
      this->state.getDiagnostics().error(m.loc, valued.error().message);
      continue;
    }
    ResultMemberIntValue whole = TypeDeclarer::wholeMember(m, value);
    if (whole.isErr()) {
      llvm::SMLoc at = m.init ? m.init->loc() : m.loc;
      this->state.getDiagnostics().error(at, whole.error().message);
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
    this->state.getDiagnostics().error(decl->loc(), same.error().message);
  }
  decl->isString = strings && !numbers;
  decl->stage = EnumStage::RESOLVED;
  if (decl->isString) {
    for (const EnumMember &m : decl->members) {
      this->state.getFlowState().markFieldAssigned(ToUtf8::toUtf8(m.text));
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
    auto *lit = this->state.getAst().make<StringLitExpr>(member->loc(), m->text);
    if (written) {
      this->state.getInventoryStore().rememberNamedUse(lit,
                                                       {decl, member->object->loc(), member->name});
    }
    const Type *literal = LiteralTypes(this->state).stringLiteralFor(expected, m->text);
    lit->type = Type::stringLiteral(m->text);
    if (literal) {
      return ok(lit);
    }
    const Type *type = this->enumType(decl);
    Expr *converted = TypeRelations(this->state).convert(lit, type);

    return ok(converted);
  }
  int64_t value = m->number;
  auto magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  Expr *lit = this->state.getAst().make<IntLitExpr>(member->loc(), magnitude);
  if (written) {
    this->state.getInventoryStore().rememberNamedUse(lit,
                                                     {decl, member->object->loc(), member->name});
  }
  if (value < 0) {
    lit = this->state.getAst().make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
  }
  Expr *checked = ExprChecker(this->state).checkExpr(lit, expected);

  return ok(checked);
}

TypeDeclarer::ResultEnumIndexValue TypeDeclarer::checkEnumIndex(EnumDecl *decl, IndexExpr *index) {
  this->resolveEnum(decl);
  index->type = Type::error();
  index->index = ExprChecker(this->state).checkExpr(index->index, Type::intTy());
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

  index->index = TypeRelations(this->state).convert(index->index, Type::intTy());
  std::optional<Linear> value = IndexProofs(this->state).linear(index->index);
  if (value && value->term.isZero()) {
    const EnumMember *found = nullptr;
    for (const EnumMember &m : decl->members) {
      if (m.number == value->offset) {
        found = &m;
      }
    }
    if (!found) {
      auto *undefined = this->state.getAst().make<UndefinedExpr>(index->loc());
      undefined->type = Type::undefinedTy();
      return ok(undefined);
    }
    std::u16string name = TypeDeclarer::toUtf16(found->name);
    auto *lit = this->state.getAst().make<StringLitExpr>(index->loc(), name);
    lit->type = Type::string();
    return ok(lit);
  }
  index->form = EnumNameIndex{.enumDecl = decl};
  index->type = Type::optional(Type::string());

  return ok(index);
}

}
