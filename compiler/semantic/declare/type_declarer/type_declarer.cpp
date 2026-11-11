#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
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
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <string>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

TypeDeclarer::TypeDeclarer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->classBases = &all.classBases;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->literalTypes = &all.literalTypes;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->moduleDeclarer = &all.moduleDeclarer;
  this->aliasInstantiator = &all.aliasInstantiator;
  this->builtinModuleTypes = &all.builtinModuleTypes;
  this->builtinModuleChecker = &all.builtinModuleChecker;
}

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
  bool isBuiltin = Type::fromName(name) || TypeNameTable::isName(name, TypeName::ARRAY);
  bool isInteger = isBuiltin && Type::fromName(name)->isNumeric();
  bool isForTsc = isInteger && alias->type.kind() == TypeRefKind::NAME_REF &&
                  TypeNameTable::isName(alias->type.name, TypeName::NUMBER);
  isForTsc |= isBuiltin && TypeNameTable::isName(name, TypeName::JSON) &&
              alias->type.kind() == TypeRefKind::UNION_REF;
  if (isForTsc) {
    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::DECLARED_FOR_TSC,
        .message = "",
    });
  }
  if (isInteger) {
    std::string message = ErrorTextBuiltinAlias::number(name);

    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::REDEFINES_NUMBER,
        .message = message,
    });
  }
  if (isBuiltin) {
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
      bool isForTsc =
          own.isErr() && own.error().code == TypeDeclarerBuiltinAliasErrorCode::DECLARED_FOR_TSC;
      if (own.isErr() && !isForTsc) {
        this->state.getDiagnostics().error(alias->loc(), own.error().message);
      }
      if (own.isErr()) {
        continue;
      }
    } else if (decl) {
      name = decl->name;
      Scopes::ResultEntryDeclared added =
          this->scopes->declare(name, {EnumEntry{.enumDecl = decl}});
      if (added.isErr()) {
        this->state.getDiagnostics().error(decl->loc(), added.error().message);
      }
      if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
        ScopeEntry previousEntry = this->scopes->lookup(name);
        std::string declaredHere = ErrorTextDeclare::previous();
        this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
      }
    } else if (cls && this->state.getScopeStack().isTopLevel()) {
      name = cls->name;
      this->classDeclarer->declareClass(cls);
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
      std::vector<const Type *> params = this->aliasInstantiator->aliasTypeParams(alias);
      this->aliasInstantiator->instantiateAlias(alias, std::move(params), alias->loc());
    } else if (alias) {
      this->resolveAlias(alias);
    } else if (cls) {
      if (!cls->baseExpr) {
        this->classDeclarer->resolveClass(cls);
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
  const NodeName *imported = this->builtinModuleChecker->nodeName(name);
  if (imported) {
    return this->builtinModuleTypes->exported(imported->module, imported->name);
  }
  if (this->state.getTypeContext().isInsideBuiltinModule()) {
    return this->builtinModuleTypes->alias(name);
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
  if (alias->resolved != nullptr) {
    return alias->resolved;
  }
  if (alias->resolving) {
    return this->resolvingAlias(alias);
  }
  std::vector<AliasFrame> frames;
  this->startAlias(frames, alias);
  while (!frames.empty()) {
    TypeAliasDecl *needed = TypeDeclarer::takeUnresolvedAlias(frames.back());
    if (needed != nullptr) {
      this->startAlias(frames, needed);
    } else {
      this->finishAlias(frames);
    }
  }

  return alias->resolved;
}

const Type *TypeDeclarer::resolvingAlias(TypeAliasDecl *alias) {
  ResultAliasNotCyclic cycle = TypeDeclarer::aliasCycle(alias);
  if (cycle.isOk()) {
    alias->recursive = true;
    return alias->self;
  }
  this->state.getDiagnostics().error(alias->loc(), cycle.error().message);
  alias->resolved = Type::error();

  return alias->resolved;
}

void TypeDeclarer::startAlias(std::vector<AliasFrame> &frames, TypeAliasDecl *alias) {
  std::optional<Module *> previousModule = this->enterHomeModule(alias);
  alias->resolving = true;
  bool isBuiltinModule = this->builtinModuleTypes->alias(alias->name) == alias;
  if (isBuiltinModule) {
    this->state.getTypeContext().enterBuiltinModule();
  }
  alias->self = TypeDeclarer::declareSelf(alias);
  std::vector<TypeAliasDecl *> needed = this->neededAliases(alias);
  frames.push_back(AliasFrame{alias, previousModule, isBuiltinModule, std::move(needed), 0});
}

TypeAliasDecl *TypeDeclarer::takeUnresolvedAlias(AliasFrame &frame) {
  while (frame.nextNeeded < frame.needed.size()) {
    TypeAliasDecl *needed = frame.needed[frame.nextNeeded];
    frame.nextNeeded += 1;
    bool isWaiting = needed->resolved == nullptr && !needed->resolving;
    if (isWaiting) {
      return needed;
    }
  }

  return nullptr;
}

void TypeDeclarer::finishAlias(std::vector<AliasFrame> &frames) {
  AliasFrame frame = std::move(frames.back());
  frames.pop_back();
  TypeAliasDecl *alias = frame.alias;
  const Type *type = this->resolveAliasBody(alias);
  alias->resolving = false;
  if (frame.builtinModule) {
    this->state.getTypeContext().leaveBuiltinModule();
  }
  if (alias->resolved == nullptr) {
    alias->resolved = type;
  }
  this->nameBuiltinUnion(alias);
  if (frame.previousModule.has_value()) {
    this->moduleDeclarer->useModule(*frame.previousModule);
  }
}

std::optional<Module *> TypeDeclarer::enterHomeModule(const Stmt *decl) {
  ModuleStates &modules = this->state.getModuleStates();
  Module *home = modules.findDeclModule(decl);
  Module *here = modules.getCurrentModule();
  bool isElsewhere = home != nullptr && home != here && this->state.getScopeStack().isTopLevel();
  if (!isElsewhere) {
    return std::nullopt;
  }
  this->moduleDeclarer->useModule(home);

  return here;
}

Type *TypeDeclarer::declareSelf(const TypeAliasDecl *alias) {
  TypeRefKind kind = alias->type.kind();
  if (kind == TypeRefKind::OBJECT_REF) {
    return Type::declareObject(alias->name);
  }
  if (kind == TypeRefKind::UNION_REF) {
    return Type::declareUnion(alias->name);
  }

  return nullptr;
}

std::vector<TypeAliasDecl *> TypeDeclarer::neededAliases(const TypeAliasDecl *alias) const {
  std::vector<const TypeRef *> parts;
  for (const TypeRef &base : alias->bases) {
    std::vector<const TypeRef *> baseParts = TypeRefParts::collect(base, kNeededAliasesRoute);
    parts.insert(parts.end(), baseParts.begin(), baseParts.end());
  }
  std::vector<const TypeRef *> bodyParts = TypeRefParts::collect(alias->type, kNeededAliasesRoute);
  parts.insert(parts.end(), bodyParts.begin(), bodyParts.end());
  std::vector<TypeAliasDecl *> needed;
  for (const TypeRef *part : parts) {
    TypeAliasDecl *found = this->plainAliasNamed(*part);
    if (found != nullptr) {
      needed.push_back(found);
    }
  }

  return needed;
}

TypeAliasDecl *TypeDeclarer::plainAliasNamed(const TypeRef &part) const {
  bool isBuiltinName = Type::fromName(part.name) != nullptr;
  if (part.kind() != TypeRefKind::NAME_REF || isBuiltinName) {
    return nullptr;
  }
  auto *alias = dyn_cast_or_null<TypeAliasDecl>(this->lookupType(part.name));
  bool isPlain =
      alias != nullptr && alias->typeParams.empty() && !TypeDeclarer::mentionsTypeOf(alias->type);

  return isPlain ? alias : nullptr;
}

const Type *TypeDeclarer::resolveAliasBody(TypeAliasDecl *alias) {
  TypeRefKind kind = alias->type.kind();
  if (kind == TypeRefKind::OBJECT_REF) {
    return this->resolveObjectAlias(alias);
  }
  if (kind == TypeRefKind::UNION_REF) {
    return this->resolveUnionAlias(alias);
  }

  return this->typeResolver->resolveTypeInner(alias->type);
}

const Type *TypeDeclarer::resolveObjectAlias(TypeAliasDecl *alias) {
  std::vector<const Type *> bases;
  std::vector<Overload> overloads;
  std::vector<Field> fields = alias->bases.empty()
                                  ? this->typeResolver->resolveFields(alias->type, &overloads)
                                  : this->classBases->interfaceFields(alias, bases, overloads);
  const Type *type = nullptr;
  if (alias->recursive) {
    alias->self->defineFields(std::move(fields), std::move(overloads));
    type = alias->self;
  } else {
    type = Type::object(std::move(fields), alias->name, std::move(overloads));
  }
  for (const Type *base : bases) {
    if (!type->isError()) {
      this->classBases->addBase(type, base);
    }
  }

  return type;
}

const Type *TypeDeclarer::resolveUnionAlias(TypeAliasDecl *alias) {
  const Type *type = this->typeResolver->resolveTypeInner(alias->type);
  ResultUnionRecursion recursion = TypeDeclarer::recursiveUnion(alias, type);
  if (recursion.isErr()) {
    this->state.getDiagnostics().error(alias->loc(), recursion.error().message);
    return Type::error();
  }
  if (alias->recursive && !type->isError()) {
    alias->self->defineMembers(type);
    return alias->self;
  }

  return type;
}

void TypeDeclarer::nameBuiltinUnion(const TypeAliasDecl *alias) {
  bool isBuiltin = this->state.getAliasStore().findBuiltinAlias(alias->name) == alias ||
                   this->builtinModuleTypes->alias(alias->name) == alias;
  if (isBuiltin && alias->type.kind() == TypeRefKind::UNION_REF) {
    Type::nameUnion(alias->resolved, alias->name);
  }
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
    this->reportEnumCycle(decl);
    return;
  }
  std::vector<EnumFrame> frames;
  this->startEnum(frames, decl);
  while (!frames.empty()) {
    EnumDecl *needed = this->unresolvedEnum(frames.back());
    if (needed != nullptr) {
      this->startEnum(frames, needed);
    } else {
      this->resolveNextMember(frames);
    }
  }
}

void TypeDeclarer::reportEnumCycle(const EnumDecl *decl) {
  ResultEnumIsResolved resolved = TypeDeclarer::enumCycle(decl);
  if (resolved.isErr()) {
    this->state.getDiagnostics().error(decl->loc(), resolved.error().message);
  }
}

void TypeDeclarer::startEnum(std::vector<EnumFrame> &frames, EnumDecl *decl) {
  std::optional<Module *> previousModule = this->enterHomeModule(decl);
  decl->stage = EnumStage::RESOLVING;
  EnumCount count{0, true, false, false};
  frames.push_back(EnumFrame{decl, previousModule, 0, count});
  this->enterMember(frames.back());
}

void TypeDeclarer::enterMember(const EnumFrame &frame) {
  const std::vector<EnumMember> &members = frame.decl->members;
  if (frame.nextMember == members.size()) {
    return;
  }
  const EnumMember &member = members[frame.nextMember];
  for (size_t earlier = 0; earlier < frame.nextMember; ++earlier) {
    ResultMemberIsUnique unique = TypeDeclarer::uniqueMember(member, members[earlier]);
    if (unique.isErr()) {
      this->state.getDiagnostics().error(member.loc, unique.error().message);
    }
  }
}

EnumDecl *TypeDeclarer::unresolvedEnum(const EnumFrame &frame) const {
  if (frame.nextMember == frame.decl->members.size()) {
    return nullptr;
  }
  const Expr *init = frame.decl->members[frame.nextMember].init;
  std::vector<EnumDecl *> needed = this->constantFolder->enumsNeeded(init, frame.decl);
  for (EnumDecl *other : needed) {
    if (other->stage == EnumStage::UNRESOLVED) {
      return other;
    }
  }

  return nullptr;
}

void TypeDeclarer::resolveNextMember(std::vector<EnumFrame> &frames) {
  EnumFrame &frame = frames.back();
  if (frame.nextMember == frame.decl->members.size()) {
    this->finishEnum(frames);
  } else {
    this->reportNeededCycles(frame);
    this->countMember(frame);
    frame.nextMember += 1;
    this->enterMember(frame);
  }
}

void TypeDeclarer::reportNeededCycles(const EnumFrame &frame) {
  const Expr *init = frame.decl->members[frame.nextMember].init;
  for (const EnumDecl *other : this->constantFolder->enumsNeeded(init, frame.decl)) {
    this->reportEnumCycle(other);
  }
}

void TypeDeclarer::countMember(EnumFrame &frame) {
  EnumMember &m = frame.decl->members[frame.nextMember];
  std::optional<double> value = this->memberNumber(frame, m);
  if (value.has_value()) {
    this->numberMember(frame, m, *value);
  }
}

std::optional<double> TypeDeclarer::memberNumber(EnumFrame &frame, EnumMember &m) {
  if (!m.init) {
    return frame.count.next;
  }
  std::optional<ConstantValue> found =
      this->constantFolder->evalConstant(m.init, frame.decl, frame.nextMember);
  ResultMemberConstant constant = TypeDeclarer::memberConstant(found);
  if (constant.isErr()) {
    this->state.getDiagnostics().error(m.init->loc(), constant.error().message);
    frame.count.counting = false;
    return std::nullopt;
  }
  const ConstantValue &c = constant.value();
  if (c.isString) {
    m.isString = true;
    m.text = c.text;
    frame.count.strings = true;
    frame.count.counting = false;
    return std::nullopt;
  }

  return c.number;
}

void TypeDeclarer::numberMember(EnumFrame &frame, EnumMember &m, double value) {
  ResultMemberHasValue valued = TypeDeclarer::countedMember(m, frame.count.counting);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(m.loc, valued.error().message);
    return;
  }
  ResultMemberIntValue whole = TypeDeclarer::wholeMember(m, value);
  if (whole.isErr()) {
    llvm::SMLoc at = m.init ? m.init->loc() : m.loc;
    this->state.getDiagnostics().error(at, whole.error().message);
    frame.count.counting = false;
    return;
  }
  m.number = static_cast<int64_t>(value);
  frame.count.numbers = true;
  frame.count.next = value + 1;
  frame.count.counting = true;
}

