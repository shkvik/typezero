#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
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

TypeOperators::TypeOperators(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->eventMaps = &all.eventMaps;
  this->typeParams = &all.typeParams;
  this->typeResolver = &all.typeResolver;
  this->utilityTypes = &all.utilityTypes;
  this->classDeclarer = &all.classDeclarer;
  this->templateTypes = &all.templateTypes;
  this->argListChecker = &all.argListChecker;
  this->returnInferrer = &all.returnInferrer;
  this->extendsRelation = &all.extendsRelation;
  this->topLevelChecker = &all.topLevelChecker;
  this->conditionalTypes = &all.conditionalTypes;
  this->classValueChecker = &all.classValueChecker;
}

std::u16string TypeOperators::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

llvm::ArrayRef<const Type *> TypeOperators::membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

const Type *TypeOperators::keysOf(const Type *type) {
  const Type *plain = type->nonOptional();
  if (!plain->isUnion()) {
    return this->ownKeys(plain);
  }
  std::vector<const Type *> common;
  bool first = true;
  for (const Type *m : plain->members()) {
    const Type *keys = this->ownKeys(m->nonOptional());
    if (!keys) {
      return nullptr;
    }
    std::vector<const Type *> mine;
    for (const Type *k : TypeOperators::membersOf(keys)) {
      if (first || llvm::is_contained(common, k)) {
        mine.push_back(k);
      }
    }
    common = std::move(mine);
    first = false;
  }

  return common.empty() ? Type::never() : Type::unionOf(common);
}

const Type *TypeOperators::ownKeys(const Type *type) {
  if (type->isRecord()) {
    return type->keyType();
  }
  if (!type->isObject() || type->isTuple() || type->isAnyObject()) {
    return nullptr;
  }
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls) {
    this->classDeclarer->completeClass(cls);
  }
  std::vector<const Type *> keys;
  for (const Field &f : type->fields()) {
    if (llvm::StringRef(f.name).starts_with("%")) {
      continue;
    }
    if (const ClassField *field = cls ? ClassBases::findField(cls, f.name) : nullptr;
        field && field->access != Access::Public) {
      continue;
    }
    keys.push_back(Type::stringLiteral(TypeOperators::utf16(f.name)));
  }

  return keys.empty() ? Type::never() : Type::unionOf(keys);
}

const Type *TypeOperators::evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands) {
  TypeOp kind = op->typeOp();
  if (kind == TypeOp::Conditional) {
    return this->conditionalTypes->evalConditional(operands);
  }
  if (kind == TypeOp::Mapped) {
    return this->evalMapped(op, operands);
  }
  if (TypeOperators::mentionsAnyParam(operands)) {
    return Type::typeOp(kind, operands);
  }
  const Type *failed = TypeOperators::firstError(operands);
  if (failed != nullptr) {
    return failed;
  }

  return this->evalReadyTypeOp(kind, operands);
}

bool TypeOperators::mentionsAnyParam(llvm::ArrayRef<const Type *> operands) {
  for (const Type *t : operands) {
    if (t != nullptr && TypeQueries::mentionsParam(t)) {
      return true;
    }
  }

  return false;
}

