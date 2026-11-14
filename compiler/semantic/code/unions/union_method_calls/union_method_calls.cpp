#include "compiler/semantic/code/unions/union_method_calls/union_method_calls.h"

namespace tz {

UnionMethodCalls::UnionMethodCalls(SemanticState &owner, UnionRules &sharedUnionRules)
    : state(owner),
      unionRules(sharedUnionRules) {}

void UnionMethodCalls::checkUnionMethodCall(const CheckUnionMethodCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  call->type = Type::error();
  member->type = Type::error();
  this->state.walk.tasks.pushTask(ResolveUnionMethodCall{.call = call, .member = member});
  for (FuncSymbol *function : this->neededFunctions(member)) {
    this->state.walk.tasks.pushTask(InferReturnType{.function = function});
  }
}

std::vector<FuncSymbol *> UnionMethodCalls::neededFunctions(const MemberExpr *member) const {
  auto needed = std::vector<FuncSymbol *>();
  const Type *receiver = member->object->type;
  if (!receiver->isUnion()) {
    return needed;
  }
  for (const Type *memberType : receiver->members()) {
    const auto *classDecl = this->unionRules.classOf(memberType);
    if (classDecl == nullptr || this->unionRules.hasDataField(classDecl, member->name)) {
      continue;
    }
    auto variants = classDecl->findMethodsNamed(member->name);
    bool isUnknown = variants.size() == 1 && !variants[0]->sym->hasReturnType();
    if (isUnknown) {
      needed.push_back(variants[0]->sym);
    }
  }

  return needed;
}

void UnionMethodCalls::resolveUnionMethodCall(const ResolveUnionMethodCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *receiver = member->object->type;
  auto resolved = this->resolveUnionMethods(member, receiver);
  if (resolved.isErr()) {
    this->state.walk.tasks.pushTask(ReportUnionMethodCall{
        .call = call,
        .member = member,
        .message = resolved.error().message,
    });
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  const Type *functionType = resolved.value().type;
  call->target = UnionMethodCall{.unionMethods = resolved.value().methods};
  member->type = functionType;
  call->type = functionType->ret();
  this->state.walk.tasks.pushChecked(call);
  llvm::ArrayRef<const Type *> params = functionType->params();

  return this->state.walk.tasks.pushTask(CheckArguments{
      .call = call,
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = functionType->minArgs(),
      .hasRest = functionType->hasRest(),
      .loc = member->loc(),
      .checked = {},
  });
}

void UnionMethodCalls::reportUnionMethodCall(const ReportUnionMethodCall &task) {
  this->state.diagnostics.error(task.member->loc(), task.message);

  return this->state.walk.tasks.pushChecked(task.call);
}

const Type *UnionMethodCalls::knownFunctionType(const FuncSymbol *function) const {
  const Type *assumed =
      function->inferring && function->assumed != nullptr ? function->assumed : Type::error();
  const Type *returned = function->hasReturnType() ? function->getKnownReturnType() : assumed;

  return Type::function(function->params, function->minArgs, function->hasRest, returned,
                        function->hasThis);
}

bool UnionMethodCalls::isAllObjects(const Type *type) const {
  for (const Type *member : type->members()) {
    if (!member->isObject()) {
      return false;
    }
  }

  return true;
}

UnionMethodCalls::ResultUnionMethodList
UnionMethodCalls::resolveUnionMethods(const MemberExpr *member, const Type *receiver) {
  auto narrowed = this->checkNarrowedObjects(member, receiver);
  if (narrowed.isErr()) {
    return err(narrowed.error());
  }
  const Type *functionType = nullptr;
  auto methods = std::vector<UnionMethod>();
  for (const Type *memberType : receiver->members()) {
    auto part = this->memberMethod(member, memberType);
    if (part.isErr()) {
      return err(part.error());
    }
    auto callable = this->checkCallablePart(member, memberType, part.value().type);
    if (callable.isErr()) {
      return err(callable.error());
    }
    auto same = this->checkSameMethodType(member, functionType, part.value().type);
    if (same.isErr()) {
      return err(same.error());
    }
    functionType = part.value().type;
    methods.push_back(part.value().entry);
  }

  return ok(UnionMethods{
      .type = functionType,
      .methods = methods,
  });
}

UnionMethodCalls::ResultMethodAllowed
UnionMethodCalls::checkNarrowedObjects(const MemberExpr *member, const Type *receiver) const {
  if (receiver->isTagged() || !this->isAllObjects(receiver)) {
    const char *typeOf = TypeSpelling::typeOfName(receiver->members().front());
    auto message = ErrorTextUnionMember::narrow(member->name, receiver->name(), typeOf);

    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::NOT_NARROWED,
        .message = message,
    });
  }

  return ok();
}

