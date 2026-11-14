#include "compiler/semantic/code/iterators/iterator_signatures/iterator_signatures.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/iterator/iterator_result_field_table/iterator_result_field_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kIteratorMethod = "[Symbol.iterator]";
constexpr llvm::StringLiteral kAsyncIteratorMethod = "[Symbol.asyncIterator]";

}

IteratorSignatures::IteratorSignatures(SemanticState &owner)
    : state(owner) {}

void IteratorSignatures::resolveIteratorType(const ResolveIteratorType &task) {
  const TypeRef &ref = *task.ref;
  auto form = this->checkIteratorForm(ref);
  if (form.isErr()) {
    this->state.diagnostics.error(ref.loc, form.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  if (form.value() == IteratorForm::RESULT) {
    this->state.walk.tasks.pushTask(FinishIteratorResult{.ref = task.ref});
    return this->pushIteratorArguments(ref);
  }
  bool isAsync = form.value() == IteratorForm::ASYNC;
  this->state.walk.tasks.pushTask(FinishIteratorArgs{.ref = task.ref, .isAsync = isAsync});

  return this->pushIteratorArguments(ref);
}

IteratorSignatures::ResultIteratorForm
IteratorSignatures::checkIteratorForm(const TypeRef &ref) const {
  bool isUnshadowed = this->state.findTypeDeclaration(ref.name) == nullptr;
  bool isResult = TypeNameTable::isName(ref.name, TypeName::ITERATOR_RESULT) && isUnshadowed;
  if (isResult && (ref.args.empty() || ref.args.size() > 2)) {
    auto message = ErrorTextIteratorType::resultArgs();

    return err(CodeIteratorsRegistryResolveIteratorTypeError{
        .code = CodeIteratorsRegistryResolveIteratorTypeErrorCode::RESULT_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  if (isResult) {
    return ok(IteratorForm::RESULT);
  }
  if (TypeNameTable::isName(ref.name, TypeName::ITERABLE) && isUnshadowed) {
    auto message = ErrorTextIteratorType::iterable();

    return err(CodeIteratorsRegistryResolveIteratorTypeError{
        .code = CodeIteratorsRegistryResolveIteratorTypeErrorCode::ITERABLE_TYPE,
        .message = message,
    });
  }
  if (ref.args.empty() || ref.args.size() > 3) {
    auto message = ErrorTextIteratorType::typeArgs(ref.name);

    return err(CodeIteratorsRegistryResolveIteratorTypeError{
        .code = CodeIteratorsRegistryResolveIteratorTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const auto *builtin = TypeNameTable::find(ref.name);
  bool isAsync = builtin->form.as<IteratorTypeName>()->async;

  return ok(isAsync ? IteratorForm::ASYNC : IteratorForm::SYNC);
}

void IteratorSignatures::pushIteratorArguments(const TypeRef &ref) {
  for (size_t index = ref.args.size(); index > 0; --index) {
    if (this->isResolvedArgument(ref, index - 1)) {
      this->state.walk.tasks.pushTask(
          ResolveTypeAnswer{.ref = &ref.args[index - 1], .use = TypeUse::STANDALONE});
    }
  }
}

bool IteratorSignatures::isResolvedArgument(const TypeRef &ref, size_t index) const {
  if (index == 0) {
    return true;
  }

  return index < ref.args.size() && !this->isNamingNoValue(ref.args[index]);
}

bool IteratorSignatures::isNamingNoValue(const TypeRef &arg) const {
  if (arg.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  bool isVoid = TypeNameTable::isName(arg.name, TypeName::VOID_TYPE);
  bool isUndefined = TypeNameTable::isName(arg.name, TypeName::UNDEFINED);
  bool isUnknown = TypeNameTable::isName(arg.name, TypeName::UNKNOWN);

  return isVoid || isUndefined || isUnknown;
}

const Type *IteratorSignatures::takeTypeArgument(const TypeRef &ref, size_t index,
                                                 const Type *fallback) {
  if (!this->isResolvedArgument(ref, index)) {
    return fallback;
  }

  return this->state.walk.tasks.takeAnswer();
}

void IteratorSignatures::finishIteratorArgs(const FinishIteratorArgs &task) {
  const TypeRef &ref = *task.ref;
  const Type *sent = this->takeTypeArgument(ref, 2, Type::voidTy());
  const Type *returned = this->takeTypeArgument(ref, 1, Type::voidTy());
  const Type *yielded = this->state.walk.tasks.takeAnswer();
  for (const Type *type : {yielded, returned, sent}) {
    if (type->isError()) {
      return this->state.walk.tasks.pushAnswer(type);
    }
  }
  const Type *iterator = task.isAsync ? Type::asyncIterator(yielded, returned, sent)
                                      : Type::iterator(yielded, returned, sent);

  return this->state.walk.tasks.pushAnswer(iterator);
}

void IteratorSignatures::finishIteratorResult(const FinishIteratorResult &task) {
  const Type *returned = this->takeTypeArgument(*task.ref, 1, Type::undefinedTy());
  const Type *value = this->state.walk.tasks.takeAnswer();
  if (value->isError() || returned->isError()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *result = this->iteratorResultType(value, returned);

  return this->state.walk.tasks.pushAnswer(result);
}

void IteratorSignatures::walkIterableParam(const WalkIterableParam &task) {
  TypeRef *ref = task.ref;
  auto counted = this->checkIterableArguments(*ref);
  if (counted.isErr()) {
    this->state.diagnostics.error(ref->loc, counted.error().message);
    ref->resolved = Type::error();
    return;
  }
  this->state.walk.tasks.pushTask(FinishIterableParam{
      .functionDecl = task.functionDecl,
      .function = task.function,
      .ref = ref,
  });

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref->args[0], .use = TypeUse::STANDALONE});
}

IteratorSignatures::ResultIterableArgs
IteratorSignatures::checkIterableArguments(const TypeRef &ref) const {
  if (ref.args.size() != 1) {
    auto message = ErrorTextIterableParam::typeArgs();

    return err(CodeIteratorsRegistryIterableParamError{
        .code = CodeIteratorsRegistryIterableParamErrorCode::WRONG_ARGUMENT_COUNT,
        .message = message,
    });
  }

  return ok(&ref);
}

void IteratorSignatures::finishIterableParam(const FinishIterableParam &task) {
  TypeRef *ref = task.ref;
  const Type *element = this->state.walk.tasks.takeAnswer();
  auto param = this->checkIterableParam(task.functionDecl, task.function, element, *ref);
  if (param.isErr()) {
    this->state.diagnostics.error(ref->loc, param.error().message);
    ref->resolved = Type::error();
    return;
  }
  ref->resolved = param.value();
}

IteratorSignatures::ResultIterableParam
IteratorSignatures::checkIterableParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                       const Type *element, const TypeRef &ref) {
  if (element->isError()) {
    return ok(element);
  }
  if (functionDecl->isConstructor()) {
    auto message = ErrorTextIterableParam::inCtor(element->name());

    return err(CodeIteratorsRegistryIterableParamError{
        .code = CodeIteratorsRegistryIterableParamErrorCode::CONSTRUCTOR_ITERABLE,
        .message = message,
    });
  }
  if (functionDecl->var != nullptr && !functionDecl->typeParams.empty()) {
    return ok(Type::error());
  }
  if (functionDecl->isClosure() || functionDecl->var != nullptr) {
    auto message = ErrorTextIterableParam::notTop(element->name());

    return err(CodeIteratorsRegistryIterableParamError{
        .code = CodeIteratorsRegistryIterableParamErrorCode::CLOSURE_ITERABLE,
        .message = message,
    });
  }
  const auto *param = this->addIterableTypeParam(functionDecl, function, element, ref);

  return ok(param);
}

void IteratorSignatures::walkGeneratorReturn(const WalkGeneratorReturn &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  const Type *declared = task.function->getKnownReturnType();
  auto generator = this->checkGeneratorReturnType(functionDecl, declared);
  if (generator.isErr()) {
    this->state.diagnostics.error(functionDecl->returnType->loc, generator.error().message);
    task.function->setReturnType(Type::error());
  }
}

void IteratorSignatures::nextValueOf(const NextValueOf &task) {
  const Type *object = task.type->nonOptional();
  if (!object->isObject() || object->isTuple()) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  auto methods = this->plainNextMethods(object);
  if (methods.empty()) {
    const auto *field = this->nextFieldType(object);
    return this->pushNextValue(field);
  }
  this->state.walk.tasks.pushTask(AfterNextMethods{.count = methods.size()});
  for (FunctionDecl *functionDecl : llvm::reverse(methods)) {
    this->state.walk.tasks.pushTask(FunctionTypeOf{.function = functionDecl->sym});
  }
}

std::vector<FunctionDecl *> IteratorSignatures::plainNextMethods(const Type *object) const {
  auto plain = std::vector<FunctionDecl *>();
  ClassDecl *classDecl = this->classOf(object);
  if (classDecl == nullptr) {
    return plain;
  }
  auto nextName = IteratorMethodTable::getName(IteratorMethod::NEXT);
  for (FunctionDecl *functionDecl : classDecl->findMethodsNamed(nextName)) {
    if (this->isPlainNext(functionDecl)) {
      plain.push_back(functionDecl);
    }
  }

  return plain;
}

const Type *IteratorSignatures::nextFieldType(const Type *object) const {
  const auto *field = object->field(IteratorMethodTable::getName(IteratorMethod::NEXT));
  if (field != nullptr && field->type->isFunction()) {
    return field->type;
  }

  return nullptr;
}

void IteratorSignatures::afterNextMethods(const AfterNextMethods &task) {
  const Type *last = this->state.walk.tasks.takeAnswer();
  for (size_t index = 1; index < task.count; ++index) {
    this->state.walk.tasks.takeAnswer();
  }

  return this->pushNextValue(last);
}

void IteratorSignatures::pushNextValue(const Type *next) {
  if (next == nullptr || next->minArgs() != 0 || next->ret()->isError()) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  const auto *value = this->nextResultValue(next->ret());

  return this->state.walk.tasks.pushAnswer(value);
}

void IteratorSignatures::elementTypeOf(const ElementTypeOf &task) {
  const auto *known = this->knownElementType(task.type);
  if (known != nullptr) {
    return this->state.walk.tasks.pushAnswer(known);
  }
  const FunctionDecl *method = this->soleSymbolMethod(task.type, kIteratorMethod);
  if (method == nullptr) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  this->state.walk.tasks.pushTask(AfterSoleIteratorReturn{});

  return this->state.walk.tasks.pushTask(ReturnTypeOf{.function = method->sym});
}

const Type *IteratorSignatures::knownElementType(const Type *type) const {
  if (type == nullptr || type->isError()) {
    return nullptr;
  }
  if (type->isParam()) {
    return type->iterated();
  }
  if (type->isMap()) {
    return Type::tuple({type->keyType(), type->valueType()});
  }
  if (type->isSet()) {
    return type->keyType();
  }
  if (type->intlKind() == IntlKind::Segments) {
    return this->state.library.intl.segmentDataType();
  }

  return this->state.code.iteration.iteratedType(type);
}

const FunctionDecl *IteratorSignatures::soleSymbolMethod(const Type *type,
                                                         llvm::StringRef name) const {
  const ClassDecl *classDecl = this->classOf(type);
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findMethodsNamed(name);
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return nullptr;
  }

  return methods[0];
}

void IteratorSignatures::asyncIterableTypeOf(const AsyncIterableTypeOf &task) {
  const FunctionDecl *method = this->soleSymbolMethod(task.type, kAsyncIteratorMethod);
  if (method == nullptr) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  this->state.walk.tasks.pushTask(AfterAsyncIterableReturn{});

  return this->state.walk.tasks.pushTask(ReturnTypeOf{.function = method->sym});
}

void IteratorSignatures::afterAsyncIterableReturn() {
  const Type *result = this->state.walk.tasks.takeAnswer();
  bool isAsyncIterator = result != nullptr && result->isAsyncIterator();

  return this->state.walk.tasks.pushAnswer(isAsyncIterator ? result : nullptr);
}

void IteratorSignatures::afterSoleIteratorReturn() {
  const Type *result = this->state.walk.tasks.takeAnswer();
  if (result == nullptr || result->isError()) {
    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  if (result->isIterator()) {
    return this->state.walk.tasks.pushAnswer(result->yielded());
  }

  return this->state.walk.tasks.pushTask(NextValueOf{.type = result});
}

IteratorSignatures::ResultGeneratorReturns
IteratorSignatures::checkGeneratorReturnType(const FunctionDecl *functionDecl,
                                             const Type *declared) {
  bool isPlain = !functionDecl->isGenerator || declared->isError();
  if (!isPlain && functionDecl->isAsync && !declared->isAsyncIterator()) {
    const Type *yielded = declared->isIterator() ? declared->yielded() : declared;
    auto shown = this->shownElementName(yielded);
    auto message = ErrorTextGeneratorType::async(shown);

    return err(CodeIteratorsRegistryGeneratorReturnTypeError{
        .code = CodeIteratorsRegistryGeneratorReturnTypeErrorCode::ASYNC_NEEDS_ASYNC_ITERATOR,
        .message = message,
    });
  }
  if (!isPlain && !functionDecl->isAsync && !declared->isIterator()) {
    auto shown = this->shownElementName(declared);
    auto message = ErrorTextGeneratorType::sync(shown);

    return err(CodeIteratorsRegistryGeneratorReturnTypeError{
        .code = CodeIteratorsRegistryGeneratorReturnTypeErrorCode::NEEDS_ITERATOR,
        .message = message,
    });
  }

  return ok(declared);
}

std::string IteratorSignatures::shownElementName(const Type *element) const {
  if (element->isVoid()) {
    return Type::intTy()->name().str();
  }

  return element->name().str();
}

const Type *IteratorSignatures::iteratorResultType(const Type *value, const Type *returned) const {
  bool isNothingReturned = returned == nullptr || returned->isVoid();
  const Type *ending = isNothingReturned ? Type::undefinedTy() : returned;
  const auto *going = Type::object(
      {{IteratorResultFieldTable::getName(IteratorResultField::DONE).str(),
        Type::boolLiteral(false), false},
       {IteratorResultFieldTable::getName(IteratorResultField::VALUE).str(), value, false}});
  const auto *finished = Type::object(
      {{IteratorResultFieldTable::getName(IteratorResultField::DONE).str(), Type::boolLiteral(true),
        false},
       {IteratorResultFieldTable::getName(IteratorResultField::VALUE).str(), ending, false}});

  return Type::unionOf({going, finished});
}

bool IteratorSignatures::isPlainNext(const FunctionDecl *functionDecl) const {
  return functionDecl->sym != nullptr && functionDecl->sym->minArgs == 0 &&
         functionDecl->typeParams.empty();
}

const Type *IteratorSignatures::nextResultValue(const Type *result) const {
  llvm::ArrayRef<const Type *> members =
      result->isUnion() ? result->members() : llvm::ArrayRef<const Type *>(result);
  bool isResultShaped =
      llvm::all_of(members, std::bind_front(&IteratorSignatures::isIteratorResultShape, this));
  if (!isResultShaped) {
    return nullptr;
  }
  auto values = std::vector<const Type *>();
  for (const Type *member : members) {
    if (!this->isFinishedResult(member)) {
      values.push_back(
          member->field(IteratorResultFieldTable::getName(IteratorResultField::VALUE))->type);
    }
  }
  if (values.empty()) {
    return nullptr;
  }
  const auto *value = Type::unionOf(values);

  return value->isUndefined() || value->isVoid() ? nullptr : value;
}

bool IteratorSignatures::isIteratorResultShape(const Type *member) const {
  if (!member->isObject()) {
    return false;
  }
  const Field *done = member->field(IteratorResultFieldTable::getName(IteratorResultField::DONE));
  const Field *value = member->field(IteratorResultFieldTable::getName(IteratorResultField::VALUE));
  if (done == nullptr || value == nullptr) {
    return false;
  }
  bool isBoolLiteral = done->type->isLiteral() && done->type->literalBase()->isBool();

  return done->type->isBool() || isBoolLiteral;
}

bool IteratorSignatures::isFinishedResult(const Type *member) const {
  const Field *done = member->field(IteratorResultFieldTable::getName(IteratorResultField::DONE));

  return done->type->isLiteral() && done->type->literalNumber() != 0;
}

const Type *IteratorSignatures::addIterableTypeParam(FunctionDecl *functionDecl,
                                                     FuncSymbol *function, const Type *element,
                                                     const TypeRef &ref) {
  auto iterable = TypeNameTable::getName(TypeName::ITERABLE);
  auto *param = Type::param(iterable.str() + "<" + element->name().str() + ">");
  param->setIterated(element);
  auto typeParam = TypeParamDecl{
      .name = this->state.ast.hiddenName(),
      .loc = ref.loc,
  };
  functionDecl->typeParams.push_back(typeParam);
  functionDecl->typeArgs.push_back(param);
  function->typeParams.push_back(param);
  function->typeDefaults.push_back(nullptr);

  return param;
}

ClassDecl *IteratorSignatures::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