void TypeDeclarer::finishEnum(std::vector<EnumFrame> &frames) {
  EnumFrame frame = frames.back();
  frames.pop_back();
  EnumDecl *decl = frame.decl;
  ResultEnumKindIsSame same =
      TypeDeclarer::enumKinds(decl, frame.count.numbers, frame.count.strings);
  if (same.isErr()) {
    this->state.getDiagnostics().error(decl->loc(), same.error().message);
  }
  decl->isString = frame.count.strings && !frame.count.numbers;
  decl->stage = EnumStage::RESOLVED;
  if (decl->isString) {
    for (const EnumMember &m : decl->members) {
      this->state.getFlowState().markFieldAssigned(ToUtf8::toUtf8(m.text));
    }
  }
  if (frame.previousModule.has_value()) {
    this->moduleDeclarer->useModule(*frame.previousModule);
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

  bool isWritten = member->object && member->object->loc() != member->loc();
  if (m->isString) {
    auto *lit = this->state.getAst().make<StringLitExpr>(member->loc(), m->text);
    if (isWritten) {
      this->state.getInventoryStore().rememberNamedUse(lit,
                                                       {decl, member->object->loc(), member->name});
    }
    const Type *literal = this->literalTypes->stringLiteralFor(expected, m->text);
    lit->type = Type::stringLiteral(m->text);
    if (literal) {
      return ok(lit);
    }
    const Type *type = this->enumType(decl);
    Expr *converted = this->typeRelations->convert(lit, type);

    return ok(converted);
  }
  int64_t value = m->number;
  auto magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  Expr *lit = this->state.getAst().make<IntLitExpr>(member->loc(), magnitude);
  if (isWritten) {
    this->state.getInventoryStore().rememberNamedUse(lit,
                                                     {decl, member->object->loc(), member->name});
  }
  if (value < 0) {
    lit = this->state.getAst().make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
  }
  Expr *checked = this->exprChecker->checkExpr(lit, expected);

  return ok(checked);
}

TypeDeclarer::ResultEnumIndexValue TypeDeclarer::checkEnumIndex(EnumDecl *decl, IndexExpr *index) {
  this->resolveEnum(decl);
  index->type = Type::error();
  index->index = this->exprChecker->checkExpr(index->index, Type::intTy());
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

  index->index = this->typeRelations->convert(index->index, Type::intTy());
  std::optional<Linear> value = this->indexProofs->linear(index->index);
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