UnionMethodCalls::ResultMethodAllowed
UnionMethodCalls::checkCallablePart(const MemberExpr *member, const Type *memberType,
                                    const Type *partType) const {
  if (partType == nullptr || !partType->isFunction()) {
    const Type *receiver = member->object->type;
    bool isField = partType != nullptr;
    auto message = ErrorTextUnionMethod::notMethod(receiver->name(), memberType->name(),
                                                   member->name, isField);

    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }

  return ok();
}

UnionMethodCalls::ResultMethodAllowed
UnionMethodCalls::checkSameMethodType(const MemberExpr *member, const Type *common,
                                      const Type *partType) const {
  if (common != nullptr && partType != common) {
    const Type *receiver = member->object->type;
    auto message = ErrorTextUnionMethod::differ(receiver->name(), member->name, common->name(),
                                                partType->name());

    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::METHOD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok();
}

UnionMethodCalls::ResultUnionMethodPart UnionMethodCalls::memberMethod(const MemberExpr *member,
                                                                       const Type *memberType) {
  auto *classDecl = this->unionRules.classOf(memberType);
  if (classDecl != nullptr) {
    return this->classMemberMethod(member, memberType, classDecl);
  }
  bool hasField = memberType->field(member->name) != nullptr;
  if (hasField && memberType->isBase()) {
    const Type *receiver = member->object->type;
    auto message = ErrorTextUnionMethod::isBase(member->name, receiver->name(), memberType->name());

    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::INTERFACE_MEMBER,
        .message = message,
    });
  }

  return ok(this->fieldPart(memberType, member->name));
}

UnionMethodCalls::UnionMethodPart UnionMethodCalls::fieldPart(const Type *memberType,
                                                              llvm::StringRef name) const {
  const Field *field = memberType->field(name);
  if (field == nullptr) {
    return this->missingPart(memberType);
  }

  return UnionMethodPart{
      .type = field->type,
      .entry = UnionMethod{.member = memberType, .method = nullptr, .virtualCall = false},
  };
}

UnionMethodCalls::UnionMethodPart UnionMethodCalls::missingPart(const Type *memberType) const {
  return UnionMethodPart{
      .type = nullptr,
      .entry = UnionMethod{.member = memberType, .method = nullptr, .virtualCall = false},
  };
}

UnionMethodCalls::ResultUnionMethodPart
UnionMethodCalls::classMemberMethod(const MemberExpr *member, const Type *memberType,
                                    ClassDecl *classDecl) {
  if (this->unionRules.hasDataField(classDecl, member->name)) {
    return this->classFieldPart(member, memberType, classDecl);
  }
  auto variants = classDecl->findMethodsNamed(member->name);
  if (variants.size() > 1) {
    const Type *receiver = member->object->type;
    auto message =
        ErrorTextUnionMethod::variants(member->name, memberType->name(), receiver->name());

    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::SEVERAL_VARIANTS,
        .message = message,
    });
  }
  if (variants.empty()) {
    return ok(this->missingPart(memberType));
  }

  return this->classVariantMethod(member, memberType, classDecl, variants[0]);
}

UnionMethodCalls::ResultUnionMethodPart
UnionMethodCalls::classFieldPart(const MemberExpr *member, const Type *memberType,
                                 const ClassDecl *classDecl) {
  const ClassDecl *owner = classDecl->findFieldOwner(member->name);
  const ClassField *field = classDecl->findField(member->name);
  auto accessible = this->unionRules.checkAccess(owner, field->access, member->name);
  if (accessible.isErr()) {
    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }

  return ok(this->fieldPart(memberType, member->name));
}

UnionMethodCalls::ResultUnionMethodPart
UnionMethodCalls::classVariantMethod(const MemberExpr *member, const Type *memberType,
                                     ClassDecl *classDecl, FunctionDecl *variant) {
  auto accessible = this->unionRules.checkAccess(variant->cls(), variant->access(), member->name);
  if (accessible.isErr()) {
    return err(CodeUnionsRegistryCheckUnionMethodCallError{
        .code = CodeUnionsRegistryCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }
  FuncSymbol *function = variant->sym;
  auto entry = this->variantEntry(memberType, classDecl, function);
  const auto *functionType = this->knownFunctionType(function);

  return ok(UnionMethodPart{
      .type = functionType,
      .entry = entry,
  });
}

UnionMethod UnionMethodCalls::variantEntry(const Type *memberType, ClassDecl *classDecl,
                                           FuncSymbol *function) const {
  auto *implementation = classDecl->findSoleImplementation(function->decl);
  if (implementation != nullptr) {
    return UnionMethod{.member = memberType, .method = implementation->sym, .virtualCall = false};
  }

  return UnionMethod{.member = memberType, .method = function, .virtualCall = true};
}

}
