#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/types/template_types/template_types.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr const char *kHiddenFieldPrefix = "%";

}

TypeOperators::TypeOperators(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.eventMaps;
  this->typeParams = &all.typeParams;
  this->classBases = &all.classBases;
  this->typeResolver = &all.typeResolver;
  this->utilityTypes = &all.utilityTypes;
  this->classDeclarer = &all.classDeclarer;
  this->templateTypes = &all.templateTypes;
  this->argListChecker = &all.argListChecker;
  this->returnInferrer = &all.returnInferrer;
  this->genericChecker = &all.genericChecker;
  this->extendsRelation = &all.extendsRelation;
  this->topLevelChecker = &all.topLevelChecker;
  this->conditionalTypes = &all.conditionalTypes;
  this->classValueChecker = &all.classValueChecker;
}

llvm::ArrayRef<const Type *> TypeOperators::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

const Type *TypeOperators::keysOf(const Type *type) {
  const Type *plain = type->nonOptional();
  if (!plain->isUnion()) {
    return this->ownKeys(plain);
  }
  auto common = std::vector<const Type *>();
  bool isFirst = true;
  for (const Type *member : plain->members()) {
    const Type *keys = this->ownKeys(member->nonOptional());
    if (keys == nullptr) {
      return nullptr;
    }
    common = this->sharedKeys(common, keys, isFirst);
    isFirst = false;
  }
  if (common.empty()) {
    return Type::never();
  }

  return Type::unionOf(common);
}

std::vector<const Type *> TypeOperators::sharedKeys(llvm::ArrayRef<const Type *> common,
                                                    const Type *keys, bool isFirst) const {
  auto shared = std::vector<const Type *>();
  for (const Type *key : this->membersOf(keys)) {
    if (isFirst || llvm::is_contained(common, key)) {
      shared.push_back(key);
    }
  }

  return shared;
}

const Type *TypeOperators::ownKeys(const Type *type) {
  if (type->isRecord()) {
    return type->keyType();
  }
  if (!type->isObject() || type->isTuple() || type->isAnyObject()) {
    return nullptr;
  }
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }
  auto keys = std::vector<const Type *>();
  for (const Field &field : type->fields()) {
    if (this->isPublicKey(classDecl, field.name)) {
      auto text = ToUtf16::toUtf16(field.name);
      keys.push_back(Type::stringLiteral(text));
    }
  }
  if (keys.empty()) {
    return Type::never();
  }

  return Type::unionOf(keys);
}

bool TypeOperators::isPublicKey(const ClassDecl *classDecl, llvm::StringRef name) const {
  if (name.starts_with(kHiddenFieldPrefix)) {
    return false;
  }
  if (classDecl == nullptr) {
    return true;
  }
  const ClassField *declared = this->classBases->findField(classDecl, name);

  return declared == nullptr || declared->access == Access::Public;
}

const Type *TypeOperators::evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands) {
  TypeOp kind = op->typeOp();
  if (kind == TypeOp::Conditional) {
    return this->conditionalTypes->evalConditional(operands);
  }
  if (kind == TypeOp::Mapped) {
    return this->evalMapped(op, operands);
  }
  if (this->isMentioningAnyParam(operands)) {
    return Type::typeOp(kind, operands);
  }
  const auto *failed = this->firstError(operands);
  if (failed != nullptr) {
    return failed;
  }

  return this->evalReadyTypeOp(kind, operands);
}

bool TypeOperators::isMentioningAnyParam(llvm::ArrayRef<const Type *> operands) const {
  for (const Type *type : operands) {
    if (type != nullptr && TypeQueries::mentionsParam(type)) {
      return true;
    }
  }

  return false;
}

const Type *TypeOperators::firstError(llvm::ArrayRef<const Type *> operands) const {
  for (const Type *type : operands) {
    if (type != nullptr && type->isError()) {
      return type;
    }
  }

  return nullptr;
}

