#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

TypeParams::TypeParams(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->typeOperators = &all.typeOperators;
  this->returnInferrer = &all.returnInferrer;
  this->decoratorChecker = &all.decoratorChecker;
  this->aliasInstantiator = &all.aliasInstantiator;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool TypeParams::isMentioningInObject(const Type *type, llvm::ArrayRef<const Type *> params) const {
  auto parts = TypeParts::collect(type, kMentionsInObjectRoute);
  for (const Type *part : parts.getTypes()) {
    bool isOperation = part->isParam() && part->typeOp() != TypeOp::None;
    if ((isOperation || part->isObject()) && GenericCheckHelpers::mentionsAnyOf(part, params)) {
      return true;
    }
  }

  return false;
}

TypeParams::ResultParamNameIsNew TypeParams::uniqueName(const TypeParamDecl &earlier,
                                                        const TypeParamDecl &typeParam) const {
  if (earlier.name == typeParam.name) {
    auto message = ErrorTextUniqueParam::duplicate(typeParam.name);

    return err(TypeParamsUniqueNameError{
        .code = TypeParamsUniqueNameErrorCode::DUPLICATE_TYPE_PARAMETER,
        .message = message,
    });
  }

  return ok(&typeParam);
}

TypeParams::ResultNotBuiltInName TypeParams::ownName(const TypeParamDecl &typeParam) const {
  if (Type::fromName(typeParam.name) != nullptr) {
    auto message = ErrorTextOwnName::builtin(typeParam.name);

    return err(TypeParamsOwnNameError{
        .code = TypeParamsOwnNameErrorCode::BUILTIN_NAME,
        .message = message,
    });
  }

  return ok(&typeParam);
}

TypeParams::ResultConstraintKept TypeParams::constraintAlone(const Type *type,
                                                             const TypeParamDecl &typeParam) const {
  if (type->isUndefined()) {
    auto message = ErrorTextConstraint::undefined(typeParam.name);

    return err(TypeParamsConstraintAloneError{
        .code = TypeParamsConstraintAloneErrorCode::UNDEFINED_CONSTRAINT,
        .message = message,
    });
  }

  return ok(type);
}

TypeParams::ResultDefaultSatisfy TypeParams::defaultFits(const Type *fallback, const Type *param,
                                                         const TypeParamDecl &typeParam) {
  if (!fallback->isError() && !this->isSatisfying(fallback, param)) {
    auto constraint = GenericCheckHelpers::constraintSpelling(param, &typeParam);
    auto message = ErrorTextDefaultFits::unsatisfied(fallback->name(), typeParam.name, constraint);

    return err(TypeParamsDefaultFitsError{
        .code = TypeParamsDefaultFitsErrorCode::DEFAULT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(fallback);
}

TypeParams::ResultDefaultPresent TypeParams::defaultGiven(const TypeParamDecl &typeParam,
                                                          bool isAfterDefault) const {
  if (!typeParam.defaultType.has_value() && isAfterDefault) {
    auto message = ErrorTextDefaultGiven::missing(typeParam.name);

    return err(TypeParamsDefaultGivenError{
        .code = TypeParamsDefaultGivenErrorCode::MISSING_DEFAULT,
        .message = message,
    });
  }

  return ok(&typeParam);
}

std::vector<const Type *> TypeParams::declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                                        std::vector<const Type *> *defaults) {
  auto created = this->createTypeParams(decls);
  std::vector<const Type *> params(created.begin(), created.end());
  this->bindTypeParams(decls, params);
  bool isAfterDefault = false;
  for (size_t index = 0; index < decls.size(); ++index) {
    const TypeParamDecl &typeParam = decls[index];
    if (typeParam.constraint.has_value()) {
      this->constrainTypeParam(created[index], typeParam);
    }
    const auto *fallback = this->resolvedDefault(typeParam, params[index]);
    if (typeParam.defaultType.has_value()) {
      isAfterDefault = true;
    }
    auto present = this->defaultGiven(typeParam, isAfterDefault);
    if (present.isErr()) {
      this->state.getDiagnostics().error(typeParam.loc, present.error().message);
    }
    if (defaults != nullptr) {
      defaults->push_back(fallback);
    }
  }

  return params;
}

std::vector<Type *> TypeParams::createTypeParams(llvm::ArrayRef<TypeParamDecl> decls) {
  auto created = std::vector<Type *>();
  for (size_t index = 0; index < decls.size(); ++index) {
    this->walkParamName(decls, index);
    created.push_back(Type::param(decls[index].name));
  }

  return created;
}

void TypeParams::walkParamName(llvm::ArrayRef<TypeParamDecl> decls, size_t index) {
  const TypeParamDecl &typeParam = decls[index];
  for (const TypeParamDecl &earlier : decls.take_front(index)) {
    auto unique = this->uniqueName(earlier, typeParam);
    if (unique.isErr()) {
      this->state.getDiagnostics().error(typeParam.loc, unique.error().message);
    }
  }
  auto own = this->ownName(typeParam);
  if (own.isErr()) {
    this->state.getDiagnostics().error(typeParam.loc, own.error().message);
  }
}

void TypeParams::constrainTypeParam(Type *param, const TypeParamDecl &typeParam) {
  const TypeRef &ref = *typeParam.constraint;
  auto found = this->readConstraint(ref, param);
  if (!found.isResolved) {
    return;
  }
  auto alone = this->checkConstraintsAlone(found, typeParam);
  if (alone.isErr()) {
    this->state.getDiagnostics().error(ref.loc, alone.error().message);
    return;
  }
  std::vector<const Type *> alternatives =
      found.constraint != nullptr ? std::vector<const Type *>() : found.alternatives;
  param->setConstraint(found.constraint, alternatives);
}

TypeParams::ParamConstraint TypeParams::noConstraint() const {
  return ParamConstraint{.constraint = nullptr, .alternatives = {}, .isResolved = true};
}

bool TypeParams::isUnknownList(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::ARRAY_REF || ref.args[0].kind() != TypeRefKind::NAME_REF) {
    return false;
  }

  return TypeNameTable::isName(ref.args[0].name, TypeName::UNKNOWN);
}

TypeParams::ParamConstraint TypeParams::readConstraint(const TypeRef &ref, Type *param) {
  if (this->isUnknownList(ref)) {
    param->setList();

    return this->noConstraint();
  }
  if (ref.kind() == TypeRefKind::NAME_REF && TypeNameTable::isName(ref.name, TypeName::UNKNOWN)) {
    return this->noConstraint();
  }
  if (ref.kind() == TypeRefKind::UNION_REF) {
    return this->unionConstraint(ref);
  }
  const auto *constraint = this->typeResolver->resolveType(ref);

  return ParamConstraint{
      .constraint = constraint,
      .alternatives = {},
      .isResolved = !constraint->isError(),
  };
}

TypeParams::ParamConstraint TypeParams::unionConstraint(const TypeRef &ref) {
  auto found = this->noConstraint();
  for (const TypeRef &member : ref.args) {
    const auto *type = this->typeResolver->resolveType(member);
    if (type->isError()) {
      found.isResolved = false;
    }
    if (!llvm::is_contained(found.alternatives, type)) {
      found.alternatives.push_back(type);
    }
  }
  const std::vector<const Type *> &alternatives = found.alternatives;
  bool isOptionalPair = found.isResolved && alternatives.size() == 2 &&
                        (alternatives[0]->isUndefined() || alternatives[1]->isUndefined());
  if (isOptionalPair) {
    const Type *defined = alternatives[0]->isUndefined() ? alternatives[1] : alternatives[0];
    found.constraint = Type::optional(defined);
  }

  return found;
}

TypeParams::ResultConstraintsKept
TypeParams::checkConstraintsAlone(const ParamConstraint &found,
                                  const TypeParamDecl &typeParam) const {
  llvm::ArrayRef<const Type *> all = found.constraint != nullptr
                                         ? llvm::ArrayRef<const Type *>(found.constraint)
                                         : llvm::ArrayRef<const Type *>(found.alternatives);
  for (const Type *type : all) {
    auto kept = this->constraintAlone(type, typeParam);
    if (kept.isErr()) {
      return err(kept.error());
    }
  }

  return ok();
}

const Type *TypeParams::resolvedDefault(const TypeParamDecl &typeParam, const Type *param) {
  if (!typeParam.defaultType.has_value()) {
    return nullptr;
  }
  const auto *fallback = this->typeResolver->resolveType(*typeParam.defaultType);
  auto satisfied = this->defaultFits(fallback, param, typeParam);
  if (satisfied.isErr()) {
    this->state.getDiagnostics().error(typeParam.defaultType->loc, satisfied.error().message);

    return Type::error();
  }

  return fallback;
}

void TypeParams::bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                llvm::ArrayRef<const Type *> types) {
  size_t count = std::min(decls.size(), types.size());
  for (size_t index = 0; index < count; ++index) {
    const TypeParamDecl &typeParam = decls[index];
    auto *alias =
        this->state.getAst().make<TypeAliasDecl>(typeParam.loc, typeParam.name, TypeRef());
    alias->resolved = types[index];
    alias->boundParam = true;
    this->state.getScopeStack().setType(typeParam.name, alias);
  }
}

const Type *TypeParams::substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                                   llvm::ArrayRef<const Type *> args) {
  return this->substituteAt(type, params, args, VoidAs::VOID);
}

