#include "compiler/semantic/declaration/types/declaration_types_registry.h"
#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/declaration/classes/declaration_classes_registry.h"
#include "compiler/semantic/declaration/modules/declaration_modules_registry.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

DeclarationTypesRegistry::DeclarationTypesRegistry(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeResolver = &all.typeResolver;
  this->classes = &all.declaration.classes;
  this->modules = &all.declaration.modules;
  this->aliasInstantiator = &all.aliasInstantiator;
}

std::string DeclarationTypesRegistry::formatNumber(double value) const {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

DeclarationTypesRegistry::ResultAliasIsOwnType
DeclarationTypesRegistry::builtinAlias(const TypeAliasDecl *alias) const {
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
    return err(DeclarationTypesRegistryBuiltinAliasError{
        .code = DeclarationTypesRegistryBuiltinAliasErrorCode::DECLARED_FOR_TSC,
        .message = "",
    });
  }
  if (isInteger) {
    auto message = ErrorTextBuiltinAlias::number(name);

    return err(DeclarationTypesRegistryBuiltinAliasError{
        .code = DeclarationTypesRegistryBuiltinAliasErrorCode::REDEFINES_NUMBER,
        .message = message,
    });
  }
  if (isBuiltin) {
    auto message = ErrorTextBuiltinAlias::builtin(name);

    return err(DeclarationTypesRegistryBuiltinAliasError{
        .code = DeclarationTypesRegistryBuiltinAliasErrorCode::REDEFINES_BUILTIN,
        .message = message,
    });
  }

  return ok(alias);
}

DeclarationTypesRegistry::ResultTypeNameIsFree
DeclarationTypesRegistry::freeTypeName(llvm::StringRef name) const {
  if (this->state.walk.scopes.hasTypeInCurrentScope(name)) {
    auto message = ErrorTextFreeTypeName::duplicate(name);

    return err(DeclarationTypesRegistryFreeTypeNameError{
        .code = DeclarationTypesRegistryFreeTypeNameErrorCode::DUPLICATE_TYPE,
        .message = message,
    });
  }

  return ok(name);
}

std::vector<Stmt *> DeclarationTypesRegistry::declareTypes(llvm::ArrayRef<Stmt *> stmts) {
  auto declared = this->declareTypeNames(stmts);
  this->resolveTypes(declared);

  return declared;
}

std::vector<Stmt *> DeclarationTypesRegistry::declareTypeNames(llvm::ArrayRef<Stmt *> stmts) {
  auto declared = std::vector<Stmt *>();
  for (Stmt *stmt : stmts) {
    auto name = this->declaredTypeName(stmt);
    if (name.has_value()) {
      this->addNamedType(*name, stmt, declared);
    }
  }

  return declared;
}

std::optional<llvm::StringRef> DeclarationTypesRegistry::declaredTypeName(Stmt *stmt) {
  auto *alias = dyn_cast<TypeAliasDecl>(stmt);
  auto *enumDecl = dyn_cast<EnumDecl>(stmt);
  auto *classDecl = dyn_cast<ClassDecl>(stmt);
  if (alias != nullptr) {
    return this->ownAliasName(alias);
  }
  if (enumDecl != nullptr) {
    return this->declareEnumName(enumDecl);
  }
  if (classDecl != nullptr && this->state.walk.scopes.isTopLevel()) {
    this->classes->declareClass(classDecl);

    return llvm::StringRef(classDecl->name);
  }

  return std::nullopt;
}

std::optional<llvm::StringRef> DeclarationTypesRegistry::ownAliasName(const TypeAliasDecl *alias) {
  auto own = this->builtinAlias(alias);
  if (own.isOk()) {
    return llvm::StringRef(alias->name);
  }
  if (own.error().code != DeclarationTypesRegistryBuiltinAliasErrorCode::DECLARED_FOR_TSC) {
    this->state.diagnostics.error(alias->loc(), own.error().message);
  }

  return std::nullopt;
}