const Type *TypeOperators::evalReadyTypeOp(TypeOp kind, llvm::ArrayRef<const Type *> operands) {
  switch (kind) {
    case TypeOp::KeyOf:
      return this->evalKeyOf(operands[0]);
    case TypeOp::Index:
      return this->evalIndexed(operands[0], operands[1]);
    case TypeOp::Exclude:
    case TypeOp::Extract:
      return this->evalFiltered(kind, operands[0], operands[1]);
    case TypeOp::Template:
      return this->templateTypes->evalTemplate(operands);
    case TypeOp::Spread:
      return this->evalSpread(operands);
    case TypeOp::Uppercase:
    case TypeOp::Lowercase:
    case TypeOp::Capitalize:
    case TypeOp::Uncapitalize:
      return this->templateTypes->evalStringCase(kind, operands[0]);
    case TypeOp::NonNullable:
      return operands[0]->nonOptional();
    case TypeOp::ReturnType:
    case TypeOp::Parameters:
      return this->evalFunctionPart(kind, operands[0]);
    case TypeOp::None:
    case TypeOp::Mapped:
    case TypeOp::Conditional:
      return Type::error();
  }

  return Type::error();
}

const Type *TypeOperators::evalKeyOf(const Type *operand) {
  const Type *keys = this->keysOf(operand);
  if (keys == nullptr) {
    return Type::error();
  }

  return keys;
}

const Type *TypeOperators::evalIndexed(const Type *object, const Type *key) {
  bool isList = object->isTuple() || object->isArray();
  if (isList && this->isLengthKey(key)) {
    return this->lengthType(object);
  }
  const auto *member = this->argListChecker->memberType(object, key);
  if (member == nullptr) {
    return Type::error();
  }

  return member;
}

bool TypeOperators::isLengthKey(const Type *key) const {
  if (!key->isStringLiteral()) {
    return false;
  }
  auto text = ToUtf8::toUtf8(key->literalText());

  return BuiltinPropertyTable::isName(text, BuiltinProperty::LENGTH);
}

const Type *TypeOperators::lengthType(const Type *object) const {
  bool isFixed = object->isTuple() && object->tupleRest() == nullptr &&
                 object->tupleMin() == object->fields().size();
  if (isFixed) {
    return Type::numberLiteral(double(object->fields().size()), Type::intTy());
  }

  return Type::intTy();
}

const Type *TypeOperators::evalFiltered(TypeOp kind, const Type *members, const Type *filter) {
  bool isKeepingMatches = kind == TypeOp::Extract;
  auto kept = std::vector<const Type *>();
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(members)) {
    bool isMatching = this->extendsRelation->isExtendingType(member, filter);
    if (isMatching == isKeepingMatches) {
      kept.push_back(member);
    }
  }
  if (kept.empty()) {
    return Type::never();
  }

  return Type::unionOf(kept);
}

const Type *TypeOperators::evalSpread(llvm::ArrayRef<const Type *> operands) const {
  auto elements = std::vector<const Type *>();
  auto spread = std::vector<bool>();
  for (size_t index = 0; index + 1 < operands.size(); index += 2) {
    spread.push_back(operands[index]->literalNumber() != 0);
    elements.push_back(operands[index + 1]);
  }
  auto why = std::string();
  const auto *tuple = ArgumentLists::spreadTuple(elements, spread, why);
  if (tuple == nullptr) {
    return Type::error();
  }

  return tuple;
}

const Type *TypeOperators::evalFunctionPart(TypeOp kind, const Type *operand) const {
  const Type *functionType = operand->nonOptional();
  if (!functionType->isFunction()) {
    return Type::error();
  }
  if (kind == TypeOp::ReturnType) {
    return functionType->ret();
  }
  llvm::ArrayRef<const Type *> all = functionType->params();
  llvm::ArrayRef<const Type *> fixed = functionType->hasRest() ? all.drop_back() : all;
  const Type *rest = functionType->hasRest() ? all.back() : nullptr;
  size_t required = std::min(functionType->minArgs(), fixed.size());

  return Type::argumentList(fixed, required, rest);
}