const Type *TypeParams::substituteValue(const Type *type, llvm::ArrayRef<const Type *> params,
                                        llvm::ArrayRef<const Type *> args) {
  return this->substituteAt(type, params, args, VoidAs::UNDEFINED);
}

const Type *TypeParams::substituteAt(const Type *type, llvm::ArrayRef<const Type *> params,
                                     llvm::ArrayRef<const Type *> args, VoidAs voidAs) {
  std::vector<SubstituteStep> steps = {SubstituteType{.type = type, .voidAs = voidAs}};
  auto answers = std::vector<const Type *>();
  while (!steps.empty()) {
    SubstituteStep step = steps.back();
    steps.pop_back();
    const SubstituteType *enter = step.as<SubstituteType>();
    if (enter != nullptr) {
      this->enterSubstitute(*enter, params, args, steps, answers);
    } else {
      const auto *built = this->buildSubstituted(*step.as<BuildSubstituted>(), answers);
      answers.push_back(built);
    }
  }

  return answers.back();
}

void TypeParams::enterSubstitute(const SubstituteType &enter, llvm::ArrayRef<const Type *> params,
                                 llvm::ArrayRef<const Type *> args,
                                 std::vector<SubstituteStep> &steps,
                                 std::vector<const Type *> &answers) {
  const Type *type = enter.type;
  if (type == nullptr || !GenericCheckHelpers::mentionsAnyOf(type, params)) {
    answers.push_back(type);
    return;
  }
  auto form = this->substituteForm(type);
  if (!form.has_value()) {
    const auto *plain = this->plainSubstitute(type, params, args, enter.voidAs);
    answers.push_back(plain);
    return;
  }
  if (*form == SubstituteForm::OBJECT) {
    this->state.getGenericStore().enterSubstitution(type);
  }
  auto parts = this->substituteParts(type, *form);
  steps.push_back(BuildSubstituted{.type = type, .form = *form, .count = parts.size()});
  for (const SubstituteType &part : llvm::reverse(parts)) {
    steps.push_back(part);
  }
}