const Type *TypeOperators::firstError(llvm::ArrayRef<const Type *> operands) {
  for (const Type *t : operands) {
    if (t != nullptr && t->isError()) {
      return t;
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
      return TypeOperators::evalSpread(operands);
    case TypeOp::Uppercase:
    case TypeOp::Lowercase:
    case TypeOp::Capitalize:
    case TypeOp::Uncapitalize:
      return this->templateTypes->evalStringCase(kind, operands[0]);
    case TypeOp::NonNullable:
      return operands[0]->nonOptional();
    case TypeOp::ReturnType:
    case TypeOp::Parameters:
      return TypeOperators::evalFunctionPart(kind, operands[0]);
    case TypeOp::None:
    case TypeOp::Mapped:
    case TypeOp::Conditional:
      return Type::error();
  }

  return Type::error();
}

const Type *TypeOperators::evalKeyOf(const Type *operand) {
  const Type *keys = this->keysOf(operand);

  return keys != nullptr ? keys : Type::error();
}

const Type *TypeOperators::evalIndexed(const Type *object, const Type *key) {
  bool lengthKey =
      key->isStringLiteral() &&
      BuiltinPropertyTable::isName(ToUtf8::toUtf8(key->literalText()), BuiltinProperty::LENGTH);
  if (lengthKey && (object->isTuple() || object->isArray())) {
    return TypeOperators::lengthType(object);
  }
  const Type *member = this->argListChecker->memberType(object, key);

  return member != nullptr ? member : Type::error();
}

const Type *TypeOperators::lengthType(const Type *object) {
  bool fixed =
      object->isTuple() && !object->tupleRest() && object->tupleMin() == object->fields().size();
  if (fixed) {
    return Type::numberLiteral(double(object->fields().size()), Type::intTy());
  }

  return Type::intTy();
}

const Type *TypeOperators::evalFiltered(TypeOp kind, const Type *members, const Type *filter) {
  std::vector<const Type *> kept;
  for (const Type *m : TypeopsCheckHelpers::distributedMembers(members)) {
    if (this->extendsRelation->extendsType(m, filter) == (kind == TypeOp::Extract)) {
      kept.push_back(m);
    }
  }

  return kept.empty() ? Type::never() : Type::unionOf(kept);
}

const Type *TypeOperators::evalSpread(llvm::ArrayRef<const Type *> operands) {
  std::vector<const Type *> elements;
  std::vector<bool> spread;
  for (size_t i = 0; i + 1 < operands.size(); i += 2) {
    spread.push_back(operands[i]->literalNumber() != 0);
    elements.push_back(operands[i + 1]);
  }
  std::string why;
  const Type *tuple = ArgumentLists::spreadTuple(elements, spread, why);

  return tuple != nullptr ? tuple : Type::error();
}

const Type *TypeOperators::evalFunctionPart(TypeOp kind, const Type *operand) {
  const Type *fn = operand->nonOptional();
  if (!fn->isFunction()) {
    return Type::error();
  }
  if (kind == TypeOp::ReturnType) {
    return fn->ret();
  }
  std::vector<const Type *> params(fn->params().begin(),
                                   fn->params().end() - (fn->hasRest() ? 1 : 0));
  const Type *rest = fn->hasRest() ? fn->params().back() : nullptr;

  return Type::argumentList(params, std::min(fn->minArgs(), params.size()), rest);
}

const Type *TypeOperators::evalMapped(const Type *op, llvm::ArrayRef<const Type *> operands) {
  MappedOperands mapped{
      .key = operands[0],
      .keys = operands[1],
      .value = operands[2],
      .source = operands[3],
      .rename = operands.size() > 4 ? operands[4] : nullptr,
      .optional = op->mappedOptional(),
      .readonly = op->mappedReadonly(),
  };
  if (TypeOperators::isOpenMapped(mapped)) {
    return Type::mapped(mapped.key, mapped.keys, mapped.value, mapped.optional, mapped.readonly,
                        mapped.source, mapped.rename);
  }
  const Type *keys = mapped.keys;
  if (keys->isNever()) {
    return Type::object({});
  }
  std::vector<Field> fields;
  for (const Type *name : TypeOperators::membersOf(keys)) {
    const Type *failed = this->addMappedField(mapped, name, fields);
    if (failed != nullptr) {
      return failed;
    }
  }

  return Type::object(std::move(fields));
}

bool TypeOperators::isOpenMapped(const MappedOperands &mapped) {
  return TypeQueries::mentionsParam(mapped.keys) ||
         (mapped.source != nullptr && TypeQueries::mentionsParam(mapped.source)) ||
         ConditionalTypes::mentionsParamBesides(mapped.value, {mapped.key}) ||
         (mapped.rename != nullptr &&
          ConditionalTypes::mentionsParamBesides(mapped.rename, {mapped.key}));
}

const Type *TypeOperators::addMappedField(const MappedOperands &mapped, const Type *name,
                                          std::vector<Field> &fields) {
  if (!name->isStringLiteral()) {
    return Type::error();
  }
  std::string text = ToUtf8::toUtf8(name->literalText());
  const Type *type = this->typeParams->substitute(mapped.value, {mapped.key}, {name});
  if (type->isError()) {
    return type;
  }
  const Field *from = mapped.source != nullptr ? mapped.source->field(text) : nullptr;
  if (mapped.optional > 0) {
    type = Type::optional(type);
  } else if (mapped.optional < 0) {
    type = type->nonOptional();
  }
  bool readonly =
      mapped.readonly > 0 || (mapped.readonly == 0 && from != nullptr && from->readonly);
  std::vector<std::string> names{text};
  const Type *failed = this->renamedFields(mapped, name, names);
  if (failed != nullptr) {
    return failed;
  }
  for (const std::string &field : names) {
    TypeOperators::mergeMappedField(fields, field, type, readonly);
  }

  return nullptr;
}

const Type *TypeOperators::renamedFields(const MappedOperands &mapped, const Type *name,
                                         std::vector<std::string> &names) {
  if (mapped.rename == nullptr) {
    return nullptr;
  }
  const Type *renamed = this->typeParams->substitute(mapped.rename, {mapped.key}, {name});
  if (renamed->isError()) {
    return renamed;
  }
  names.clear();
  for (const Type *n : TypeopsCheckHelpers::distributedMembers(renamed)) {
    if (!n->isStringLiteral()) {
      return Type::error();
    }
    names.push_back(ToUtf8::toUtf8(n->literalText()));
  }

  return nullptr;
}

void TypeOperators::mergeMappedField(std::vector<Field> &fields, const std::string &field,
                                     const Type *type, bool readonly) {
  for (Field &f : fields) {
    if (f.name == field) {
      f.type = Type::unionOf({f.type, type});
      return;
    }
  }
  fields.push_back({field, type, readonly});
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
  const Type *type = this->typeResolver->resolveType(ref.args[0], false);
  if (type->isError()) {
    return ok(type);
  }
  const Type *keys = this->evalTypeOp(Type::typeOp(TypeOp::KeyOf, {type}), {type});
  if (keys->isError()) {
    std::string message = ErrorTextTypeOp::keyOf(type->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::NOT_AN_OBJECT_TYPE,
        .message = message,
    });
  }

  return ok(keys);
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveIndexed(const TypeRef &ref) {
  const Type *object = this->typeResolver->resolveType(ref.args[0], false);
  const Type *key = this->typeResolver->resolveType(ref.args[1], false);
  if (object->isError() || key->isError()) {
    return ok(Type::error());
  }
  const Type *member = this->evalTypeOp(Type::indexedAccess(object, key), {object, key});
  if (member->isError()) {
    std::string message = ErrorTextTypeOp::noMember(object->name(), key->name());

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
    std::string message = ErrorTextTypeOp::typeOf(name);

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::TYPE_OF_UNKNOWN,
        .message = message,
    });
  }
  for (size_t i = 1; i < ref.names().size(); ++i) {
    llvm::StringRef part = ref.names()[i];
    const Field *field = type->isObject() ? type->field(part) : nullptr;
    const Type *method = field != nullptr ? nullptr : this->singleMethodType(type, part);
    if (method != nullptr) {
      type = method;
      continue;
    }
    if (field == nullptr) {
      std::string message = ErrorTextTypeOp::noField(type->name(), part);

      return err(TypeOperatorsResolveTypeOpError{
          .code = TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_FIELD,
          .message = message,
      });
    }
    type = field->type;
  }

  return ok(type);
}