const Type *TypeOperators::evalMapped(const Type *op, llvm::ArrayRef<const Type *> operands) {
  auto mapped = MappedOperands{
      .key = operands[0],
      .keys = operands[1],
      .value = operands[2],
      .source = operands[3],
      .rename = operands.size() > 4 ? operands[4] : nullptr,
      .optional = op->mappedOptional(),
      .readonly = op->mappedReadonly(),
  };
  if (this->isOpenMapped(mapped)) {
    return Type::mapped(mapped.key, mapped.keys, mapped.value, mapped.optional, mapped.readonly,
                        mapped.source, mapped.rename);
  }
  const Type *keys = mapped.keys;
  if (keys->isNever()) {
    return Type::object({});
  }
  auto fields = std::vector<Field>();
  for (const Type *name : this->membersOf(keys)) {
    const auto *failed = this->addMappedField(mapped, name, fields);
    if (failed != nullptr) {
      return failed;
    }
  }

  return Type::object(std::move(fields));
}

bool TypeOperators::isOpenMapped(const MappedOperands &mapped) const {
  return TypeQueries::mentionsParam(mapped.keys) ||
         (mapped.source != nullptr && TypeQueries::mentionsParam(mapped.source)) ||
         this->conditionalTypes->isMentioningParamBesides(mapped.value, {mapped.key}) ||
         (mapped.rename != nullptr &&
          this->conditionalTypes->isMentioningParamBesides(mapped.rename, {mapped.key}));
}

const Type *TypeOperators::addMappedField(const MappedOperands &mapped, const Type *name,
                                          std::vector<Field> &fields) {
  if (!name->isStringLiteral()) {
    return Type::error();
  }
  auto text = ToUtf8::toUtf8(name->literalText());
  const auto *value = this->typeParams->substitute(mapped.value, {mapped.key}, {name});
  if (value->isError()) {
    return value;
  }
  const Type *type = this->mappedFieldType(mapped, value);
  bool isReadonly = this->isMappedReadonly(mapped, text);
  auto names = std::vector<std::string>{text};
  const auto *failed = this->renamedFields(mapped, name, names);
  if (failed != nullptr) {
    return failed;
  }
  for (const std::string &field : names) {
    this->mergeMappedField(fields, field, type, isReadonly);
  }

  return nullptr;
}

const Type *TypeOperators::mappedFieldType(const MappedOperands &mapped, const Type *value) const {
  if (mapped.optional > 0) {
    return Type::optional(value);
  }
  if (mapped.optional < 0) {
    return value->nonOptional();
  }

  return value;
}

bool TypeOperators::isMappedReadonly(const MappedOperands &mapped, llvm::StringRef name) const {
  if (mapped.readonly != 0) {
    return mapped.readonly > 0;
  }
  if (mapped.source == nullptr) {
    return false;
  }
  const Field *from = mapped.source->field(name);

  return from != nullptr && from->readonly;
}

const Type *TypeOperators::renamedFields(const MappedOperands &mapped, const Type *name,
                                         std::vector<std::string> &names) {
  if (mapped.rename == nullptr) {
    return nullptr;
  }
  const auto *renamed = this->typeParams->substitute(mapped.rename, {mapped.key}, {name});
  if (renamed->isError()) {
    return renamed;
  }
  names.clear();
  for (const Type *renamedTo : TypeopsCheckHelpers::distributedMembers(renamed)) {
    if (!renamedTo->isStringLiteral()) {
      return Type::error();
    }
    names.push_back(ToUtf8::toUtf8(renamedTo->literalText()));
  }

  return nullptr;
}