const Type *TypeParams::plainSubstitute(const Type *type, llvm::ArrayRef<const Type *> params,
                                        llvm::ArrayRef<const Type *> args, VoidAs voidAs) const {
  if (!type->isParam()) {
    return type;
  }
  auto position = size_t(llvm::find(params, type) - params.begin());
  const Type *arg = args[position];
  if (arg == nullptr) {
    return type;
  }
  if (arg->isVoid() && voidAs == VoidAs::UNDEFINED) {
    return Type::undefinedTy();
  }

  return arg;
}

std::optional<SubstituteForm> TypeParams::substituteForm(const Type *type) const {
  switch (type->kind()) {
    case Type::Kind::Param:
      return this->paramForm(type);
    case Type::Kind::Optional:
      return SubstituteForm::OPTIONAL;
    case Type::Kind::Array:
      return SubstituteForm::ARRAY;
    case Type::Kind::Result:
      return SubstituteForm::RESULT;
    case Type::Kind::Function:
      return SubstituteForm::FUNCTION;
    case Type::Kind::Iterator:
      return SubstituteForm::ITERATOR;
    case Type::Kind::Dict:
      return SubstituteForm::DICT;
    case Type::Kind::Union:
      return SubstituteForm::UNION;
    case Type::Kind::Promise:
      return SubstituteForm::PROMISE;
    case Type::Kind::ClassRef:
      return this->classRefForm(type);
    case Type::Kind::Object:
      return this->objectForm(type);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Double:
    case Type::Kind::Decimal:
    case Type::Kind::String:
    case Type::Kind::Never:
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Unknown:
      return std::nullopt;
  }

  return std::nullopt;
}

