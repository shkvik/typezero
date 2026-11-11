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

bool TypeParams::mentionsInObject(const Type *type, llvm::ArrayRef<const Type *> params) {
  TypeParts parts = TypeParts::collect(type, kMentionsInObjectRoute);
  for (const Type *part : parts.getTypes()) {
    bool isOperation = part->isParam() && part->typeOp() != TypeOp::None;
    if ((isOperation || part->isObject()) && GenericCheckHelpers::mentionsAnyOf(part, params)) {
      return true;
    }
  }

  return false;
}

TypeParams::ResultParamNameIsNew TypeParams::uniqueName(const TypeParamDecl &earlier,
                                                        const TypeParamDecl &decl) {
  if (earlier.name == decl.name) {
    std::string message = ErrorTextUniqueParam::duplicate(decl.name);

    return err(TypeParamsUniqueNameError{
        .code = TypeParamsUniqueNameErrorCode::DUPLICATE_TYPE_PARAMETER,
        .message = message,
    });
  }

  return ok(&decl);
}

TypeParams::ResultNotBuiltInName TypeParams::ownName(const TypeParamDecl &decl) {
  if (Type::fromName(decl.name)) {
    std::string message = ErrorTextOwnName::builtin(decl.name);

    return err(TypeParamsOwnNameError{
        .code = TypeParamsOwnNameErrorCode::BUILTIN_NAME,
        .message = message,
    });
  }

  return ok(&decl);
}

TypeParams::ResultConstraintKept TypeParams::constraintAlone(const Type *type,
                                                             const TypeParamDecl &decl) {
  if (type->isUndefined()) {
    std::string message = ErrorTextConstraint::undefined(decl.name);

    return err(TypeParamsConstraintAloneError{
        .code = TypeParamsConstraintAloneErrorCode::UNDEFINED_CONSTRAINT,
        .message = message,
    });
  }

  return ok(type);
}

