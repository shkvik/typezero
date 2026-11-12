#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
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

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr double kMaxSafeInteger = 9007199254740992.0;

}

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

std::string TypeDeclarer::formatNumber(double value) const {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

TypeDeclarer::ResultAliasIsOwnType TypeDeclarer::builtinAlias(const TypeAliasDecl *alias) const {
  llvm::StringRef name = alias->name;
  TypeRefKind kind = alias->type.kind();
  const Type *named = Type::fromName(name);
  bool isBuiltin = named != nullptr || TypeNameTable::isName(name, TypeName::ARRAY);
  bool isInteger = named != nullptr && named->isNumeric();
  bool isNumberAlias = isInteger && kind == TypeRefKind::NAME_REF &&
                       TypeNameTable::isName(alias->type.name, TypeName::NUMBER);
  bool isJsonUnion =
      isBuiltin && TypeNameTable::isName(name, TypeName::JSON) && kind == TypeRefKind::UNION_REF;
  if (isNumberAlias || isJsonUnion) {
    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::DECLARED_FOR_TSC,
        .message = "",
    });
  }
  if (isInteger) {
    auto message = ErrorTextBuiltinAlias::number(name);

    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::REDEFINES_NUMBER,
        .message = message,
    });
  }
  if (isBuiltin) {
    auto message = ErrorTextBuiltinAlias::builtin(name);

    return err(TypeDeclarerBuiltinAliasError{
        .code = TypeDeclarerBuiltinAliasErrorCode::REDEFINES_BUILTIN,
        .message = message,
    });
  }

  return ok(alias);
}

TypeDeclarer::ResultTypeNameIsFree TypeDeclarer::freeTypeName(llvm::StringRef name) const {
  if (this->state.getScopeStack().hasTypeInCurrentScope(name)) {
    auto message = ErrorTextFreeTypeName::duplicate(name);

    return err(TypeDeclarerFreeTypeNameError{
        .code = TypeDeclarerFreeTypeNameErrorCode::DUPLICATE_TYPE,
        .message = message,
    });
  }

  return ok(name);
}

std::vector<Stmt *> TypeDeclarer::declareTypes(llvm::ArrayRef<Stmt *> stmts) {
  auto declared = this->declareTypeNames(stmts);
  this->resolveTypes(declared);

  return declared;
}

std::vector<Stmt *> TypeDeclarer::declareTypeNames(llvm::ArrayRef<Stmt *> stmts) {
  auto declared = std::vector<Stmt *>();
  for (Stmt *stmt : stmts) {
    auto name = this->declaredTypeName(stmt);
    if (name.has_value()) {
      this->addNamedType(*name, stmt, declared);
    }
  }

  return declared;
}

std::optional<llvm::StringRef> TypeDeclarer::declaredTypeName(Stmt *stmt) {
  auto *alias = dyn_cast<TypeAliasDecl>(stmt);
  auto *enumDecl = dyn_cast<EnumDecl>(stmt);
  auto *classDecl = dyn_cast<ClassDecl>(stmt);
  if (alias != nullptr) {
    return this->ownAliasName(alias);
  }
  if (enumDecl != nullptr) {
    return this->declareEnumName(enumDecl);
  }
  if (classDecl != nullptr && this->state.getScopeStack().isTopLevel()) {
    this->classDeclarer->declareClass(classDecl);

    return llvm::StringRef(classDecl->name);
  }

  return std::nullopt;
}

std::optional<llvm::StringRef> TypeDeclarer::ownAliasName(const TypeAliasDecl *alias) {
  auto own = this->builtinAlias(alias);
  if (own.isOk()) {
    return llvm::StringRef(alias->name);
  }
  if (own.error().code != TypeDeclarerBuiltinAliasErrorCode::DECLARED_FOR_TSC) {
    this->state.getDiagnostics().error(alias->loc(), own.error().message);
  }

  return std::nullopt;
}