std::optional<SubstituteForm> TypeParams::paramForm(const Type *type) const {
  if (type->typeOp() == TypeOp::None) {
    return std::nullopt;
  }

  return SubstituteForm::TYPE_OPERATOR;
}

std::optional<SubstituteForm> TypeParams::classRefForm(const Type *type) const {
  if (!type->isConstructor()) {
    return std::nullopt;
  }

  return SubstituteForm::CONSTRUCTOR;
}

std::optional<SubstituteForm> TypeParams::objectForm(const Type *type) const {
  if (type->isAsyncIterator()) {
    return SubstituteForm::ASYNC_ITERATOR;
  }
  if (this->state.getAliasStore().findDescriptorOrigin(type) != nullptr) {
    return SubstituteForm::DESCRIPTOR;
  }
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl != nullptr && !type->typeArgs().empty()) {
    return SubstituteForm::CLASS_INSTANCE;
  }
  if (this->state.getAliasStore().findAliasOrigin(type) != nullptr) {
    return SubstituteForm::ALIAS_INSTANCE;
  }
  if (type->isTuple()) {
    return SubstituteForm::TUPLE;
  }
  if (this->state.getGenericStore().isSubstituting(type)) {
    return std::nullopt;
  }

  return SubstituteForm::OBJECT;
}

std::vector<SubstituteType> TypeParams::substituteParts(const Type *type,
                                                        SubstituteForm form) const {
  switch (form) {
    case SubstituteForm::TYPE_OPERATOR:
      return this->partsOf(type->operands(), VoidAs::VOID);
    case SubstituteForm::OPTIONAL:
    case SubstituteForm::ARRAY:
      return {this->valuePart(type->inner())};
    case SubstituteForm::RESULT:
      return {this->voidPart(type->okType()), this->voidPart(type->errType())};
    case SubstituteForm::FUNCTION:
      return this->functionParts(type);
    case SubstituteForm::ITERATOR:
    case SubstituteForm::ASYNC_ITERATOR:
      return {this->valuePart(type->yielded()), this->voidPart(type->returned()),
              this->voidPart(type->sent())};
    case SubstituteForm::DICT:
      return {this->valuePart(type->keyType()), this->valuePart(type->valueType())};
    case SubstituteForm::UNION:
      return this->partsOf(type->members(), VoidAs::UNDEFINED);
    case SubstituteForm::PROMISE:
      return {this->voidPart(type->promised())};
    case SubstituteForm::CONSTRUCTOR:
      return {this->valuePart(type->constructed())};
    case SubstituteForm::DESCRIPTOR:
      return this->descriptorParts(type);
    case SubstituteForm::CLASS_INSTANCE:
      return this->partsOf(type->typeArgs(), VoidAs::VOID);
    case SubstituteForm::ALIAS_INSTANCE:
      return this->aliasParts(type);
    case SubstituteForm::TUPLE:
      return this->tupleParts(type);
    case SubstituteForm::OBJECT:
      return this->fieldParts(type);
  }

  return {};
}

