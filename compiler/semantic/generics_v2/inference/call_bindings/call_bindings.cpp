#include "compiler/semantic/generics_v2/inference/call_bindings/call_bindings.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics_v2/functions/generics_functions_registry_errors.h"
#include "compiler/semantic/types/type_parts/type_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"

namespace tz {

namespace {

constexpr unsigned kAnyArgs = ~0u;

}

CallBindings::CallBindings(SemanticState &owner, BindingRules &sharedBindingRules)
    : state(owner),
      bindingRules(sharedBindingRules) {}

void CallBindings::literalShape(const LiteralShape &task) {
  const auto *shape = this->bindingRules.literalShapeOf(task.init);

  return this->state.walk.tasks.pushAnswer(shape);
}

void CallBindings::checkGenericCall(const CheckGenericCall &task) {
  return this->startBinding(task.call, task.function, task.loc, task.expected, task.typeArgs,
                            BindingOwner::CALL);
}

void CallBindings::bindGenericCall(const BindGenericCall &task) {
  return this->startBinding(task.call, task.function, task.loc, task.expected, task.typeArgs,
                            BindingOwner::BIND);
}

void CallBindings::startBinding(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                const Type *expected, llvm::ArrayRef<TypeRef> typeArgs,
                                BindingOwner owner) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  const auto *result = generic->getReturnType();
  bool isOwnClass = this->returnsOwnClass(generic, result);
  auto draft = GenericBindingDraft{
      .call = call,
      .generic = generic,
      .loc = loc,
      .expected = expected,
      .typeArgs = typeArgs,
      .owner = owner,
      .result = isOwnClass ? nullptr : result,
      .listRest = this->listRestOf(generic),
      .fixed = generic->params.size() - (generic->hasRest ? 1 : 0),
      .errorsBefore = this->state.diagnostics.getWrittenErrorCount(),
      .bound = std::vector<const Type *>(generic->typeParams.size(), nullptr),
      .checked = std::vector<bool>(call->args.size(), false),
      .literals = {},
      .contextual = {},
      .params = {},
      .isInferred = true,
      .inference = std::nullopt,
      .pending = {},
      .index = 0,
  };
  size_t limit = generic->hasRest ? draft.fixed : call->args.size();
  size_t count = std::min(call->args.size(), limit);
  for (size_t index = 0; index < count; ++index) {
    auto plain = this->checkSpreadFree(call, index);
    if (plain.isErr()) {
      this->state.diagnostics.error(call->args[index]->loc(), plain.error().message);
      tasks.pushTask(WalkArgsLoosely{.call = call});

      return this->bindingRules.deliverFailure(draft);
    }
  }
  auto minArgs = static_cast<unsigned>(generic->minArgs);
  unsigned maxArgs = generic->hasRest ? kAnyArgs : static_cast<unsigned>(generic->params.size());
  auto arity = this->checkArity(call, minArgs, maxArgs);
  if (arity.isErr()) {
    this->state.diagnostics.error(loc, arity.error().message);

    return this->bindingRules.deliverFailure(draft);
  }
  if (!typeArgs.empty()) {
    return this->bindExplicitTypeArgs(std::move(draft));
  }
  draft.inference.emplace(generic->typeParams, this->state.generics.aliases,
                          IterationLookups::ELEMENTS_AND_ASYNC);

  return tasks.pushTask(NextBindingArgument{.draft = std::move(draft)});
}