llvm::StringRef TypeDeclarer::declareEnumName(EnumDecl *enumDecl) {
  llvm::StringRef name = enumDecl->name;
  auto added = this->scopes->declare(name, {EnumEntry{.enumDecl = enumDecl}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(enumDecl->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->scopes->lookup(name);
    auto declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }

  return name;
}

void TypeDeclarer::addNamedType(llvm::StringRef name, Stmt *stmt, std::vector<Stmt *> &declared) {
  auto unused = this->freeTypeName(name);
  if (unused.isErr()) {
    auto *previous = this->state.getScopeStack().findTypeInCurrentScope(name);
    auto previousText = ErrorTextFreeTypeName::previous();
    this->state.getDiagnostics().error(stmt->loc(), unused.error().message);
    this->state.getDiagnostics().note(previous->loc(), previousText);
    return;
  }
  this->state.getScopeStack().addType(name, stmt);
  declared.push_back(stmt);
}

bool TypeDeclarer::isMentioningTypeOf(const TypeRef &ref) const {
  auto pending = std::vector<const TypeRef *>{&ref};
  while (!pending.empty()) {
    const TypeRef *part = pending.back();
    pending.pop_back();
    if (part->kind() == TypeRefKind::TYPE_OF_REF) {
      return true;
    }
    for (const TypeRef &arg : part->args) {
      pending.push_back(&arg);
    }
  }

  return false;
}

void TypeDeclarer::resolveTypes(llvm::ArrayRef<Stmt *> declared) {
  for (Stmt *stmt : declared) {
    this->resolveDeclared(stmt);
  }
}

void TypeDeclarer::resolveDeclared(Stmt *stmt) {
  auto *alias = dyn_cast<TypeAliasDecl>(stmt);
  auto *classDecl = dyn_cast<ClassDecl>(stmt);
  auto *enumDecl = dyn_cast<EnumDecl>(stmt);
  if (alias != nullptr) {
    this->resolveDeclaredAlias(alias);
  }
  if (classDecl != nullptr && classDecl->baseExpr == nullptr) {
    this->classDeclarer->resolveClass(classDecl);
  }
  if (enumDecl != nullptr) {
    this->resolveEnum(enumDecl);
  }
}

void TypeDeclarer::resolveDeclaredAlias(TypeAliasDecl *alias) {
  if (this->isMentioningTypeOf(alias->type)) {
    return;
  }
  if (alias->typeParams.empty()) {
    this->resolveAlias(alias);
    return;
  }
  auto params = this->aliasInstantiator->aliasTypeParams(alias);
  this->aliasInstantiator->instantiateAlias(alias, std::move(params), alias->loc());
}

Stmt *TypeDeclarer::lookupType(llvm::StringRef name) const {
  auto *found = this->state.getScopeStack().findType(name);
  if (found != nullptr) {
    return found;
  }
  auto *alias = this->state.getAliasStore().findBuiltinAlias(name);
  if (alias != nullptr) {
    return alias;
  }
  const auto *imported = this->builtinModuleChecker->nodeName(name);
  if (imported != nullptr) {
    return this->builtinModuleTypes->exported(imported->module, imported->name);
  }
  if (this->state.getTypeContext().isInsideBuiltinModule()) {
    return this->builtinModuleTypes->alias(name);
  }

  return nullptr;
}

TypeDeclarer::ResultAliasNotCyclic TypeDeclarer::aliasCycle(const TypeAliasDecl *alias) const {
  if (alias->resolving && alias->self == nullptr) {
    auto message = ErrorTextAliasCycle::itself(alias->name);

    return err(TypeDeclarerAliasCycleError{
        .code = TypeDeclarerAliasCycleErrorCode::REFERS_TO_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

TypeDeclarer::ResultUnionRecursion TypeDeclarer::recursiveUnion(const TypeAliasDecl *alias,
                                                                const Type *type) const {
  if (alias->recursive && !type->isError() && !type->isUnion()) {
    auto message = ErrorTextRecursiveUnion::notUnion(alias->name);

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
  auto frames = std::vector<AliasFrame>();
  this->startAlias(frames, alias);
  while (!frames.empty()) {
    auto *needed = this->takeUnresolvedAlias(frames.back());
    if (needed != nullptr) {
      this->startAlias(frames, needed);
    } else {
      this->finishAlias(frames);
    }
  }

  return alias->resolved;
}

const Type *TypeDeclarer::resolvingAlias(TypeAliasDecl *alias) {
  auto cycle = this->aliasCycle(alias);
  if (cycle.isOk()) {
    alias->recursive = true;

    return alias->self;
  }
  this->state.getDiagnostics().error(alias->loc(), cycle.error().message);
  alias->resolved = Type::error();

  return alias->resolved;
}

void TypeDeclarer::startAlias(std::vector<AliasFrame> &frames, TypeAliasDecl *alias) {
  auto previousModule = this->enterHomeModule(alias);
  alias->resolving = true;
  bool isBuiltinModule = this->builtinModuleTypes->alias(alias->name) == alias;
  if (isBuiltinModule) {
    this->state.getTypeContext().enterBuiltinModule();
  }
  alias->self = this->declareSelf(alias);
  auto needed = this->neededAliases(alias);
  auto frame = AliasFrame{
      .alias = alias,
      .previousModule = previousModule,
      .isBuiltinModule = isBuiltinModule,
      .needed = std::move(needed),
      .nextNeeded = 0,
  };
  frames.push_back(std::move(frame));
}

TypeAliasDecl *TypeDeclarer::takeUnresolvedAlias(AliasFrame &frame) const {
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
  const auto *type = this->resolveAliasBody(alias);
  alias->resolving = false;
  if (frame.isBuiltinModule) {
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

std::optional<Module *> TypeDeclarer::enterHomeModule(const Stmt *declared) {
  auto &modules = this->state.getModuleStates();
  auto *home = modules.findDeclModule(declared);
  auto *here = modules.getCurrentModule();
  bool isElsewhere = home != nullptr && home != here && this->state.getScopeStack().isTopLevel();
  if (!isElsewhere) {
    return std::nullopt;
  }
  this->moduleDeclarer->useModule(home);

  return here;
}

Type *TypeDeclarer::declareSelf(const TypeAliasDecl *alias) const {
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
  auto parts = std::vector<const TypeRef *>();
  for (const TypeRef &base : alias->bases) {
    auto baseParts = TypeRefParts::collect(base, kNeededAliasesRoute);
    parts.insert(parts.end(), baseParts.begin(), baseParts.end());
  }
  auto bodyParts = TypeRefParts::collect(alias->type, kNeededAliasesRoute);
  parts.insert(parts.end(), bodyParts.begin(), bodyParts.end());
  auto needed = std::vector<TypeAliasDecl *>();
  for (const TypeRef *part : parts) {
    auto *found = this->plainAliasNamed(*part);
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
  auto *found = this->lookupType(part.name);
  auto *alias = dyn_cast_or_null<TypeAliasDecl>(found);
  if (alias == nullptr || !alias->typeParams.empty() || this->isMentioningTypeOf(alias->type)) {
    return nullptr;
  }

  return alias;
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
  auto bases = std::vector<const Type *>();
  auto overloads = std::vector<Overload>();
  std::vector<Field> fields = alias->bases.empty()
                                  ? this->typeResolver->resolveFields(alias->type, &overloads)
                                  : this->classBases->interfaceFields(alias, bases, overloads);
  const Type *type = this->definedObject(alias, std::move(fields), std::move(overloads));
  for (const Type *base : bases) {
    if (!type->isError()) {
      this->classBases->addBase(type, base);
    }
  }

  return type;
}

const Type *TypeDeclarer::definedObject(const TypeAliasDecl *alias, std::vector<Field> fields,
                                        std::vector<Overload> overloads) const {
  if (alias->recursive) {
    alias->self->defineFields(std::move(fields), std::move(overloads));

    return alias->self;
  }

  return Type::object(std::move(fields), alias->name, std::move(overloads));
}

const Type *TypeDeclarer::resolveUnionAlias(TypeAliasDecl *alias) {
  const auto *type = this->typeResolver->resolveTypeInner(alias->type);
  auto recursion = this->recursiveUnion(alias, type);
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

TypeDeclarer::ResultEnumIsResolved TypeDeclarer::enumCycle(const EnumDecl *enumDecl) const {
  if (enumDecl->stage == EnumStage::RESOLVING) {
    auto message = ErrorTextEnumCycle::itself(enumDecl->name);

    return err(TypeDeclarerEnumCycleError{
        .code = TypeDeclarerEnumCycleErrorCode::DEPENDS_ON_ITSELF,
        .message = message,
    });
  }

  return ok(enumDecl);
}

TypeDeclarer::ResultMemberIsUnique TypeDeclarer::uniqueMember(const EnumMember &member,
                                                              const EnumMember &other) const {
  if (other.name == member.name) {
    auto message = ErrorTextUniqueMember::duplicate(member.name);

    return err(TypeDeclarerUniqueMemberError{
        .code = TypeDeclarerUniqueMemberErrorCode::DUPLICATE_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

TypeDeclarer::ResultMemberConstant
TypeDeclarer::memberConstant(std::optional<ConstantValue> constant) const {
  if (!constant.has_value()) {
    auto message = ErrorTextMemberConstant::notConst();

    return err(TypeDeclarerMemberConstantError{
        .code = TypeDeclarerMemberConstantErrorCode::NOT_CONSTANT,
        .message = message,
    });
  }

  return ok(*constant);
}

TypeDeclarer::ResultMemberHasValue TypeDeclarer::countedMember(const EnumMember &member,
                                                               bool isCounting) const {
  if (member.init == nullptr && !isCounting) {
    auto message = ErrorTextCountedMember::noValue(member.name);

    return err(TypeDeclarerCountedMemberError{
        .code = TypeDeclarerCountedMemberErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&member);
}

TypeDeclarer::ResultMemberIntValue TypeDeclarer::wholeMember(const EnumMember &member,
                                                             double value) const {
  if (value != std::trunc(value) || std::fabs(value) > kMaxSafeInteger) {
    auto shown = this->formatNumber(value);
    auto message = ErrorTextWholeMember::notWhole(member.name, shown);

    return err(TypeDeclarerWholeMemberError{
        .code = TypeDeclarerWholeMemberErrorCode::NOT_WHOLE,
        .message = message,
    });
  }

  return ok(value);
}

TypeDeclarer::ResultEnumKindIsSame TypeDeclarer::enumKinds(const EnumDecl *enumDecl,
                                                           const EnumCount &count) const {
  if (count.hasNumbers && count.hasStrings) {
    auto message = ErrorTextEnumKinds::mixed(enumDecl->name);

    return err(TypeDeclarerEnumKindsError{
        .code = TypeDeclarerEnumKindsErrorCode::MIXED,
        .message = message,
    });
  }

  return ok(enumDecl);
}

void TypeDeclarer::resolveEnum(EnumDecl *enumDecl) {
  if (enumDecl->stage != EnumStage::UNRESOLVED) {
    this->reportEnumCycle(enumDecl);
    return;
  }
  auto frames = std::vector<EnumFrame>();
  this->startEnum(frames, enumDecl);
  while (!frames.empty()) {
    auto *needed = this->unresolvedEnum(frames.back());
    if (needed != nullptr) {
      this->startEnum(frames, needed);
    } else {
      this->resolveNextMember(frames);
    }
  }
}

void TypeDeclarer::reportEnumCycle(const EnumDecl *enumDecl) {
  auto resolved = this->enumCycle(enumDecl);
  if (resolved.isErr()) {
    this->state.getDiagnostics().error(enumDecl->loc(), resolved.error().message);
  }
}

void TypeDeclarer::startEnum(std::vector<EnumFrame> &frames, EnumDecl *enumDecl) {
  auto previousModule = this->enterHomeModule(enumDecl);
  enumDecl->stage = EnumStage::RESOLVING;
  auto count = EnumCount{
      .next = 0,
      .isCounting = true,
      .hasNumbers = false,
      .hasStrings = false,
  };
  auto frame = EnumFrame{
      .enumDecl = enumDecl,
      .previousModule = previousModule,
      .nextMember = 0,
      .count = count,
  };
  frames.push_back(frame);
  this->enterMember(frames.back());
}

void TypeDeclarer::enterMember(const EnumFrame &frame) {
  const std::vector<EnumMember> &members = frame.enumDecl->members;
  if (frame.nextMember == members.size()) {
    return;
  }
  const EnumMember &member = members[frame.nextMember];
  auto earlier = llvm::ArrayRef<EnumMember>(members).take_front(frame.nextMember);
  for (const EnumMember &other : earlier) {
    auto unique = this->uniqueMember(member, other);
    if (unique.isErr()) {
      this->state.getDiagnostics().error(member.loc, unique.error().message);
    }
  }
}

EnumDecl *TypeDeclarer::unresolvedEnum(const EnumFrame &frame) const {
  if (frame.nextMember == frame.enumDecl->members.size()) {
    return nullptr;
  }
  const Expr *init = frame.enumDecl->members[frame.nextMember].init;
  auto needed = this->constantFolder->enumsNeeded(init, frame.enumDecl);
  for (EnumDecl *other : needed) {
    if (other->stage == EnumStage::UNRESOLVED) {
      return other;
    }
  }

  return nullptr;
}

void TypeDeclarer::resolveNextMember(std::vector<EnumFrame> &frames) {
  EnumFrame &frame = frames.back();
  if (frame.nextMember == frame.enumDecl->members.size()) {
    return this->finishEnum(frames);
  }
  this->reportNeededCycles(frame);
  this->countMember(frame);
  frame.nextMember += 1;

  return this->enterMember(frame);
}

void TypeDeclarer::reportNeededCycles(const EnumFrame &frame) {
  const Expr *init = frame.enumDecl->members[frame.nextMember].init;
  for (const EnumDecl *other : this->constantFolder->enumsNeeded(init, frame.enumDecl)) {
    this->reportEnumCycle(other);
  }
}

void TypeDeclarer::countMember(EnumFrame &frame) {
  EnumMember &member = frame.enumDecl->members[frame.nextMember];
  auto value = this->memberNumber(frame, member);
  if (value.has_value()) {
    this->numberMember(frame, member, *value);
  }
}

std::optional<double> TypeDeclarer::memberNumber(EnumFrame &frame, EnumMember &member) {
  if (member.init == nullptr) {
    return frame.count.next;
  }
  auto found = this->constantFolder->evalConstant(member.init, frame.enumDecl, frame.nextMember);
  auto constant = this->memberConstant(found);
  if (constant.isErr()) {
    this->state.getDiagnostics().error(member.init->loc(), constant.error().message);
    frame.count.isCounting = false;

    return std::nullopt;
  }
  const ConstantValue &value = constant.value();
  if (value.isString) {
    member.isString = true;
    member.text = value.text;
    frame.count.hasStrings = true;
    frame.count.isCounting = false;

    return std::nullopt;
  }

  return value.number;
}

void TypeDeclarer::numberMember(EnumFrame &frame, EnumMember &member, double value) {
  auto valued = this->countedMember(member, frame.count.isCounting);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(member.loc, valued.error().message);
    return;
  }
  auto whole = this->wholeMember(member, value);
  if (whole.isErr()) {
    llvm::SMLoc at = member.init != nullptr ? member.init->loc() : member.loc;
    this->state.getDiagnostics().error(at, whole.error().message);
    frame.count.isCounting = false;
    return;
  }
  member.number = static_cast<int64_t>(value);
  frame.count.hasNumbers = true;
  frame.count.next = value + 1;
  frame.count.isCounting = true;
}

void TypeDeclarer::finishEnum(std::vector<EnumFrame> &frames) {
  EnumFrame frame = frames.back();
  frames.pop_back();
  EnumDecl *enumDecl = frame.enumDecl;
  auto same = this->enumKinds(enumDecl, frame.count);
  if (same.isErr()) {
    this->state.getDiagnostics().error(enumDecl->loc(), same.error().message);
  }
  enumDecl->isString = frame.count.hasStrings && !frame.count.hasNumbers;
  enumDecl->stage = EnumStage::RESOLVED;
  if (enumDecl->isString) {
    this->markTextsAssigned(enumDecl);
  }
  if (frame.previousModule.has_value()) {
    this->moduleDeclarer->useModule(*frame.previousModule);
  }
}

void TypeDeclarer::markTextsAssigned(const EnumDecl *enumDecl) {
  for (const EnumMember &member : enumDecl->members) {
    auto text = ToUtf8::toUtf8(member.text);
    this->state.getFlowState().markFieldAssigned(text);
  }
}

const Type *TypeDeclarer::enumType(EnumDecl *enumDecl) {
  this->resolveEnum(enumDecl);
  if (!enumDecl->isString) {
    return Type::intTy();
  }
  auto values = std::vector<const Type *>();
  for (const EnumMember &member : enumDecl->members) {
    if (member.isString) {
      values.push_back(Type::stringLiteral(member.text));
    }
  }
  if (values.empty()) {
    return Type::string();
  }
  const auto *type = Type::unionOf(values);
  Type::nameUnion(type, enumDecl->name);

  return type;
}

TypeDeclarer::ResultEnumMemberRead
TypeDeclarer::checkEnumMember(EnumDecl *enumDecl, MemberExpr *member, const Type *expected) {
  this->resolveEnum(enumDecl);
  const auto *enumMember = enumDecl->find(member->name);
  if (enumMember == nullptr) {
    auto message = ErrorTextEnumMember::missing(member->name, enumDecl->name);

    return err(TypeDeclarerCheckEnumMemberError{
        .code = TypeDeclarerCheckEnumMemberErrorCode::NO_SUCH_MEMBER,
        .message = message,
    });
  }

  if (enumMember->isString) {
    auto *text = this->textMemberValue(enumDecl, member, *enumMember, expected);

    return ok(text);
  }
  auto *number = this->numberMemberValue(enumDecl, member, *enumMember, expected);

  return ok(number);
}

void TypeDeclarer::rememberMemberUse(const EnumDecl *enumDecl, const MemberExpr *member,
                                     const Expr *value) {
  bool isWritten = member->object != nullptr && member->object->loc() != member->loc();
  if (!isWritten) {
    return;
  }
  auto named = NamedUse{
      .decl = enumDecl,
      .name = member->object->loc(),
      .member = member->name,
  };
  this->state.getInventoryStore().rememberNamedUse(value, named);
}

Expr *TypeDeclarer::textMemberValue(EnumDecl *enumDecl, const MemberExpr *member,
                                    const EnumMember &enumMember, const Type *expected) {
  auto *literalExpr = this->state.getAst().make<StringLitExpr>(member->loc(), enumMember.text);
  this->rememberMemberUse(enumDecl, member, literalExpr);
  const auto *literal = this->literalTypes->stringLiteralFor(expected, enumMember.text);
  literalExpr->type = Type::stringLiteral(enumMember.text);
  if (literal != nullptr) {
    return literalExpr;
  }
  const auto *type = this->enumType(enumDecl);

  return this->typeRelations->convert(literalExpr, type);
}

Expr *TypeDeclarer::numberMemberValue(const EnumDecl *enumDecl, const MemberExpr *member,
                                      const EnumMember &enumMember, const Type *expected) {
  auto &ast = this->state.getAst();
  int64_t value = enumMember.number;
  auto magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  Expr *size = ast.make<IntLitExpr>(member->loc(), magnitude);
  this->rememberMemberUse(enumDecl, member, size);
  if (value < 0) {
    Expr *negative = ast.make<UnaryExpr>(member->loc(), UnaryOp::Neg, size);

    return this->exprChecker->checkExpr(negative, expected);
  }

  return this->exprChecker->checkExpr(size, expected);
}

TypeDeclarer::ResultEnumIndexValue TypeDeclarer::checkEnumIndex(EnumDecl *enumDecl,
                                                                IndexExpr *index) {
  this->resolveEnum(enumDecl);
  index->type = Type::error();
  index->index = this->exprChecker->checkExpr(index->index, Type::intTy());
  const Type *type = index->index->type;
  if (type->isError()) {
    return err(TypeDeclarerCheckEnumIndexError{
        .code = TypeDeclarerCheckEnumIndexErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (enumDecl->isString) {
    auto message = ErrorTextEnumIndex::strings(enumDecl->name);

    return err(TypeDeclarerCheckEnumIndexError{
        .code = TypeDeclarerCheckEnumIndexErrorCode::STRING_ENUM,
        .message = message,
    });
  }
  if (!type->isInteger()) {
    auto message = ErrorTextEnumIndex::notInteger(type->name());

    return err(TypeDeclarerCheckEnumIndexError{
        .code = TypeDeclarerCheckEnumIndexErrorCode::NOT_AN_INTEGER,
        .message = message,
    });
  }

  index->index = this->typeRelations->convert(index->index, Type::intTy());
  auto value = this->indexProofs->linear(index->index);
  if (value.has_value() && value->term.isZero()) {
    auto *name = this->memberNameAt(enumDecl, value->offset, index->loc());

    return ok(name);
  }
  index->form = EnumNameIndex{.enumDecl = enumDecl};
  index->type = Type::optional(Type::string());

  return ok<Expr *>(index);
}

Expr *TypeDeclarer::memberNameAt(const EnumDecl *enumDecl, int64_t number, llvm::SMLoc loc) {
  const EnumMember *found = this->lastMemberNumbered(enumDecl, number);
  if (found == nullptr) {
    auto *undefined = this->state.getAst().make<UndefinedExpr>(loc);
    undefined->type = Type::undefinedTy();

    return undefined;
  }
  auto name = ToUtf16::toUtf16(found->name);
  auto *literal = this->state.getAst().make<StringLitExpr>(loc, name);
  literal->type = Type::string();

  return literal;
}

const EnumMember *TypeDeclarer::lastMemberNumbered(const EnumDecl *enumDecl, int64_t number) const {
  for (const EnumMember &member : llvm::reverse(enumDecl->members)) {
    if (member.number == number) {
      return &member;
    }
  }

  return nullptr;
}

}
