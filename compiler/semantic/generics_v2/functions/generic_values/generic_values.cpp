#include "compiler/semantic/generics_v2/functions/generic_values/generic_values.h"
#include "compiler/semantic/generics_v2/inference/candidate_inference/candidate_inference.h"
#include "compiler/semantic/types/type_parts/type_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"

namespace tz {

GenericValues::GenericValues(SemanticState &owner,
                             FunctionInstanceRules &sharedFunctionInstanceRules)
    : state(owner),
      functionInstanceRules(sharedFunctionInstanceRules) {}

void GenericValues::checkGenericValue(const CheckGenericValue &task) {
  NameExpr *name = task.name;
  FuncSymbol *generic = task.generic;
  auto &tasks = this->state.walk.tasks;
  name->type = Type::error();
  const auto *declaredReturn = generic->getReturnType();
  bool isOwnClass = this->returnsOwnClass(generic, declaredReturn);
  const Signature *expected = task.wanted.has_value() ? &*task.wanted : nullptr;
  auto given = this->typeArgsGiven(generic, name, expected);
  if (given.isErr()) {
    this->state.diagnostics.error(name->loc(), given.error().message);

    return tasks.pushChecked(name);
  }
  auto draft = GenericValueDraft{
      .name = name,
      .generic = generic,
      .returnType = isOwnClass ? nullptr : declaredReturn,
      .bound = std::vector<const Type *>(generic->typeParams.size(), nullptr),
  };
  if (name->typeArgs.empty()) {
    if (expected != nullptr) {
      this->bindFromExpected(generic, draft.returnType, *expected, draft.bound);
    }

    return this->finishGenericValueArgs(std::move(draft));
  }
  auto counted = this->typeArgCount(generic, name);
  if (counted.isErr()) {
    this->state.diagnostics.error(name->loc(), counted.error().message);

    return tasks.pushChecked(name);
  }

  return this->nextValueTypeArg(NextValueTypeArg{.draft = std::move(draft), .index = 0});
}

void GenericValues::nextValueTypeArg(const NextValueTypeArg &task) {
  NameExpr *name = task.draft.name;
  if (task.index == name->typeArgs.size()) {
    return this->finishGenericValueArgs(task.draft);
  }
  auto &tasks = this->state.walk.tasks;
  const TypeRef &ref = name->typeArgs[task.index];
  tasks.pushTask(AfterValueTypeArg{.draft = task.draft, .index = task.index});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &ref, .use = TypeUse::STANDALONE_OR_VOID});
}

void GenericValues::afterValueTypeArg(const AfterValueTypeArg &task) {
  GenericValueDraft draft = task.draft;
  draft.bound[task.index] = this->state.walk.tasks.takeAnswer();

  return this->nextValueTypeArg(
      NextValueTypeArg{.draft = std::move(draft), .index = task.index + 1});
}

void GenericValues::finishGenericValueArgs(GenericValueDraft draft) {
  auto &tasks = this->state.walk.tasks;
  auto finish = FinishTypeArgs{
      .generic = draft.generic,
      .bound = draft.bound,
      .loc = draft.name->loc(),
      .use = TypeArgUse::VALUE,
      .missing = MissingTypeArgs::REPORTED,
  };
  tasks.pushTask(AfterGenericValueArgs{.draft = std::move(draft)});

  return tasks.pushTask(std::move(finish));
}

void GenericValues::afterGenericValueArgs(const AfterGenericValueArgs &task) {
  GenericValueDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  bool isFinished = tasks.takeVerdict();
  if (isFinished) {
    draft.bound = tasks.takeAnswers(draft.generic->typeParams.size());
  }
  if (!isFinished || draft.returnType == nullptr) {
    return tasks.pushChecked(draft.name);
  }
  FuncSymbol *generic = draft.generic;
  auto list = TypeListDraft{
      .types = {},
      .isValues = {},
      .params = generic->typeParams,
      .args = draft.bound,
      .index = 0,
      .results = {},
  };
  for (const Type *param : generic->params) {
    list.types.push_back(param);
    list.isValues.push_back(true);
  }
  list.types.push_back(draft.returnType);
  list.isValues.push_back(false);
  tasks.pushTask(AfterGenericValueType{.draft = std::move(draft)});

  return tasks.pushTask(SubstituteTypeList{.draft = std::move(list)});
}