const Type *TypeOperators::typeOfName(llvm::StringRef name) {
  const Type *scoped = this->scopedValueType(name);
  if (scoped != nullptr) {
    return scoped;
  }
  Module *module = this->state.getModuleStates().getCurrentModule();
  if (module == nullptr) {
    return nullptr;
  }
  for (Stmt *item : module->file.items) {
    const Type *declared = this->declaredValueType(item, name);
    if (declared != nullptr) {
      return declared;
    }
  }

  return this->importedNameType(module->file.imports, name);
}

const Type *TypeOperators::scopedValueType(llvm::StringRef name) {
  ScopeEntry entry = this->topLevelChecker->lookupValue(name);
  VarSymbol *var = entry.var();
  if (var != nullptr) {
    return var->type;
  }
  ClassDecl *cls = entry.cls();
  if (cls != nullptr && !cls->isGeneric()) {
    return this->classValueChecker->classValueType(cls);
  }
  if (entry.func() != nullptr) {
    return this->utilityTypes->functionValueType(entry.func());
  }

  return nullptr;
}

const Type *TypeOperators::declaredValueType(Stmt *item, llvm::StringRef name) {
  auto *decl = dyn_cast<VarDeclStmt>(item);
  if (decl != nullptr) {
    return this->declaredVarType(decl, name);
  }
  auto *fn = dyn_cast<FunctionDecl>(item);
  if (fn != nullptr) {
    return this->declaredFunctionType(fn, name);
  }

  return nullptr;
}