SubstituteType TypeParams::valuePart(const Type *type) const {
  return SubstituteType{.type = type, .voidAs = VoidAs::UNDEFINED};
}

SubstituteType TypeParams::voidPart(const Type *type) const {
  return SubstituteType{.type = type, .voidAs = VoidAs::VOID};
}

std::vector<SubstituteType> TypeParams::functionParts(const Type *type) const {
  auto parts = this->partsOf(type->params(), VoidAs::UNDEFINED);
  parts.push_back(this->voidPart(type->ret()));

  return parts;
}

std::vector<SubstituteType> TypeParams::descriptorParts(const Type *type) const {
  const auto *origin = this->state.getAliasStore().findDescriptorOrigin(type);

  return {this->valuePart(origin->args), this->voidPart(origin->ret)};
}

std::vector<SubstituteType> TypeParams::aliasParts(const Type *type) const {
  const auto *origin = this->state.getAliasStore().findAliasOrigin(type);

  return this->partsOf(origin->args, VoidAs::VOID);
}

std::vector<SubstituteType> TypeParams::tupleParts(const Type *type) const {
  auto parts = this->fieldParts(type);
  if (!this->hasListParam(type) && type->tupleRest() != nullptr) {
    parts.push_back(this->valuePart(type->tupleRest()));
  }

  return parts;
}

std::vector<SubstituteType> TypeParams::partsOf(llvm::ArrayRef<const Type *> types,
                                                VoidAs voidAs) const {
  auto parts = std::vector<SubstituteType>();
  for (const Type *part : types) {
    parts.push_back(SubstituteType{.type = part, .voidAs = voidAs});
  }

  return parts;
}

std::vector<SubstituteType> TypeParams::fieldParts(const Type *type) const {
  auto parts = std::vector<SubstituteType>();
  for (const Field &field : type->fields()) {
    parts.push_back(this->valuePart(field.type));
  }

  return parts;
}

bool TypeParams::hasListParam(const Type *tuple) const {
  for (const Field &field : tuple->fields()) {
    if (field.type->isListParam()) {
      return true;
    }
  }

  return false;
}

const Type *TypeParams::buildSubstituted(const BuildSubstituted &build,
                                         std::vector<const Type *> &answers) {
  const Type *type = build.type;
  std::vector<const Type *> parts = ChildrenFirst::takeAnswers(answers, build.count);
  switch (build.form) {
    case SubstituteForm::TYPE_OPERATOR:
      return this->typeOperators->evalTypeOp(type, parts);
    case SubstituteForm::OPTIONAL:
      return Type::optional(parts[0]);
    case SubstituteForm::ARRAY:
      return Type::array(parts[0]);
    case SubstituteForm::RESULT:
      return Type::result(parts[0], parts[1]);
    case SubstituteForm::FUNCTION:
      return this->substitutedFunction(type, std::move(parts));
    case SubstituteForm::ITERATOR:
      return Type::iterator(parts[0], parts[1], parts[2]);
    case SubstituteForm::DICT:
      return Type::dict(type->dictKind(), parts[0], parts[1]);
    case SubstituteForm::UNION:
      return Type::unionOf(parts);
    case SubstituteForm::PROMISE:
      return Type::promise(parts[0]);
    case SubstituteForm::CONSTRUCTOR:
      return Type::constructorOf(parts[0]);
    case SubstituteForm::ASYNC_ITERATOR:
      return Type::asyncIterator(parts[0], parts[1], parts[2]);
    case SubstituteForm::DESCRIPTOR:
      return this->decoratorChecker->methodDescriptorType(parts[0], parts[1]);
    case SubstituteForm::CLASS_INSTANCE:
      return this->substitutedInstance(type, std::move(parts));
    case SubstituteForm::ALIAS_INSTANCE:
      return this->substitutedAlias(type, std::move(parts));
    case SubstituteForm::TUPLE:
      return this->substitutedTuple(type, std::move(parts));
    case SubstituteForm::OBJECT:
      return this->substitutedObject(type, parts);
  }

  return Type::error();
}