void GenericValues::afterGenericValueType(const AfterGenericValueType &task) {
  const GenericValueDraft &draft = task.draft;
  NameExpr *name = draft.name;
  FuncSymbol *generic = draft.generic;
  auto &tasks = this->state.walk.tasks;
  auto params = tasks.takeAnswers(generic->params.size() + 1);
  const Type *result = params.back();
  params.pop_back();
  name->binding = NamedFunction{.func = generic};
  name->type = Type::function(params, generic->minArgs, generic->hasRest, result, generic->hasThis);
  tasks.pushChecked(name);
  bool isConcrete = this->isConcrete(draft.bound);
  if (!isConcrete || this->state.walk.functions.isInsideTrial() || generic->inferring) {
    return;
  }

  return this->functionInstanceRules.startInstantiate(InstantiateDraft{
      .generic = generic,
      .args = draft.bound,
      .loc = name->loc(),
      .exact = {},
      .reps = {},
      .name = name,
      .call = nullptr,
      .isCopyingReturn = false,
      .instance = nullptr,
      .function = nullptr,
      .copy = nullptr,
      .index = 0,
      .isPrecise = false,
  });
}

const ClassDecl *GenericValues::literalClassIn(const Type *type) const {
  auto parts = TypeParts::collect(type, kLiteralClassRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *classDecl = part->classDecl();
    if (classDecl != nullptr && classDecl->literal() != nullptr) {
      return classDecl;
    }
  }

  return nullptr;
}

GenericValues::ResultTypeArgsCount GenericValues::typeArgCount(const FuncSymbol *generic,
                                                               const NameExpr *name) const {
  if (name->typeArgs.size() > generic->typeParams.size()) {
    auto message = ErrorTextGenericCount::tooMany(generic->name, generic->typeParams.size(),
                                                  name->typeArgs.size());

    return err(GenericsFunctionsRegistryTypeArgCountError{
        .code = GenericsFunctionsRegistryTypeArgCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name->typeArgs.size());
}

GenericValues::ResultTypeArgsGiven GenericValues::typeArgsGiven(const FuncSymbol *generic,
                                                                const NameExpr *name,
                                                                const Signature *expected) const {
  if (name->typeArgs.empty() && expected == nullptr) {
    auto message = ErrorTextGenericValue::needsArgs(generic->name);

    return err(GenericsFunctionsRegistryTypeArgsGivenError{
        .code = GenericsFunctionsRegistryTypeArgsGivenErrorCode::NEEDS_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name);
}

GenericValues::ResultNoClassReturn GenericValues::ownClassReturn(const FuncSymbol *generic,
                                                                 const ClassDecl *classDecl) const {
  if (classDecl != nullptr) {
    auto message = ErrorTextOwnClass::literal(generic->name);

    return err(GenericsFunctionsRegistryOwnClassReturnError{
        .code = GenericsFunctionsRegistryOwnClassReturnErrorCode::RETURNS_LITERAL_CLASS,
        .message = message,
    });
  }

  return ok(generic);
}

bool GenericValues::returnsOwnClass(const FuncSymbol *generic, const Type *returnType) {
  const ClassDecl *classDecl = returnType != nullptr ? this->literalClassIn(returnType) : nullptr;
  auto plain = this->ownClassReturn(generic, classDecl);
  if (plain.isOk()) {
    return false;
  }
  if (!this->state.generics.copies.isOwnClassReturnReported(generic)) {
    this->state.generics.copies.markOwnClassReturnReported(generic);
    this->state.diagnostics.error(classDecl->literal()->loc(), plain.error().message);
  }

  return true;
}

void GenericValues::bindFromExpected(const FuncSymbol *generic, const Type *returnType,
                                     const Signature &expected, std::vector<const Type *> &bound) {
  llvm::ArrayRef<const Type *> genericTypeParams = generic->typeParams;
  CandidateInference fromParams(genericTypeParams, this->state.generics.aliases,
                                IterationLookups::NONE);
  CandidateInference fromResult(genericTypeParams, this->state.generics.aliases,
                                IterationLookups::NONE);
  size_t shared = std::min(generic->params.size(), expected.params.size());
  for (size_t index = 0; index < shared; ++index) {
    if (expected.params[index] != nullptr) {
      fromParams.from(generic->params[index], expected.params[index]);
    }
  }
  if (returnType != nullptr && expected.ret != nullptr && !expected.voidResult) {
    fromResult.from(returnType, expected.ret);
  }
  for (size_t index = 0; index < genericTypeParams.size(); ++index) {
    for (const CandidateInference *inference : {&fromParams, &fromResult}) {
      const auto *type = this->unifiedCandidates(*inference, index);
      if (type != nullptr && bound[index] == nullptr) {
        bound[index] = type;
      }
    }
  }
}

const Type *GenericValues::unifiedCandidates(const CandidateInference &inference,
                                             size_t index) const {
  const Type *type = nullptr;
  for (const Type *candidate : inference.getCandidates(index)) {
    type = type != nullptr ? this->state.types.relations.unify(type, candidate) : candidate;
    if (type == nullptr) {
      return nullptr;
    }
  }

  return type;
}

bool GenericValues::isConcrete(llvm::ArrayRef<const Type *> bound) const {
  for (const Type *type : bound) {
    if (TypeQueries::mentionsParam(type)) {
      return false;
    }
  }

  return true;
}

}