TypeParams::ResultDefaultSatisfy TypeParams::defaultFits(const Type *fallback, const Type *param,
                                                         const TypeParamDecl &decl) {
  if (!fallback->isError() && !this->satisfies(fallback, param)) {
    std::string constraint = GenericCheckHelpers::constraintSpelling(param, &decl);
    std::string message =
        ErrorTextDefaultFits::unsatisfied(fallback->name(), decl.name, constraint);

    return err(TypeParamsDefaultFitsError{
        .code = TypeParamsDefaultFitsErrorCode::DEFAULT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(fallback);
}

TypeParams::ResultDefaultPresent TypeParams::defaultGiven(const TypeParamDecl &decl,
                                                          bool afterDefault) {
  if (!decl.defaultType && afterDefault) {
    std::string message = ErrorTextDefaultGiven::missing(decl.name);

    return err(TypeParamsDefaultGivenError{
        .code = TypeParamsDefaultGivenErrorCode::MISSING_DEFAULT,
        .message = message,
    });
  }

  return ok(&decl);
}

std::vector<const Type *> TypeParams::declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                                        std::vector<const Type *> *defaults) {
  std::vector<Type *> created = this->createTypeParams(decls);
  std::vector<const Type *> params(created.begin(), created.end());
  this->bindTypeParams(decls, params);
  bool isAfterDefault = false;
  for (size_t i = 0; i < decls.size(); ++i) {
    const TypeParamDecl &decl = decls[i];
    if (decl.constraint.has_value()) {
      this->constrainTypeParam(created[i], decl);
    }
    const Type *fallback = this->resolvedDefault(decl, params[i]);
    isAfterDefault |= decl.defaultType.has_value();
    ResultDefaultPresent present = TypeParams::defaultGiven(decl, isAfterDefault);
    if (present.isErr()) {
      this->state.getDiagnostics().error(decl.loc, present.error().message);
    }
    if (defaults != nullptr) {
      defaults->push_back(fallback);
    }
  }

  return params;
}

std::vector<Type *> TypeParams::createTypeParams(llvm::ArrayRef<TypeParamDecl> decls) {
  std::vector<Type *> created;
  for (size_t i = 0; i < decls.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      ResultParamNameIsNew unique = TypeParams::uniqueName(decls[j], decls[i]);
      if (unique.isErr()) {
        this->state.getDiagnostics().error(decls[i].loc, unique.error().message);
      }
    }
    ResultNotBuiltInName own = TypeParams::ownName(decls[i]);
    if (own.isErr()) {
      this->state.getDiagnostics().error(decls[i].loc, own.error().message);
    }
    created.push_back(Type::param(decls[i].name));
  }

  return created;
}

void TypeParams::constrainTypeParam(Type *param, const TypeParamDecl &decl) {
  const TypeRef &ref = *decl.constraint;
  ParamConstraint found = this->readConstraint(ref, param);
  if (!found.resolved || !this->constraintsAlone(found, decl, ref)) {
    return;
  }
  std::vector<const Type *> alternatives =
      found.constraint != nullptr ? std::vector<const Type *>() : found.alternatives;
  param->setConstraint(found.constraint, alternatives);
}

TypeParams::ParamConstraint TypeParams::readConstraint(const TypeRef &ref, Type *param) {
  ParamConstraint found;
  bool isUnknownList = ref.kind() == TypeRefKind::ARRAY_REF &&
                       ref.args[0].kind() == TypeRefKind::NAME_REF &&
                       TypeNameTable::isName(ref.args[0].name, TypeName::UNKNOWN);
  if (isUnknownList) {
    param->setList();
    return found;
  }
  if (ref.kind() == TypeRefKind::NAME_REF && TypeNameTable::isName(ref.name, TypeName::UNKNOWN)) {
    return found;
  }
  if (ref.kind() == TypeRefKind::UNION_REF) {
    return this->unionConstraint(ref);
  }
  found.constraint = this->typeResolver->resolveType(ref, false);
  found.resolved = !found.constraint->isError();

  return found;
}

TypeParams::ParamConstraint TypeParams::unionConstraint(const TypeRef &ref) {
  ParamConstraint found;
  for (const TypeRef &member : ref.args) {
    const Type *type = this->typeResolver->resolveType(member, false);
    found.resolved &= !type->isError();
    if (!llvm::is_contained(found.alternatives, type)) {
      found.alternatives.push_back(type);
    }
  }
  const std::vector<const Type *> &alternatives = found.alternatives;
  bool isOptionalPair = found.resolved && alternatives.size() == 2 &&
                        (alternatives[0]->isUndefined() || alternatives[1]->isUndefined());
  if (isOptionalPair) {
    found.constraint = Type::optional(alternatives[alternatives[0]->isUndefined() ? 1 : 0]);
  }

  return found;
}

bool TypeParams::constraintsAlone(const ParamConstraint &found, const TypeParamDecl &decl,
                                  const TypeRef &ref) {
  llvm::ArrayRef<const Type *> all = found.constraint != nullptr
                                         ? llvm::ArrayRef<const Type *>(found.constraint)
                                         : llvm::ArrayRef<const Type *>(found.alternatives);
  for (const Type *type : all) {
    ResultConstraintKept kept = TypeParams::constraintAlone(type, decl);
    if (kept.isErr()) {
      this->state.getDiagnostics().error(ref.loc, kept.error().message);
      return false;
    }
  }

  return true;
}

const Type *TypeParams::resolvedDefault(const TypeParamDecl &decl, const Type *param) {
  if (!decl.defaultType.has_value()) {
    return nullptr;
  }
  const Type *fallback = this->typeResolver->resolveType(*decl.defaultType, false);
  ResultDefaultSatisfy satisfied = this->defaultFits(fallback, param, decl);
  if (satisfied.isErr()) {
    this->state.getDiagnostics().error(decl.defaultType->loc, satisfied.error().message);
    return Type::error();
  }

  return fallback;
}

void TypeParams::bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                llvm::ArrayRef<const Type *> types) {
  for (size_t i = 0; i < decls.size() && i < types.size(); ++i) {
    auto *alias = this->state.getAst().make<TypeAliasDecl>(decls[i].loc, decls[i].name, TypeRef());
    alias->resolved = types[i];
    alias->boundParam = true;
    this->state.getScopeStack().setType(decls[i].name, alias);
  }
}

const Type *TypeParams::substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                                   llvm::ArrayRef<const Type *> args) {
  return this->substituteAt(type, params, args, true);
}

const Type *TypeParams::substituteValue(const Type *type, llvm::ArrayRef<const Type *> params,
                                        llvm::ArrayRef<const Type *> args) {
  return this->substituteAt(type, params, args, false);
}

