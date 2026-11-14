#include "compiler/semantic/code/iterators/code_iteration.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"

namespace tz {

const Type *CodeIteration::iteratedType(const Type *type) const {
  if (type->isArray()) {
    return type->inner();
  }
  if (type->isString()) {
    return type;
  }
  if (type->isIterator()) {
    return type->yielded();
  }
  if (type->isTypedArray()) {
    return type->elementType();
  }
  if (type->isParam() && type->iterated() != nullptr) {
    return type->iterated();
  }

  return nullptr;
}

const Type *CodeIteration::asyncIteratedType(const Type *type) const {
  if (type->isAsyncIterator()) {
    return type->yielded();
  }
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises) {
    return Type::string();
  }
  const auto *element = this->iteratedType(type);
  if (element == nullptr) {
    return nullptr;
  }
  const auto *awaited = AsyncChecker::awaitedType(element);

  return awaited != nullptr && awaited->isVoid() ? Type::undefinedTy() : awaited;
}

CodeIteration::ResultSendsNothing CodeIteration::checkSendsNothing(const Type *iterator,
                                                                   llvm::StringRef what) const {
  const Type *sent = iterator->sent();
  bool isFree = sent->isVoid() || sent->isOptional() || sent->isError();
  if (!isFree) {
    auto message = ErrorTextSendsNothing::expects(what, iterator->name(), sent->name());

    return err(CodeIteratorsRegistryCheckSendsNothingError{
        .code = CodeIteratorsRegistryCheckSendsNothingErrorCode::EXPECTS_VALUE,
        .message = message,
    });
  }

  return ok(iterator);
}

}