const Type *TypeOperators::declaredVarType(const VarDeclStmt *decl, llvm::StringRef name) {
  const Type *type = nullptr;
  for (const VarDeclarator &d : decl->decls) {
    if (d.name != name || type != nullptr) {
      continue;
    }
    if (d.typeAnn.has_value()) {
      type = this->typeResolver->resolveType(*d.typeAnn, false);
    } else if (d.init != nullptr) {
      type = GenericChecker::literalShape(d.init);
    }
  }
  if (type != nullptr) {
    return type;
  }
  for (const VarDeclarator &d : decl->decls) {
    auto *create = d.name == name ? llvm::dyn_cast_or_null<NewExpr>(d.init) : nullptr;
    if (create != nullptr) {
      type = this->eventMaps->createdType(create);
    }
  }

  return type;
}

const Type *TypeOperators::declaredFunctionType(const FunctionDecl *fn, llvm::StringRef name) {
  if (fn->name != name || !fn->typeParams.empty() || !fn->returnType.has_value()) {
    return nullptr;
  }
  std::vector<const Type *> params;
  size_t minArgs = 0;
  for (const Param &p : fn->params) {
    if (!p.type.has_value()) {
      return nullptr;
    }
    const Type *t = this->typeResolver->resolveType(*p.type, false);
    params.push_back(p.optional() || p.defaultValue() ? Type::optional(t) : t);
    if (!p.optional() && !p.defaultValue() && !p.rest()) {
      minArgs = params.size();
    }
  }
  bool rest = !fn->params.empty() && fn->params.back().rest();
  const Type *returned = this->typeResolver->resolveType(*fn->returnType, true);

  return Type::function(params, minArgs, rest, returned);
}

const Type *TypeOperators::importedNameType(const std::vector<ImportDecl> &imports,
                                            llvm::StringRef name) {
  for (const ImportDecl &decl : imports) {
    for (const ModuleName &imported : decl.names) {
      if (imported.as != name || decl.from == nullptr) {
        continue;
      }
      const Type *type = this->utilityTypes->importedValueType(decl.from, imported.name);
      if (type != nullptr) {
        return type;
      }
    }
  }

  return nullptr;
}