const Type *TypeParams::substitutedFunction(const Type *type, std::vector<const Type *> parts) {
  const Type *returnType = parts.back();
  parts.pop_back();

  return Type::function(parts, type->minArgs(), type->hasRest(), returnType, type->hasThis());
}

const Type *TypeParams::substitutedAlias(const Type *type, std::vector<const Type *> parts) {
  TypeAliasDecl *alias = this->state.getAliasStore().findAliasOrigin(type)->alias;

  return this->aliasInstantiator->instantiateAlias(alias, std::move(parts), alias->loc());
}

const Type *TypeParams::substitutedInstance(const Type *type, std::vector<const Type *> newArgs) {
  const ClassDecl *classDecl = type->classDecl();
  const ClassDecl *owner = classDecl->generic() != nullptr ? classDecl->generic() : classDecl;
  auto *generic = const_cast<ClassDecl *>(owner);
  llvm::SMLoc use = this->state.getGenericStore().getInstanceLoc();
  auto made = this->genericClassInstantiator->instantiateClass(generic, std::move(newArgs), use);
  if (made.isOk()) {
    return made.value()->type;
  }
  auto code = made.error().code;
  if (code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    bool isEndless = code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES;
    llvm::SMLoc at = isEndless || !use.isValid() ? generic->loc() : use;
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return Type::error();
}

const Type *TypeParams::substitutedTuple(const Type *type, std::vector<const Type *> parts) const {
  bool hasRest = !this->hasListParam(type) && type->tupleRest() != nullptr;
  const Type *rest = hasRest ? parts.back() : nullptr;
  if (hasRest) {
    parts.pop_back();
  }
  auto spread = std::vector<bool>();
  for (const Field &field : type->fields()) {
    spread.push_back(field.type->isListParam());
  }
  if (llvm::is_contained(spread, true)) {
    auto why = std::string();
    const auto *list = ArgumentLists::spreadTuple(parts, spread, why);

    return list != nullptr ? list : Type::error();
  }
  if (type->tupleMin() < parts.size() || type->tupleRest() != nullptr) {
    return Type::argumentList(parts, type->tupleMin(), rest);
  }
  bool isReadonly = !type->fields().empty() && type->fields().front().readonly;

  return Type::tuple(parts, isReadonly);
}

const Type *TypeParams::substitutedObject(const Type *type, llvm::ArrayRef<const Type *> parts) {
  this->state.getGenericStore().leaveSubstitution(type);
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < parts.size(); ++index) {
    const Field &field = type->fields()[index];
    auto substituted = Field{.name = field.name, .type = parts[index], .readonly = field.readonly};
    fields.push_back(substituted);
  }

  return Type::object(std::move(fields));
}

const Type *TypeParams::representation(const Type *type) const {
  auto answers = std::vector<const Type *>();
  for (const Type *node : ChildrenFirst::order(type, this, &TypeParams::representationParts)) {
    const auto *shown = this->nodeRepresentation(node, answers);
    answers.push_back(shown);
  }

  return answers.back();
}

NodeChildren<const Type *> TypeParams::representationParts(const Type *type) const {
  if (type->kind() == Type::Kind::Array || type->kind() == Type::Kind::Optional) {
    return {type->inner()};
  }
  if (type->kind() == Type::Kind::Result) {
    return {type->okType(), type->errType()};
  }
  if (type->kind() == Type::Kind::Union) {
    return NodeChildren<const Type *>(type->members().begin(), type->members().end());
  }

  return {};
}