llvm::StringRef DeclarationTypesRegistry::declareEnumName(EnumDecl *enumDecl) {
  llvm::StringRef name = enumDecl->name;
  auto added = this->state.walk.scopes.declare(name, {EnumEntry{.enumDecl = enumDecl}});
  if (added.isErr()) {
    this->state.diagnostics.error(enumDecl->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopeStackContextDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->state.walk.scopes.findName(name);
    auto declaredHere = ErrorTextDeclareName::previous();
    this->state.diagnostics.note(previousEntry.loc(), declaredHere);
  }

  return name;
}

void DeclarationTypesRegistry::addNamedType(llvm::StringRef name, Stmt *stmt,
                                            std::vector<Stmt *> &declared) {
  auto unused = this->freeTypeName(name);
  if (unused.isErr()) {
    auto *previous = this->state.walk.scopes.findTypeInCurrentScope(name);
    auto previousText = ErrorTextFreeTypeName::previous();
    this->state.diagnostics.error(stmt->loc(), unused.error().message);
    this->state.diagnostics.note(previous->loc(), previousText);
    return;
  }
  this->state.walk.scopes.addType(name, stmt);
  declared.push_back(stmt);
}

bool DeclarationTypesRegistry::isMentioningTypeOf(const TypeRef &ref) const {
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

void DeclarationTypesRegistry::resolveTypes(llvm::ArrayRef<Stmt *> declared) {
  for (Stmt *stmt : declared) {
    this->resolveDeclared(stmt);
  }
}

void DeclarationTypesRegistry::resolveDeclared(Stmt *stmt) {
  auto *alias = dyn_cast<TypeAliasDecl>(stmt);
  auto *classDecl = dyn_cast<ClassDecl>(stmt);
  auto *enumDecl = dyn_cast<EnumDecl>(stmt);
  if (alias != nullptr) {
    this->resolveDeclaredAlias(alias);
  }
  if (classDecl != nullptr && classDecl->baseExpr == nullptr) {
    this->classes->resolveClass(classDecl);
  }
  if (enumDecl != nullptr) {
    this->resolveEnum(enumDecl);
  }
}

void DeclarationTypesRegistry::resolveDeclaredAlias(TypeAliasDecl *alias) {
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

DeclarationTypesRegistry::ResultAliasNotCyclic
DeclarationTypesRegistry::aliasCycle(const TypeAliasDecl *alias) const {
  if (alias->resolving && alias->self == nullptr) {
    auto message = ErrorTextAliasCycle::itself(alias->name);

    return err(DeclarationTypesRegistryAliasCycleError{
        .code = DeclarationTypesRegistryAliasCycleErrorCode::REFERS_TO_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

DeclarationTypesRegistry::ResultUnionRecursion
DeclarationTypesRegistry::recursiveUnion(const TypeAliasDecl *alias, const Type *type) const {
  if (alias->recursive && !type->isError() && !type->isUnion()) {
    auto message = ErrorTextRecursiveUnion::notUnion(alias->name);

    return err(DeclarationTypesRegistryRecursiveUnionError{
        .code = DeclarationTypesRegistryRecursiveUnionErrorCode::NOT_A_UNION,
        .message = message,
    });
  }

  return ok(type);
}

const Type *DeclarationTypesRegistry::resolveAlias(TypeAliasDecl *alias) {
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

const Type *DeclarationTypesRegistry::resolvingAlias(TypeAliasDecl *alias) {
  auto cycle = this->aliasCycle(alias);
  if (cycle.isOk()) {
    alias->recursive = true;

    return alias->self;
  }
  this->state.diagnostics.error(alias->loc(), cycle.error().message);
  alias->resolved = Type::error();

  return alias->resolved;
}

void DeclarationTypesRegistry::startAlias(std::vector<AliasFrame> &frames, TypeAliasDecl *alias) {
  auto previousModule = this->enterHomeModule(alias);
  alias->resolving = true;
  bool isBuiltinModule = this->state.library.declarations.findModuleAlias(alias->name) == alias;
  if (isBuiltinModule) {
    this->state.declaration.modules.enterBuiltinModule();
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

TypeAliasDecl *DeclarationTypesRegistry::takeUnresolvedAlias(AliasFrame &frame) const {
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

void DeclarationTypesRegistry::finishAlias(std::vector<AliasFrame> &frames) {
  AliasFrame frame = std::move(frames.back());
  frames.pop_back();
  TypeAliasDecl *alias = frame.alias;
  const auto *type = this->resolveAliasBody(alias);
  alias->resolving = false;
  if (frame.isBuiltinModule) {
    this->state.declaration.modules.leaveBuiltinModule();
  }
  if (alias->resolved == nullptr) {
    alias->resolved = type;
  }
  this->nameBuiltinUnion(alias);
  if (frame.previousModule.has_value()) {
    this->modules->useModule(*frame.previousModule);
  }
}

std::optional<Module *> DeclarationTypesRegistry::enterHomeModule(const Stmt *declared) {
  auto &modules = this->state.declaration.modules;
  auto *home = modules.findDeclModule(declared);
  auto *here = modules.getCurrentModule();
  bool isElsewhere = home != nullptr && home != here && this->state.walk.scopes.isTopLevel();
  if (!isElsewhere) {
    return std::nullopt;
  }
  this->modules->useModule(home);

  return here;
}

Type *DeclarationTypesRegistry::declareSelf(const TypeAliasDecl *alias) const {
  TypeRefKind kind = alias->type.kind();
  if (kind == TypeRefKind::OBJECT_REF) {
    return Type::declareObject(alias->name);
  }
  if (kind == TypeRefKind::UNION_REF) {
    return Type::declareUnion(alias->name);
  }

  return nullptr;
}

std::vector<TypeAliasDecl *>
DeclarationTypesRegistry::neededAliases(const TypeAliasDecl *alias) const {
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

TypeAliasDecl *DeclarationTypesRegistry::plainAliasNamed(const TypeRef &part) const {
  bool isBuiltinName = Type::fromName(part.name) != nullptr;
  if (part.kind() != TypeRefKind::NAME_REF || isBuiltinName) {
    return nullptr;
  }
  auto *found = this->state.findTypeDeclaration(part.name);
  auto *alias = dyn_cast_or_null<TypeAliasDecl>(found);
  if (alias == nullptr || !alias->typeParams.empty() || this->isMentioningTypeOf(alias->type)) {
    return nullptr;
  }

  return alias;
}

const Type *DeclarationTypesRegistry::resolveAliasBody(TypeAliasDecl *alias) {
  TypeRefKind kind = alias->type.kind();
  if (kind == TypeRefKind::OBJECT_REF) {
    return this->resolveObjectAlias(alias);
  }
  if (kind == TypeRefKind::UNION_REF) {
    return this->resolveUnionAlias(alias);
  }

  return this->typeResolver->resolveTypeInner(alias->type);
}

const Type *DeclarationTypesRegistry::resolveObjectAlias(TypeAliasDecl *alias) {
  auto bases = std::vector<const Type *>();
  auto overloads = std::vector<Overload>();
  std::vector<Field> fields = alias->bases.empty()
                                  ? this->typeResolver->resolveFields(alias->type, &overloads)
                                  : this->classes->interfaceFields(alias, bases, overloads);
  const Type *type = this->definedObject(alias, std::move(fields), std::move(overloads));
  for (const Type *base : bases) {
    if (!type->isError()) {
      this->classes->addBase(type, base);
    }
  }

  return type;
}

const Type *DeclarationTypesRegistry::definedObject(const TypeAliasDecl *alias,
                                                    std::vector<Field> fields,
                                                    std::vector<Overload> overloads) const {
  if (alias->recursive) {
    alias->self->defineFields(std::move(fields), std::move(overloads));

    return alias->self;
  }

  return Type::object(std::move(fields), alias->name, std::move(overloads));
}

const Type *DeclarationTypesRegistry::resolveUnionAlias(TypeAliasDecl *alias) {
  const auto *type = this->typeResolver->resolveTypeInner(alias->type);
  auto recursion = this->recursiveUnion(alias, type);
  if (recursion.isErr()) {
    this->state.diagnostics.error(alias->loc(), recursion.error().message);

    return Type::error();
  }
  if (alias->recursive && !type->isError()) {
    alias->self->defineMembers(type);

    return alias->self;
  }

  return type;
}

void DeclarationTypesRegistry::nameBuiltinUnion(const TypeAliasDecl *alias) {
  bool isBuiltin = this->state.declaration.prelude.findBuiltinAlias(alias->name) == alias ||
                   this->state.library.declarations.findModuleAlias(alias->name) == alias;
  if (isBuiltin && alias->type.kind() == TypeRefKind::UNION_REF) {
    Type::nameUnion(alias->resolved, alias->name);
  }
}

DeclarationTypesRegistry::ResultEnumIsResolved
DeclarationTypesRegistry::enumCycle(const EnumDecl *enumDecl) const {
  if (enumDecl->stage == EnumStage::RESOLVING) {
    auto message = ErrorTextEnumCycle::itself(enumDecl->name);

    return err(DeclarationTypesRegistryEnumCycleError{
        .code = DeclarationTypesRegistryEnumCycleErrorCode::DEPENDS_ON_ITSELF,
        .message = message,
    });
  }

  return ok(enumDecl);
}

DeclarationTypesRegistry::ResultMemberIsUnique
DeclarationTypesRegistry::uniqueMember(const EnumMember &member, const EnumMember &other) const {
  if (other.name == member.name) {
    auto message = ErrorTextUniqueMember::duplicate(member.name);

    return err(DeclarationTypesRegistryUniqueMemberError{
        .code = DeclarationTypesRegistryUniqueMemberErrorCode::DUPLICATE_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

DeclarationTypesRegistry::ResultMemberConstant
DeclarationTypesRegistry::memberConstant(std::optional<ConstantValue> constant) const {
  if (!constant.has_value()) {
    auto message = ErrorTextMemberConstant::notConst();

    return err(DeclarationTypesRegistryMemberConstantError{
        .code = DeclarationTypesRegistryMemberConstantErrorCode::NOT_CONSTANT,
        .message = message,
    });
  }

  return ok(*constant);
}

DeclarationTypesRegistry::ResultMemberHasValue
DeclarationTypesRegistry::countedMember(const EnumMember &member, bool isCounting) const {
  if (member.init == nullptr && !isCounting) {
    auto message = ErrorTextCountedMember::noValue(member.name);

    return err(DeclarationTypesRegistryCountedMemberError{
        .code = DeclarationTypesRegistryCountedMemberErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&member);
}

DeclarationTypesRegistry::ResultMemberIntValue
DeclarationTypesRegistry::wholeMember(const EnumMember &member, double value) const {
  if (value != std::trunc(value) || std::fabs(value) > kMaxSafeInteger) {
    auto shown = this->formatNumber(value);
    auto message = ErrorTextWholeMember::notWhole(member.name, shown);

    return err(DeclarationTypesRegistryWholeMemberError{
        .code = DeclarationTypesRegistryWholeMemberErrorCode::NOT_WHOLE,
        .message = message,
    });
  }

  return ok(value);
}

DeclarationTypesRegistry::ResultEnumKindIsSame
DeclarationTypesRegistry::enumKinds(const EnumDecl *enumDecl, const EnumCount &count) const {
  if (count.hasNumbers && count.hasStrings) {
    auto message = ErrorTextEnumKinds::mixed(enumDecl->name);

    return err(DeclarationTypesRegistryEnumKindsError{
        .code = DeclarationTypesRegistryEnumKindsErrorCode::MIXED,
        .message = message,
    });
  }

  return ok(enumDecl);
}

void DeclarationTypesRegistry::resolveEnum(EnumDecl *enumDecl) {
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

void DeclarationTypesRegistry::reportEnumCycle(const EnumDecl *enumDecl) {
  auto resolved = this->enumCycle(enumDecl);
  if (resolved.isErr()) {
    this->state.diagnostics.error(enumDecl->loc(), resolved.error().message);
  }
}

void DeclarationTypesRegistry::startEnum(std::vector<EnumFrame> &frames, EnumDecl *enumDecl) {
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

void DeclarationTypesRegistry::enterMember(const EnumFrame &frame) {
  const std::vector<EnumMember> &members = frame.enumDecl->members;
  if (frame.nextMember == members.size()) {
    return;
  }
  const EnumMember &member = members[frame.nextMember];
  auto earlier = llvm::ArrayRef<EnumMember>(members).take_front(frame.nextMember);
  for (const EnumMember &other : earlier) {
    auto unique = this->uniqueMember(member, other);
    if (unique.isErr()) {
      this->state.diagnostics.error(member.loc, unique.error().message);
    }
  }
}

EnumDecl *DeclarationTypesRegistry::unresolvedEnum(const EnumFrame &frame) const {
  if (frame.nextMember == frame.enumDecl->members.size()) {
    return nullptr;
  }
  const Expr *init = frame.enumDecl->members[frame.nextMember].init;
  auto needed = this->enumsNeeded(init, frame.enumDecl);
  for (EnumDecl *other : needed) {
    if (other->stage == EnumStage::UNRESOLVED) {
      return other;
    }
  }

  return nullptr;
}

void DeclarationTypesRegistry::resolveNextMember(std::vector<EnumFrame> &frames) {
  EnumFrame &frame = frames.back();
  if (frame.nextMember == frame.enumDecl->members.size()) {
    return this->finishEnum(frames);
  }
  this->reportNeededCycles(frame);
  this->countMember(frame);
  frame.nextMember += 1;

  return this->enterMember(frame);
}

void DeclarationTypesRegistry::reportNeededCycles(const EnumFrame &frame) {
  const Expr *init = frame.enumDecl->members[frame.nextMember].init;
  for (const EnumDecl *other : this->enumsNeeded(init, frame.enumDecl)) {
    this->reportEnumCycle(other);
  }
}

void DeclarationTypesRegistry::countMember(EnumFrame &frame) {
  EnumMember &member = frame.enumDecl->members[frame.nextMember];
  auto value = this->memberNumber(frame, member);
  if (value.has_value()) {
    this->numberMember(frame, member, *value);
  }
}

std::optional<double> DeclarationTypesRegistry::memberNumber(EnumFrame &frame, EnumMember &member) {
  if (member.init == nullptr) {
    return frame.count.next;
  }
  auto found = this->evalConstant(member.init, frame.enumDecl, frame.nextMember);
  auto constant = this->memberConstant(found);
  if (constant.isErr()) {
    this->state.diagnostics.error(member.init->loc(), constant.error().message);
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

void DeclarationTypesRegistry::numberMember(EnumFrame &frame, EnumMember &member, double value) {
  auto valued = this->countedMember(member, frame.count.isCounting);
  if (valued.isErr()) {
    this->state.diagnostics.error(member.loc, valued.error().message);
    return;
  }
  auto whole = this->wholeMember(member, value);
  if (whole.isErr()) {
    llvm::SMLoc at = member.init != nullptr ? member.init->loc() : member.loc;
    this->state.diagnostics.error(at, whole.error().message);
    frame.count.isCounting = false;
    return;
  }
  member.number = static_cast<int64_t>(value);
  frame.count.hasNumbers = true;
  frame.count.next = value + 1;
  frame.count.isCounting = true;
}

void DeclarationTypesRegistry::finishEnum(std::vector<EnumFrame> &frames) {
  EnumFrame frame = frames.back();
  frames.pop_back();
  EnumDecl *enumDecl = frame.enumDecl;
  auto same = this->enumKinds(enumDecl, frame.count);
  if (same.isErr()) {
    this->state.diagnostics.error(enumDecl->loc(), same.error().message);
  }
  enumDecl->isString = frame.count.hasStrings && !frame.count.hasNumbers;
  enumDecl->stage = EnumStage::RESOLVED;
  if (enumDecl->isString) {
    this->markTextsAssigned(enumDecl);
  }
  if (frame.previousModule.has_value()) {
    this->modules->useModule(*frame.previousModule);
  }
}

void DeclarationTypesRegistry::markTextsAssigned(const EnumDecl *enumDecl) {
  for (const EnumMember &member : enumDecl->members) {
    auto text = ToUtf8::toUtf8(member.text);
    this->state.flow.assignments.markFieldAssigned(text);
  }
}

const Type *DeclarationTypesRegistry::enumType(EnumDecl *enumDecl) {
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

int32_t DeclarationTypesRegistry::jsToInt32(double number) const {
  if (!std::isfinite(number)) {
    return 0;
  }
  double modulo = std::fmod(std::trunc(number), kTwoTo32);
  if (modulo < 0) {
    modulo += kTwoTo32;
  }

  return static_cast<int32_t>(static_cast<uint32_t>(modulo));
}

double DeclarationTypesRegistry::power(double base, double exponent) const {
  if (std::isnan(exponent) || (std::fabs(base) == 1 && std::isinf(exponent))) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  return std::pow(base, exponent);
}

std::optional<ConstantValue>
DeclarationTypesRegistry::evalConstant(const Expr *expr, const EnumDecl *self, size_t defined) {
  auto answers = std::vector<std::optional<ConstantValue>>();
  for (const Expr *node :
       ChildrenFirst::order(expr, this, &DeclarationTypesRegistry::constantParts)) {
    auto value = this->nodeConstant(node, self, defined, answers);
    answers.push_back(std::move(value));
  }

  return answers.back();
}

NodeChildren<const Expr *> DeclarationTypesRegistry::constantParts(const Expr *expr) const {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (unary != nullptr) {
    return {unary->operand};
  }
  if (binary != nullptr) {
    return {binary->lhs, binary->rhs};
  }

  return {};
}

std::vector<EnumDecl *> DeclarationTypesRegistry::enumsNeeded(const Expr *init,
                                                              const EnumDecl *self) const {
  auto needed = std::vector<EnumDecl *>();
  if (init == nullptr) {
    return needed;
  }
  for (const Expr *part : ExprParts::operatorTree(init)) {
    auto *other = this->enumNamedBy(part);
    if (other != nullptr && other != self) {
      needed.push_back(other);
    }
  }

  return needed;
}

EnumDecl *DeclarationTypesRegistry::enumNamedBy(const Expr *expr) const {
  auto *access = dyn_cast<MemberExpr>(expr);
  if (access == nullptr) {
    return nullptr;
  }
  auto *object = dyn_cast<NameExpr>(access->object);
  if (object == nullptr) {
    return nullptr;
  }

  return this->state.walk.scopes.findName(object->name).enumDecl();
}

ConstantValue DeclarationTypesRegistry::numberValue(double number) const {
  return ConstantValue{.isString = false, .number = number, .text = u""};
}

ConstantValue DeclarationTypesRegistry::textValue(std::u16string text) const {
  return ConstantValue{.isString = true, .number = 0, .text = std::move(text)};
}

ConstantValue DeclarationTypesRegistry::memberValue(const EnumMember &member) const {
  if (member.isString) {
    return this->textValue(member.text);
  }

  return this->numberValue(static_cast<double>(member.number));
}

std::optional<ConstantValue>
DeclarationTypesRegistry::nodeConstant(const Expr *expr, const EnumDecl *self, size_t defined,
                                       std::vector<std::optional<ConstantValue>> &answers) {
  auto *unary = dyn_cast<UnaryExpr>(expr);
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (unary != nullptr) {
    auto operand = ChildrenFirst::takeAnswer(answers);

    return this->unaryConstant(unary->op, operand);
  }
  if (binary != nullptr) {
    auto right = ChildrenFirst::takeAnswer(answers);
    auto left = ChildrenFirst::takeAnswer(answers);

    return this->binaryConstant(binary->op, left, right);
  }

  return this->leafConstant(expr, self, defined);
}

std::optional<ConstantValue> DeclarationTypesRegistry::leafConstant(const Expr *expr,
                                                                    const EnumDecl *self,
                                                                    size_t defined) const {
  auto *intLiteral = dyn_cast<IntLitExpr>(expr);
  auto *floatLiteral = dyn_cast<FloatLitExpr>(expr);
  auto *stringLiteral = dyn_cast<StringLitExpr>(expr);
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  if (intLiteral != nullptr) {
    return this->numberValue(static_cast<double>(intLiteral->value));
  }
  if (floatLiteral != nullptr) {
    return this->numberValue(floatLiteral->value);
  }
  if (stringLiteral != nullptr) {
    return this->textValue(stringLiteral->value);
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return this->textValue(templateExpr->quasis[0]);
  }
  if (name != nullptr) {
    return this->ownMemberConstant(self, defined, name->name);
  }
  if (member != nullptr) {
    return this->enumMemberConstant(member, self, defined);
  }

  return std::nullopt;
}

std::optional<ConstantValue>
DeclarationTypesRegistry::ownMemberConstant(const EnumDecl *self, size_t defined,
                                            llvm::StringRef name) const {
  auto earlier = llvm::ArrayRef<EnumMember>(self->members).take_front(defined);
  for (const EnumMember &member : earlier) {
    if (member.name == name) {
      return this->memberValue(member);
    }
  }

  return std::nullopt;
}

std::optional<ConstantValue> DeclarationTypesRegistry::enumMemberConstant(const MemberExpr *member,
                                                                          const EnumDecl *self,
                                                                          size_t defined) const {
  const EnumDecl *enumDecl = this->enumNamedBy(member);
  if (enumDecl == nullptr) {
    return std::nullopt;
  }
  if (enumDecl == self) {
    return this->ownMemberConstant(self, defined, member->name);
  }
  const EnumMember *found = enumDecl->find(member->name);
  if (found == nullptr) {
    return std::nullopt;
  }

  return this->memberValue(*found);
}

std::optional<ConstantValue>
DeclarationTypesRegistry::unaryConstant(UnaryOp op,
                                        const std::optional<ConstantValue> &operand) const {
  if (!operand.has_value() || operand->isString) {
    return std::nullopt;
  }
  double number = operand->number;
  switch (op) {
    case UnaryOp::Neg:
      return this->numberValue(-number);
    case UnaryOp::Plus:
      return operand;
    case UnaryOp::BitNot:
      return this->numberValue(~this->jsToInt32(number));
    case UnaryOp::Not:
    case UnaryOp::TypeOf:
    case UnaryOp::Void:
    case UnaryOp::Delete:
    case UnaryOp::NonNull:
      return std::nullopt;
  }

  return std::nullopt;
}

std::optional<ConstantValue>
DeclarationTypesRegistry::binaryConstant(BinaryOp op, const std::optional<ConstantValue> &left,
                                         const std::optional<ConstantValue> &right) const {
  if (!left.has_value() || !right.has_value()) {
    return std::nullopt;
  }
  if (left->isString || right->isString) {
    return this->joinedText(op, *left, *right);
  }

  return this->numberResult(op, left->number, right->number);
}

std::optional<ConstantValue>
DeclarationTypesRegistry::joinedText(BinaryOp op, const ConstantValue &left,
                                     const ConstantValue &right) const {
  if (op != BinaryOp::Add || !left.isString || !right.isString) {
    return std::nullopt;
  }

  return this->textValue(left.text + right.text);
}

std::optional<ConstantValue> DeclarationTypesRegistry::numberResult(BinaryOp op, double left,
                                                                    double right) const {
  switch (op) {
    case BinaryOp::Add:
      return this->numberValue(left + right);
    case BinaryOp::Sub:
      return this->numberValue(left - right);
    case BinaryOp::Mul:
      return this->numberValue(left * right);
    case BinaryOp::Div:
      return this->numberValue(left / right);
    case BinaryOp::Rem:
      return this->numberValue(std::fmod(left, right));
    case BinaryOp::Pow:
      return this->numberValue(this->power(left, right));
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return this->int32Result(op, this->jsToInt32(left), this->jsToInt32(right));
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
    case BinaryOp::Nullish:
    case BinaryOp::In:
    case BinaryOp::InstanceOf:
      return std::nullopt;
  }

  return std::nullopt;
}

ConstantValue DeclarationTypesRegistry::int32Result(BinaryOp op, int32_t left,
                                                    int32_t right) const {
  int shift = right & kShiftMask;
  if (op == BinaryOp::BitAnd) {
    return this->numberValue(left & right);
  }
  if (op == BinaryOp::BitOr) {
    return this->numberValue(left | right);
  }
  if (op == BinaryOp::BitXor) {
    return this->numberValue(left ^ right);
  }
  if (op == BinaryOp::Shl) {
    uint32_t moved = static_cast<uint32_t>(left) << shift;

    return this->numberValue(static_cast<int32_t>(moved));
  }
  if (op == BinaryOp::Shr) {
    return this->numberValue(left >> shift);
  }
  uint32_t moved = static_cast<uint32_t>(left) >> shift;

  return this->numberValue(moved);
}

std::vector<Stmt *> DeclarationTypesRegistry::declareModuleTypes(Module *module) {
  auto declared = this->declareTypeNames(module->file.items);
  for (Stmt *type : declared) {
    this->state.declaration.modules.rememberDeclModule(type, module);
  }

  return declared;
}

}