const Type *TypeOperators::singleMethodType(const Type *type, llvm::StringRef name) {
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(cls);
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  bool single = methods.size() == 1 && methods[0]->sym != nullptr && methods[0]->typeParams.empty();
  if (!single) {
    return nullptr;
  }

  return this->returnInferrer->functionType(methods[0]->sym);
}

TypeOperators::ResultTypeOpResolved TypeOperators::resolveMapped(const TypeRef &ref) {
  const Type *keys = this->typeResolver->resolveType(ref.args[0], false);
  if (keys->isError()) {
    return ok(keys);
  }
  const Type *source = this->mappedSource(ref, keys);
  MappedParts parts = this->resolveMappedParts(ref, keys);
  if (parts.value->isError()) {
    return ok(parts.value);
  }
  if (parts.rename != nullptr && parts.rename->isError()) {
    return ok(parts.rename);
  }
  const Type *badName = TypeOperators::nonStringRename(parts.rename);
  if (badName != nullptr) {
    std::string message = ErrorTextTypeOp::rename(badName->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::RENAME_NOT_STRING,
        .message = message,
    });
  }
  const Type *badKey = TypeOperators::nonStringKey(keys);
  if (badKey != nullptr) {
    std::string message = ErrorTextTypeOp::keys(badKey->name());

    return err(TypeOperatorsResolveTypeOpError{
        .code = TypeOperatorsResolveTypeOpErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }
  const Type *mapped = Type::mapped(parts.key, keys, parts.value, ref.mappedOptional(),
                                    ref.mappedReadonly(), source, parts.rename);
  const Type *evaluated = this->evalTypeOp(mapped, mapped->operands());

  return ok(evaluated);
}

const Type *TypeOperators::mappedSource(const TypeRef &ref, const Type *keys) {
  if (ref.args[0].kind() == TypeRefKind::KEY_OF_REF) {
    const Type *source = this->typeResolver->resolveType(ref.args[0].args[0], false);
    if (source != nullptr) {
      return source;
    }
  }
  const Type *k = keys;
  while (k != nullptr) {
    if (k->typeOp() == TypeOp::KeyOf) {
      return k->operands()[0];
    }
    if (k->typeOp() == TypeOp::Exclude || k->typeOp() == TypeOp::Extract) {
      k = k->operands()[0];
    } else if (k->isParam() && k->constraint()) {
      k = k->constraint();
    } else {
      k = nullptr;
    }
  }

  return nullptr;
}

TypeOperators::MappedParts TypeOperators::resolveMappedParts(const TypeRef &ref, const Type *keys) {
  Type *key = Type::param(ref.name);
  key->setConstraint(keys, {});
  this->state.getScopeStack().openTypeScope();
  auto *alias = this->state.getAst().make<TypeAliasDecl>(ref.loc, ref.name, TypeRef());
  alias->resolved = key;
  this->state.getScopeStack().setType(ref.name, alias);
  const Type *value = this->typeResolver->resolveType(ref.args[1], false);
  const Type *rename =
      ref.args.size() > 2 ? this->conditionalTypes->resolveTypeArgument(ref.args[2]) : nullptr;
  this->state.getScopeStack().closeTypeScope();

  return MappedParts{
      .key = key,
      .value = value,
      .rename = rename,
  };
}

const Type *TypeOperators::nonStringRename(const Type *rename) {
  if (rename == nullptr || TypeQueries::mentionsParam(rename)) {
    return nullptr;
  }
  for (const Type *n : TypeopsCheckHelpers::distributedMembers(rename)) {
    if (!n->isStringLiteral()) {
      return n;
    }
  }

  return nullptr;
}

const Type *TypeOperators::nonStringKey(const Type *keys) {
  if (TypeQueries::mentionsParam(keys)) {
    return nullptr;
  }
  const Type *each = keys;
  for (const Type *k : TypeOperators::membersOf(each)) {
    if (!k->isStringLiteral() && !k->isNever()) {
      return k;
    }
  }

  return nullptr;
}
}