const Type *TypeParams::nodeRepresentation(const Type *type,
                                           std::vector<const Type *> &answers) const {
  switch (type->kind()) {
    case Type::Kind::Object:
    case Type::Kind::Iterator:
    case Type::Kind::Dict:
      return Type::anyObject();
    case Type::Kind::Function:
      return Type::function({}, 0, false, Type::voidTy());
    case Type::Kind::Array:
      return Type::array(ChildrenFirst::takeAnswer(answers));
    case Type::Kind::Optional:
      return Type::optional(ChildrenFirst::takeAnswer(answers));
    case Type::Kind::Result:
      return this->resultRepresentation(answers);
    case Type::Kind::Union:
      return this->unionRepresentation(type, answers);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Double:
    case Type::Kind::Decimal:
    case Type::Kind::String:
    case Type::Kind::Param:
    case Type::Kind::Never:
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Promise:
    case Type::Kind::Unknown:
    case Type::Kind::ClassRef:
      return type;
  }

  return type;
}

const Type *TypeParams::resultRepresentation(std::vector<const Type *> &answers) const {
  const Type *errType = ChildrenFirst::takeAnswer(answers);
  const Type *okType = ChildrenFirst::takeAnswer(answers);

  return Type::result(okType, errType);
}

const Type *TypeParams::unionRepresentation(const Type *type,
                                            std::vector<const Type *> &answers) const {
  auto members = ChildrenFirst::takeAnswers(answers, type->members().size());

  return Type::unionOf(members);
}

bool TypeParams::isFittingConstraint(const Type *arg, const Type *constraint) {
  if (TypeRelations::isImplicitlyConvertible(arg, constraint)) {
    return true;
  }

  return constraint->isObject() && this->hasMembersOf(arg, constraint);
}

bool TypeParams::isSatisfying(const Type *arg, const Type *param,
                              llvm::ArrayRef<const Type *> params,
                              llvm::ArrayRef<const Type *> args) {
  if (arg->isError()) {
    return true;
  }
  const Type *written = param->constraint();
  bool isDependent =
      written != nullptr && !params.empty() && GenericCheckHelpers::mentionsAnyOf(written, params);
  const Type *own = isDependent ? this->substitute(written, params, args) : written;
  if (own != nullptr && own->isError()) {
    return true;
  }
  if (arg->isParam() && arg->constraint() != nullptr && own != nullptr &&
      TypeRelations::isImplicitlyConvertible(arg->constraint(), own)) {
    return true;
  }
  if (own != nullptr) {
    return this->isFittingConstraint(arg, own);
  }
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();
  if (alternatives.empty()) {
    return true;
  }
  for (const Type *alternative : alternatives) {
    if (this->isFittingConstraint(arg, alternative)) {
      return true;
    }
  }

  return false;
}

bool TypeParams::hasMembersOf(const Type *arg, const Type *constraint) {
  auto parts = TypeParts::collect(arg, kMembersOfRoute);
  for (const Type *part : parts.getTypes()) {
    bool isConstrained = part->isParam() && part->constraint() != nullptr;
    bool isLeaf = !isConstrained && !part->isUnion();
    if (isLeaf && !this->hasObjectMembersOf(part, constraint)) {
      return false;
    }
  }

  return true;
}

bool TypeParams::hasObjectMembersOf(const Type *arg, const Type *constraint) {
  bool isComparable = arg->isObject() && arg->isTuple() == constraint->isTuple() &&
                      !constraint->isClass() && !arg->isAnyObject();
  if (!isComparable) {
    return false;
  }
  ClassDecl *classDecl = this->classDeclarer->classOf(arg);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }
  for (const Field &field : constraint->fields()) {
    if (!this->hasMatchingField(arg, classDecl, field)) {
      return false;
    }
  }
  for (const Overload &overload : constraint->overloads()) {
    if (!this->hasMatchingOverload(classDecl, overload)) {
      return false;
    }
  }

  return true;
}