const Type *TypeParams::substituteAt(const Type *type, llvm::ArrayRef<const Type *> params,
                                     llvm::ArrayRef<const Type *> args, bool voidOk) {
  std::vector<SubstituteStep> steps = {SubstituteType{type, voidOk}};
  std::vector<const Type *> answers;
  while (!steps.empty()) {
    SubstituteStep step = steps.back();
    steps.pop_back();
    if (const SubstituteType *enter = step.as<SubstituteType>()) {
      this->enterSubstitute(*enter, params, args, steps, answers);
    } else {
      const Type *built = this->buildSubstituted(*step.as<BuildSubstituted>(), answers);
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
  bool isUntouched = type == nullptr || !GenericCheckHelpers::mentionsAnyOf(type, params);
  std::optional<SubstituteForm> form = isUntouched ? std::nullopt : this->substituteForm(type);
  if (isUntouched) {
    answers.push_back(type);
  } else if (!form.has_value()) {
    answers.push_back(TypeParams::plainSubstitute(type, params, args, enter.voidOk));
  } else {
    if (*form == SubstituteForm::OBJECT) {
      this->state.getGenericStore().enterSubstitution(type);
    }
    std::vector<SubstituteType> parts = this->substituteParts(type, *form);
    steps.push_back(BuildSubstituted{type, *form, parts.size()});
    for (const SubstituteType &part : llvm::reverse(parts)) {
      steps.push_back(part);
    }
  }
}

const Type *TypeParams::plainSubstitute(const Type *type, llvm::ArrayRef<const Type *> params,
                                        llvm::ArrayRef<const Type *> args, bool voidOk) {
  if (!type->isParam()) {
    return type;
  }
  const Type *arg = args[size_t(llvm::find(params, type) - params.begin())];
  if (arg && arg->isVoid() && !voidOk) {
    return Type::undefinedTy();
  }

  return arg ? arg : type;
}

std::optional<SubstituteForm> TypeParams::substituteForm(const Type *type) const {
  switch (type->kind()) {
    case Type::Kind::Param:
      return type->typeOp() != TypeOp::None ? std::optional(SubstituteForm::TYPE_OPERATOR)
                                            : std::nullopt;
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
      return type->isConstructor() ? std::optional(SubstituteForm::CONSTRUCTOR) : std::nullopt;
    case Type::Kind::Object:
      return this->objectForm(type);
    default:
      return std::nullopt;
  }
}

std::optional<SubstituteForm> TypeParams::objectForm(const Type *type) const {
  if (type->isAsyncIterator()) {
    return SubstituteForm::ASYNC_ITERATOR;
  }
  if (this->state.getAliasStore().findDescriptorOrigin(type)) {
    return SubstituteForm::DESCRIPTOR;
  }
  if (const ClassDecl *cls = type->classDecl(); cls && !type->typeArgs().empty()) {
    return SubstituteForm::CLASS_INSTANCE;
  }
  if (this->state.getAliasStore().findAliasOrigin(type)) {
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
      return TypeParams::partsOf(type->operands(), true);
    case SubstituteForm::OPTIONAL:
    case SubstituteForm::ARRAY:
      return {SubstituteType{type->inner(), false}};
    case SubstituteForm::RESULT:
      return {SubstituteType{type->okType(), true}, SubstituteType{type->errType(), true}};
    case SubstituteForm::FUNCTION: {
      std::vector<SubstituteType> parts = TypeParams::partsOf(type->params(), false);
      parts.push_back(SubstituteType{type->ret(), true});
      return parts;
    }
    case SubstituteForm::ITERATOR:
    case SubstituteForm::ASYNC_ITERATOR:
      return {SubstituteType{type->yielded(), false}, SubstituteType{type->returned(), true},
              SubstituteType{type->sent(), true}};
    case SubstituteForm::DICT:
      return {SubstituteType{type->keyType(), false}, SubstituteType{type->valueType(), false}};
    case SubstituteForm::UNION:
      return TypeParams::partsOf(type->members(), false);
    case SubstituteForm::PROMISE:
      return {SubstituteType{type->promised(), true}};
    case SubstituteForm::CONSTRUCTOR:
      return {SubstituteType{type->constructed(), false}};
    case SubstituteForm::DESCRIPTOR: {
      const DescriptorOrigin *origin = this->state.getAliasStore().findDescriptorOrigin(type);
      return {SubstituteType{origin->args, false}, SubstituteType{origin->ret, true}};
    }
    case SubstituteForm::CLASS_INSTANCE:
      return TypeParams::partsOf(type->typeArgs(), true);
    case SubstituteForm::ALIAS_INSTANCE:
      return TypeParams::partsOf(this->state.getAliasStore().findAliasOrigin(type)->args, true);
    case SubstituteForm::TUPLE: {
      std::vector<SubstituteType> parts = TypeParams::fieldParts(type);
      if (!TypeParams::hasListParam(type) && type->tupleRest()) {
        parts.push_back(SubstituteType{type->tupleRest(), false});
      }
      return parts;
    }
    case SubstituteForm::OBJECT:
      return TypeParams::fieldParts(type);
  }
  llvm_unreachable("unknown substitution");
}

std::vector<SubstituteType> TypeParams::partsOf(llvm::ArrayRef<const Type *> types, bool voidOk) {
  std::vector<SubstituteType> parts;
  for (const Type *part : types) {
    parts.push_back(SubstituteType{part, voidOk});
  }

  return parts;
}

std::vector<SubstituteType> TypeParams::fieldParts(const Type *type) {
  std::vector<SubstituteType> parts;
  for (const Field &f : type->fields()) {
    parts.push_back(SubstituteType{f.type, false});
  }

  return parts;
}

bool TypeParams::hasListParam(const Type *tuple) {
  for (const Field &f : tuple->fields()) {
    if (f.type->isListParam()) {
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
    case SubstituteForm::FUNCTION: {
      const Type *ret = parts.back();
      parts.pop_back();
      return Type::function(parts, type->minArgs(), type->hasRest(), ret, type->hasThis());
    }
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
    case SubstituteForm::ALIAS_INSTANCE: {
      TypeAliasDecl *alias = this->state.getAliasStore().findAliasOrigin(type)->alias;
      return this->aliasInstantiator->instantiateAlias(alias, std::move(parts), alias->loc());
    }
    case SubstituteForm::TUPLE:
      return TypeParams::substitutedTuple(type, std::move(parts));
    case SubstituteForm::OBJECT:
      return this->substitutedObject(type, parts);
  }
  llvm_unreachable("unknown substitution");
}

const Type *TypeParams::substitutedInstance(const Type *type, std::vector<const Type *> newArgs) {
  const ClassDecl *cls = type->classDecl();
  auto *generic = const_cast<ClassDecl *>(cls->generic() ? cls->generic() : cls);
  GenericClassInstantiator::ResultClassMade made = this->genericClassInstantiator->instantiateClass(
      generic, std::move(newArgs), this->state.getGenericStore().getInstanceLoc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? generic->loc()
            : (this->state.getGenericStore().getInstanceLoc().isValid()
                   ? this->state.getGenericStore().getInstanceLoc()
                   : generic->loc());
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return made.isOk() ? made.value()->type : Type::error();
}

const Type *TypeParams::substitutedTuple(const Type *type, std::vector<const Type *> parts) {
  const Type *rest = nullptr;
  if (!TypeParams::hasListParam(type) && type->tupleRest()) {
    rest = parts.back();
    parts.pop_back();
  }
  std::vector<bool> spread;
  for (const Field &f : type->fields()) {
    spread.push_back(f.type->isListParam());
  }
  if (llvm::is_contained(spread, true)) {
    std::string why;
    const Type *list = ArgumentLists::spreadTuple(parts, spread, why);
    return list ? list : Type::error();
  }
  if (type->tupleMin() < parts.size() || type->tupleRest()) {
    return Type::argumentList(parts, type->tupleMin(), rest);
  }

  return Type::tuple(parts, !type->fields().empty() && type->fields().front().readonly);
}

const Type *TypeParams::substitutedObject(const Type *type, llvm::ArrayRef<const Type *> parts) {
  this->state.getGenericStore().leaveSubstitution(type);
  std::vector<Field> fields;
  for (size_t i = 0; i < parts.size(); ++i) {
    const Field &f = type->fields()[i];
    fields.push_back({f.name, parts[i], f.readonly});
  }

  return Type::object(std::move(fields));
}

const Type *TypeParams::representation(const Type *type) {
  std::vector<const Type *> answers;
  for (const Type *node : ChildrenFirst::order(type, TypeParams::representationParts)) {
    const Type *shown = TypeParams::nodeRepresentation(node, answers);
    answers.push_back(shown);
  }

  return answers.back();
}

NodeChildren<const Type *> TypeParams::representationParts(const Type *type) {
  switch (type->kind()) {
    case Type::Kind::Array:
    case Type::Kind::Optional:
      return {type->inner()};
    case Type::Kind::Result:
      return {type->okType(), type->errType()};
    case Type::Kind::Union:
      return NodeChildren<const Type *>(type->members().begin(), type->members().end());
    default:
      return {};
  }
}

const Type *TypeParams::nodeRepresentation(const Type *type, std::vector<const Type *> &answers) {
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
  case Type::Kind::Result: {
    const Type *err = ChildrenFirst::takeAnswer(answers);
    const Type *ok = ChildrenFirst::takeAnswer(answers);
    return Type::result(ok, err);
  }
  case Type::Kind::Union: {
    std::vector<const Type *> members = ChildrenFirst::takeAnswers(answers, type->members().size());
    return Type::unionOf(members);
  }
  default:
    return type;
  }
}

bool TypeParams::satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params,
                           llvm::ArrayRef<const Type *> args) {
  if (arg->isError()) {
    return true;
  }
  const Type *own = param->constraint();
  if (own && !params.empty() && GenericCheckHelpers::mentionsAnyOf(own, params)) {
    own = this->substitute(own, params, args);
  }
  if (own && own->isError()) {
    return true;
  }
  if (arg->isParam() && arg->constraint() && own &&
      TypeRelations::isImplicitlyConvertible(arg->constraint(), own)) {
    return true;
  }
  auto fits = [&](const Type *constraint) {
    return TypeRelations::isImplicitlyConvertible(arg, constraint) ||
           (constraint->isObject() && this->hasMembersOf(arg, constraint));
  };
  if (own) {
    return fits(own);
  }
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();

  return alternatives.empty() || llvm::any_of(alternatives, fits);
}

bool TypeParams::hasMembersOf(const Type *arg, const Type *constraint) {
  TypeParts parts = TypeParts::collect(arg, kMembersOfRoute);
  for (const Type *part : parts.getTypes()) {
    bool isConstrained = part->isParam() && part->constraint() != nullptr;
    if (isConstrained || part->isUnion()) {
      continue;
    }
    if (!this->objectHasMembersOf(part, constraint)) {
      return false;
    }
  }

  return true;
}

bool TypeParams::objectHasMembersOf(const Type *arg, const Type *constraint) {
  if (!arg->isObject() || arg->isTuple() != constraint->isTuple() || constraint->isClass() ||
      arg->isAnyObject()) {
    return false;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(arg)) {
    this->classDeclarer->completeClass(cls);
  }
  const ClassDecl *cls = arg->classDecl();
  for (const Field &f : constraint->fields()) {
    if (const Field *have = arg->field(f.name)) {
      bool isLiteral =
          f.type->isString() &&
          (have->type->isStringLiteral() ||
           (have->type->isUnion() && llvm::all_of(have->type->members(), [](const Type *m) {
              return m->isStringLiteral();
            })));
      if ((have->type != f.type && !isLiteral) || (have->readonly && !f.readonly)) {
        return false;
      }
      continue;
    }
    bool isMethod = false;
    if (cls && f.readonly && f.type->isFunction()) {
      for (FunctionDecl *fn : ClassDeclarer::methodsNamed(cls, f.name, false)) {
        isMethod |= fn->sym && fn->typeParams.empty() &&
                    this->returnInferrer->functionType(fn->sym) == f.type;
      }
    }
    if (!isMethod && !f.type->isOptional()) {
      return false;
    }
  }
  for (const Overload &overload : constraint->overloads()) {
    if (!cls) {
      return false;
    }
    std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, overload.name, false);
    for (const Type *signature : overload.signatures) {
      if (llvm::none_of(methods, [&](FunctionDecl *fn) {
            return fn->sym && this->returnInferrer->functionType(fn->sym) == signature;
          })) {
        return false;
      }
    }
  }

  return true;
}

bool TypeParams::exactTypeParam(FuncSymbol *generic, size_t k) {
  std::optional<std::vector<bool>> found = this->state.getGenericStore().findExactParams(generic);
  if (!found) {
    llvm::ArrayRef<const Type *> params = generic->typeParams;
    std::vector<bool> exact(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i) {
      auto objectLike = [](const Type *t) { return t && t->nonOptional()->isObject(); };
      exact[i] = objectLike(params[i]->constraint()) ||
                 llvm::any_of(params[i]->alternatives(), objectLike);
      exact[i] = exact[i] || params[i]->iterated() || params[i]->isListParam();
      for (const Type *p : params) {
        exact[i] = exact[i] ||
                   (p->iterated() && GenericCheckHelpers::mentionsAnyOf(p->iterated(), params[i]));
      }
      for (const Type *p : params) {
        exact[i] = exact[i] || (p != params[i] && p->constraint() &&
                                GenericCheckHelpers::mentionsAnyOf(p->constraint(), params[i]));
      }
      for (const Type *p : generic->params) {
        exact[i] = exact[i] || (p && TypeParams::mentionsInObject(p, params[i]));
      }
      if (generic->hasReturnType()) {
        exact[i] =
            exact[i] || TypeParams::mentionsInObject(generic->getKnownReturnType(), params[i]);
      }
    }
    this->state.getGenericStore().rememberExactParams(generic, exact);
    found = std::move(exact);
  }

  return k < found->size() && (*found)[k];
}

}