void TypeOperators::mergeMappedField(std::vector<Field> &fields, const std::string &name,
                                     const Type *type, bool isReadonly) const {
  for (Field &field : fields) {
    if (field.name == name) {
      field.type = Type::unionOf({field.type, type});
      return;
    }
  }
  fields.push_back({name, type, isReadonly});
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveTypeOp(const TypeRef &ref) {
  switch (ref.kind()) {
    case TypeRefKind::KEY_OF_REF:
      return this->resolveKeyOf(ref);
    case TypeRefKind::INDEXED_REF:
      return this->resolveIndexed(ref);
    case TypeRefKind::TYPE_OF_REF:
      return this->resolveTypeOf(ref);
    case TypeRefKind::MAPPED_REF:
      return this->resolveMapped(ref);
    case TypeRefKind::NAME_REF:
    case TypeRefKind::ARRAY_REF:
    case TypeRefKind::UNION_REF:
    case TypeRefKind::GENERIC_REF:
    case TypeRefKind::FUNCTION_REF:
    case TypeRefKind::OBJECT_REF:
    case TypeRefKind::TUPLE_REF:
    case TypeRefKind::LITERAL_REF:
    case TypeRefKind::PREDICATE_REF:
    case TypeRefKind::CONDITIONAL_REF:
    case TypeRefKind::INFER_REF:
    case TypeRefKind::TEMPLATE_REF:
    case TypeRefKind::INTERSECTION_REF:
      return ok(nullptr);
  }

  return ok(nullptr);
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveKeyOf(const TypeRef &ref) {
  const auto *type = this->typeResolver->resolveType(ref.args[0]);
  if (type->isError()) {
    return ok(type);
  }
  const auto *unevaluated = Type::typeOp(TypeOp::KeyOf, {type});
  const auto *keys = this->evalTypeOp(unevaluated, {type});
  if (keys->isError()) {
    auto message = ErrorTextTypeOp::keyOf(type->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::NOT_AN_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(keys);
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveIndexed(const TypeRef &ref) {
  const auto *object = this->typeResolver->resolveType(ref.args[0]);
  const auto *key = this->typeResolver->resolveType(ref.args[1]);
  if (object->isError() || key->isError()) {
    return ok(Type::error());
  }
  const auto *unevaluated = Type::indexedAccess(object, key);
  const auto *member = this->evalTypeOp(unevaluated, {object, key});
  if (member->isError()) {
    auto message = ErrorTextTypeOp::noMember(object->name(), key->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_MEMBER,
        .message = message,
    });
  }

  return ok(member);
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveTypeOf(const TypeRef &ref) {
  llvm::StringRef name = ref.names().front();
  const Type *type = this->typeOfName(name);
  if (type == nullptr) {
    auto message = ErrorTextTypeOp::typeOf(name);

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::TYPE_OF_UNKNOWN,
        .message = message,
    });
  }
  auto parts = llvm::ArrayRef<llvm::StringRef>(ref.names()).drop_front();
  for (llvm::StringRef part : parts) {
    const Type *member = this->memberValueType(type, part);
    if (member == nullptr) {
      auto message = ErrorTextTypeOp::noField(type->name(), part);

      return err(TypeOperatorsResolveTypeOpError{
          .code = TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_FIELD,
          .message = message,
      });
    }
    type = member;
  }

  return ok(type);
}

const Type *TypeOperators::memberValueType(const Type *type, llvm::StringRef name) {
  const Field *field = type->isObject() ? type->field(name) : nullptr;
  if (field != nullptr) {
    return field->type;
  }

  return this->singleMethodType(type, name);
}

const Type *TypeOperators::typeOfName(llvm::StringRef name) {
  const auto *scoped = this->scopedValueType(name);
  if (scoped != nullptr) {
    return scoped;
  }
  auto *module = this->state.getModuleStates().getCurrentModule();
  if (module == nullptr) {
    return nullptr;
  }
  for (Stmt *item : module->file.items) {
    const auto *declared = this->declaredValueType(item, name);
    if (declared != nullptr) {
      return declared;
    }
  }

  return this->importedNameType(module->file.imports, name);
}

const Type *TypeOperators::scopedValueType(llvm::StringRef name) {
  auto entry = this->topLevelChecker->lookupValue(name);
  VarSymbol *variable = entry.var();
  ClassDecl *classDecl = entry.cls();
  FuncSymbol *function = entry.func();

  if (variable != nullptr) {
    return variable->type;
  }
  if (classDecl != nullptr && !classDecl->isGeneric()) {
    return this->classValueChecker->classValueType(classDecl);
  }
  if (function != nullptr) {
    return this->utilityTypes->functionValueType(function);
  }

  return nullptr;
}

const Type *TypeOperators::declaredValueType(Stmt *item, llvm::StringRef name) {
  auto *varDecl = dyn_cast<VarDeclStmt>(item);
  auto *functionDecl = dyn_cast<FunctionDecl>(item);

  if (varDecl != nullptr) {
    return this->declaredVarType(varDecl, name);
  }
  if (functionDecl != nullptr) {
    return this->declaredFunctionType(functionDecl, name);
  }

  return nullptr;
}

const Type *TypeOperators::declaredVarType(const VarDeclStmt *varDecl, llvm::StringRef name) {
  for (const VarDeclarator &declarator : varDecl->decls) {
    const Type *written = this->writtenVarType(declarator, name);
    if (written != nullptr) {
      return written;
    }
  }
  for (const VarDeclarator &declarator : llvm::reverse(varDecl->decls)) {
    auto *create = llvm::dyn_cast_or_null<NewExpr>(declarator.init);
    if (declarator.name == name && create != nullptr) {
      return this->eventMaps->createdType(create);
    }
  }

  return nullptr;
}

const Type *TypeOperators::writtenVarType(const VarDeclarator &declarator, llvm::StringRef name) {
  if (declarator.name != name) {
    return nullptr;
  }
  if (declarator.typeAnn.has_value()) {
    return this->typeResolver->resolveType(*declarator.typeAnn);
  }
  if (declarator.init != nullptr) {
    return this->genericChecker->literalShape(declarator.init);
  }

  return nullptr;
}

const Type *TypeOperators::declaredFunctionType(const FunctionDecl *functionDecl,
                                                llvm::StringRef name) {
  if (functionDecl->name != name || !functionDecl->typeParams.empty() ||
      !functionDecl->returnType.has_value()) {
    return nullptr;
  }
  auto params = std::vector<const Type *>();
  size_t minArgs = 0;
  for (const Param &param : functionDecl->params) {
    if (!param.type.has_value()) {
      return nullptr;
    }
    const auto *type = this->typeResolver->resolveType(*param.type);
    bool isOptional = param.optional() || param.defaultValue() != nullptr;
    params.push_back(isOptional ? Type::optional(type) : type);
    if (!isOptional && !param.rest()) {
      minArgs = params.size();
    }
  }
  bool hasRest = !functionDecl->params.empty() && functionDecl->params.back().rest();
  const auto *returned = this->typeResolver->resolveTypeOrVoid(*functionDecl->returnType);

  return Type::function(params, minArgs, hasRest, returned);
}

const Type *TypeOperators::importedNameType(const std::vector<ImportDecl> &imports,
                                            llvm::StringRef name) {
  for (const ImportDecl &importDecl : imports) {
    const Type *type = this->importedFrom(importDecl, name);
    if (type != nullptr) {
      return type;
    }
  }

  return nullptr;
}

const Type *TypeOperators::importedFrom(const ImportDecl &importDecl, llvm::StringRef name) {
  if (importDecl.from == nullptr) {
    return nullptr;
  }
  for (const ModuleName &imported : importDecl.names) {
    if (imported.as == name) {
      const Type *type = this->utilityTypes->importedValueType(importDecl.from, imported.name);
      if (type != nullptr) {
        return type;
      }
    }
  }

  return nullptr;
}

const Type *TypeOperators::singleMethodType(const Type *type, llvm::StringRef name) {
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  bool isSingle =
      methods.size() == 1 && methods[0]->sym != nullptr && methods[0]->typeParams.empty();
  if (!isSingle) {
    return nullptr;
  }

  return this->returnInferrer->functionType(methods[0]->sym);
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveMapped(const TypeRef &ref) {
  const auto *keys = this->typeResolver->resolveType(ref.args[0]);
  if (keys->isError()) {
    return ok(keys);
  }
  const auto *source = this->mappedSource(ref, keys);
  auto parts = this->resolveMappedParts(ref, keys);
  if (parts.value->isError()) {
    return ok(parts.value);
  }
  if (parts.rename != nullptr && parts.rename->isError()) {
    return ok(parts.rename);
  }
  auto rename = this->checkRenameIsString(parts.rename);
  if (rename.isErr()) {
    return err(rename.error());
  }
  auto stringKeys = this->checkKeysAreStrings(keys);
  if (stringKeys.isErr()) {
    return err(stringKeys.error());
  }
  const auto *mapped = Type::mapped(parts.key, keys, parts.value, ref.mappedOptional(),
                                    ref.mappedReadonly(), source, parts.rename);
  const auto *evaluated = this->evalTypeOp(mapped, mapped->operands());

  return ok(evaluated);
}

TypeOperators::ResultTypeOpAllowed TypeOperators::checkRenameIsString(const Type *rename) const {
  const Type *badName = this->nonStringRename(rename);
  if (badName != nullptr) {
    auto message = ErrorTextTypeOp::rename(badName->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::RENAME_NOT_STRING,
        .message = message,
    });
  }

  return ok();
}

TypeOperators::ResultTypeOpAllowed TypeOperators::checkKeysAreStrings(const Type *keys) const {
  const Type *badKey = this->nonStringKey(keys);
  if (badKey != nullptr) {
    auto message = ErrorTextTypeOp::keys(badKey->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }

  return ok();
}

const Type *TypeOperators::mappedSource(const TypeRef &ref, const Type *keys) {
  if (ref.args[0].kind() == TypeRefKind::KEY_OF_REF) {
    const auto *source = this->typeResolver->resolveType(ref.args[0].args[0]);
    if (source != nullptr) {
      return source;
    }
  }
  const Type *wider = keys;
  while (wider != nullptr) {
    if (wider->typeOp() == TypeOp::KeyOf) {
      return wider->operands()[0];
    }
    wider = this->widerKeys(wider);
  }

  return nullptr;
}

const Type *TypeOperators::widerKeys(const Type *keys) const {
  if (keys->typeOp() == TypeOp::Exclude || keys->typeOp() == TypeOp::Extract) {
    return keys->operands()[0];
  }
  if (keys->isParam()) {
    return keys->constraint();
  }

  return nullptr;
}

TypeOperators::MappedParts TypeOperators::resolveMappedParts(const TypeRef &ref, const Type *keys) {
  auto *key = Type::param(ref.name);
  key->setConstraint(keys, {});
  this->state.getScopeStack().openTypeScope();
  auto *alias = this->state.getAst().make<TypeAliasDecl>(ref.loc, ref.name, TypeRef());
  alias->resolved = key;
  this->state.getScopeStack().setType(ref.name, alias);
  const auto *value = this->typeResolver->resolveType(ref.args[1]);
  const Type *rename = this->writtenRename(ref);
  this->state.getScopeStack().closeTypeScope();

  return MappedParts{
      .key = key,
      .value = value,
      .rename = rename,
  };
}

const Type *TypeOperators::writtenRename(const TypeRef &ref) {
  if (ref.args.size() <= 2) {
    return nullptr;
  }

  return this->conditionalTypes->resolveTypeArgument(ref.args[2]);
}

const Type *TypeOperators::nonStringRename(const Type *rename) const {
  if (rename == nullptr || TypeQueries::mentionsParam(rename)) {
    return nullptr;
  }
  for (const Type *renamedTo : TypeopsCheckHelpers::distributedMembers(rename)) {
    if (!renamedTo->isStringLiteral()) {
      return renamedTo;
    }
  }

  return nullptr;
}

const Type *TypeOperators::nonStringKey(const Type *keys) const {
  if (TypeQueries::mentionsParam(keys)) {
    return nullptr;
  }
  const Type *each = keys;
  for (const Type *key : this->membersOf(each)) {
    if (!key->isStringLiteral() && !key->isNever()) {
      return key;
    }
  }

  return nullptr;
}

}