bool TypeParams::isTextUnion(const Type *type) const {
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

bool TypeParams::isMatchingField(const Field &have, const Field &wanted) const {
  bool isText = have.type->isStringLiteral() || this->isTextUnion(have.type);
  bool isLiteral = wanted.type->isString() && isText;
  if (have.type != wanted.type && !isLiteral) {
    return false;
  }

  return !have.readonly || wanted.readonly;
}

bool TypeParams::hasMatchingField(const Type *arg, const ClassDecl *classDecl, const Field &field) {
  const auto *have = arg->field(field.name);
  if (have != nullptr) {
    return this->isMatchingField(*have, field);
  }
  if (this->hasMethodField(classDecl, field)) {
    return true;
  }

  return field.type->isOptional();
}

bool TypeParams::hasMethodField(const ClassDecl *classDecl, const Field &field) {
  if (classDecl == nullptr || !field.readonly || !field.type->isFunction()) {
    return false;
  }
  bool isMethod = false;
  for (FunctionDecl *method : this->classDeclarer->methodsNamed(classDecl, field.name)) {
    bool isPlain = method->sym != nullptr && method->typeParams.empty();
    if (isPlain && this->returnInferrer->functionType(method->sym) == field.type) {
      isMethod = true;
    }
  }

  return isMethod;
}

bool TypeParams::hasMatchingOverload(const ClassDecl *classDecl, const Overload &overload) {
  if (classDecl == nullptr) {
    return false;
  }
  auto methods = this->classDeclarer->methodsNamed(classDecl, overload.name);
  for (const Type *signature : overload.signatures) {
    if (!this->hasMethodOfType(methods, signature)) {
      return false;
    }
  }

  return true;
}

bool TypeParams::hasMethodOfType(llvm::ArrayRef<FunctionDecl *> methods, const Type *signature) {
  for (FunctionDecl *method : methods) {
    if (method->sym != nullptr && this->returnInferrer->functionType(method->sym) == signature) {
      return true;
    }
  }

  return false;
}

bool TypeParams::isObjectLike(const Type *type) const {
  return type != nullptr && type->nonOptional()->isObject();
}

bool TypeParams::hasObjectLikeAlternative(const Type *param) const {
  for (const Type *alternative : param->alternatives()) {
    if (this->isObjectLike(alternative)) {
      return true;
    }
  }

  return false;
}

bool TypeParams::isExactParam(const FuncSymbol *generic, size_t index) const {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  const Type *param = params[index];
  if (this->isObjectLike(param->constraint()) || this->hasObjectLikeAlternative(param)) {
    return true;
  }
  if (param->iterated() != nullptr || param->isListParam()) {
    return true;
  }
  for (const Type *other : params) {
    const Type *iterated = other->iterated();
    if (iterated != nullptr && GenericCheckHelpers::mentionsAnyOf(iterated, param)) {
      return true;
    }
  }
  for (const Type *other : params) {
    const Type *constraint = other->constraint();
    bool isOther = other != param && constraint != nullptr;
    if (isOther && GenericCheckHelpers::mentionsAnyOf(constraint, param)) {
      return true;
    }
  }
  for (const Type *type : generic->params) {
    if (type != nullptr && this->isMentioningInObject(type, param)) {
      return true;
    }
  }

  return generic->hasReturnType() &&
         this->isMentioningInObject(generic->getKnownReturnType(), param);
}

std::vector<bool> TypeParams::exactParams(const FuncSymbol *generic) const {
  auto exact = std::vector<bool>();
  for (size_t index = 0; index < generic->typeParams.size(); ++index) {
    exact.push_back(this->isExactParam(generic, index));
  }

  return exact;
}

bool TypeParams::isExactTypeParam(FuncSymbol *generic, size_t index) {
  auto found = this->state.getGenericStore().findExactParams(generic);
  if (!found.has_value()) {
    auto exact = this->exactParams(generic);
    this->state.getGenericStore().rememberExactParams(generic, exact);
    found = std::move(exact);
  }

  return index < found->size() && (*found)[index];
}

}