CallBindings::ResultSpreadIsFree CallBindings::checkSpreadFree(const CallExpr *call,
                                                               size_t index) const {
  if (call->spreads(index)) {
    auto message = ErrorTextSpread::notRest();

    return err(GenericsInferenceRegistrySpreadFreeError{
        .code = GenericsInferenceRegistrySpreadFreeErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok(call);
}

CallBindings::ResultArityMatches CallBindings::checkArity(const CallExpr *call, unsigned min,
                                                          unsigned max) const {
  size_t count = call->args.size();
  bool isFitting = count >= min && count <= max;
  if (!isFitting && call->tagged) {
    auto message = ErrorTextArity::tagged(min, max, count);

    return err(GenericsInferenceRegistryArityError{
        .code = GenericsInferenceRegistryArityErrorCode::WRONG_COUNT_TAGGED,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextArity::count(min, max, count);

    return err(GenericsInferenceRegistryArityError{
        .code = GenericsInferenceRegistryArityErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(call);
}

bool CallBindings::returnsOwnClass(const FuncSymbol *generic, const Type *returnType) {
  const ClassDecl *classDecl = returnType != nullptr ? this->literalClassIn(returnType) : nullptr;
  auto plain = this->ownClassReturn(generic, classDecl);
  if (plain.isOk()) {
    return false;
  }
  auto &copies = this->state.generics.copies;
  if (!copies.isOwnClassReturnReported(generic)) {
    copies.markOwnClassReturnReported(generic);
    this->state.diagnostics.error(classDecl->literal()->loc(), plain.error().message);
  }

  return true;
}

const ClassDecl *CallBindings::literalClassIn(const Type *type) const {
  auto parts = TypeParts::collect(type, kLiteralClassRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *classDecl = part->classDecl();
    if (classDecl != nullptr && classDecl->literal() != nullptr) {
      return classDecl;
    }
  }

  return nullptr;
}

CallBindings::ResultNoClassReturn CallBindings::ownClassReturn(const FuncSymbol *generic,
                                                               const ClassDecl *classDecl) const {
  if (classDecl != nullptr) {
    auto message = ErrorTextOwnClass::literal(generic->name);

    return err(GenericsInferenceRegistryOwnClassReturnError{
        .code = GenericsInferenceRegistryOwnClassReturnErrorCode::RETURNS_LITERAL_CLASS,
        .message = message,
    });
  }

  return ok(generic);
}

const Type *CallBindings::listRestOf(const FuncSymbol *generic) const {
  if (!generic->hasRest) {
    return nullptr;
  }
  const Type *rest = generic->params.back();
  bool isList = rest->isListParam() || ArgListChecker::isSpreadingLists(rest);

  return isList ? rest : nullptr;
}

void CallBindings::bindExplicitTypeArgs(GenericBindingDraft draft) {
  auto &tasks = this->state.walk.tasks;
  auto counted = this->typeArgCount(draft.generic, draft.typeArgs);
  if (counted.isErr()) {
    this->state.diagnostics.error(draft.loc, counted.error().message);
    draft.isInferred = false;
  }
  llvm::ArrayRef<TypeRef> typeArgs = draft.typeArgs;
  size_t count = std::min(typeArgs.size(), draft.generic->typeParams.size());
  tasks.pushTask(AfterExplicitTypeArgs{.draft = std::move(draft), .count = count});
  for (size_t index = count; index > 0; --index) {
    tasks.pushTask(
        ResolveTypeAnswer{.ref = &typeArgs[index - 1], .use = TypeUse::STANDALONE_OR_VOID});
  }
}

void CallBindings::afterExplicitTypeArgs(const AfterExplicitTypeArgs &task) {
  auto draft = task.draft;
  auto resolved = this->state.walk.tasks.takeAnswers(task.count);
  for (size_t index = 0; index < resolved.size(); ++index) {
    draft.bound[index] = resolved[index];
  }

  return this->bindingRules.finishBinding(std::move(draft));
}

CallBindings::ResultTypeArgsCount
CallBindings::typeArgCount(const FuncSymbol *generic, llvm::ArrayRef<TypeRef> typeArgs) const {
  if (typeArgs.size() > generic->typeParams.size()) {
    auto message =
        ErrorTextTypeArgCount::tooMany(generic->name, generic->typeParams.size(), typeArgs.size());

    return err(GenericsInferenceRegistryTypeArgCountError{
        .code = GenericsInferenceRegistryTypeArgCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(typeArgs.size());
}

}
