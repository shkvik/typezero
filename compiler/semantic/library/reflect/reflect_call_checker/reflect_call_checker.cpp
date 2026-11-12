#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/program/library/reflect/design_key_table/design_key_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ReflectCallChecker::ReflectCallChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->reflectChecker = &all.reflectChecker;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->metadataChecker = &all.metadataChecker;
}

ReflectCallChecker::ResultKeyOfRecords
ReflectCallChecker::checkKeyOfRecords(const Type *type) const {
  if (!type->isString() && !type->isError()) {
    auto message = ErrorTextMetadataKey::type(type->name());

    return err(ReflectCallCheckerKeyOfRecordsError{
        .code = ReflectCallCheckerKeyOfRecordsErrorCode::NOT_A_KEY,
        .message = message,
    });
  }

  return ok(type);
}

ReflectCallChecker::ResultTargetObject
ReflectCallChecker::checkTargetObject(const Type *type) const {
  if (!type->isError() && !UnknownChecker::isObjectReference(type) && !type->isAnyObject()) {
    auto message = ErrorTextMetadataTarget::notObject(type->name());

    return err(ReflectCallCheckerTargetObjectError{
        .code = ReflectCallCheckerTargetObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

ReflectCallChecker::ResultArgumentList
ReflectCallChecker::checkArgumentList(const Type *type) const {
  if (type->isError() || type->isArray()) {
    return ok(type);
  }
  if (type->isTuple()) {
    auto message = ErrorTextReflectArgs::tuple();

    return err(ReflectCallCheckerArgumentListError{
        .code = ReflectCallCheckerArgumentListErrorCode::TUPLE,
        .message = message,
    });
  }
  auto message = ErrorTextReflectArgs::notArray(type->name());

  return err(ReflectCallCheckerArgumentListError{
      .code = ReflectCallCheckerArgumentListErrorCode::NOT_AN_ARRAY,
      .message = message,
  });
}

const Type *ReflectCallChecker::designType(const Expr *key) const {
  const auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal == nullptr) {
    return nullptr;
  }
  auto written = ToUtf8::toUtf8(literal->value);
  auto designKey = DesignKeyTable::find(written);
  if (!designKey.has_value()) {
    return nullptr;
  }
  const auto *any = Type::constructorOf(Type::anyObject());
  switch (designKey.value()) {
    case DesignKey::TYPE:
    case DesignKey::RETURN_TYPE:
      return Type::optional(any);
    case DesignKey::PARAM_TYPES:
      return Type::optional(Type::array(any));
  }

  return nullptr;
}

bool ReflectCallChecker::isDesignKey(const Expr *key) const {
  const auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal == nullptr) {
    return false;
  }
  auto written = ToUtf8::toUtf8(literal->value);

  return llvm::StringRef(written).starts_with(DesignKeyTable::getPrefix());
}

Expr *ReflectCallChecker::stripImplicit(Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

bool ReflectCallChecker::isReflectAccepting(const Type *arg, const Type *param) const {
  const Type *given = arg;
  const Type *wanted = param;
  while (wanted->isOptional()) {
    if (given->isUndefined()) {
      return true;
    }
    given = given->isOptional() ? given->inner() : given;
    wanted = wanted->inner();
  }

  return wanted->isUnknown() || this->isPlainAccepting(given, wanted);
}

bool ReflectCallChecker::isPlainAccepting(const Type *arg, const Type *param) const {
  if (arg->isOptional() || arg->isUnion() || param->isUnion()) {
    return false;
  }
  const Type *value = arg->isLiteral() ? arg->literalBase() : arg;
  switch (param->kind()) {
    case Type::Kind::Int:
      return value->isInteger() && param->holds(value);
    case Type::Kind::Double:
      return value->isInteger() || value->isFloat();
    case Type::Kind::Bool:
      return value->isBool();
    case Type::Kind::String:
      return value->isString();
    case Type::Kind::Array:
    case Type::Kind::Function:
    case Type::Kind::Decimal:
    case Type::Kind::Result:
    case Type::Kind::Json:
      return arg == param;
    case Type::Kind::ClassRef:
      return this->isClassRefAccepting(arg, param);
    case Type::Kind::Object:
      return this->isObjectAccepting(arg, param);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Optional:
    case Type::Kind::Param:
    case Type::Kind::Never:
    case Type::Kind::Iterator:
    case Type::Kind::Dict:
    case Type::Kind::Literal:
    case Type::Kind::Union:
    case Type::Kind::Promise:
    case Type::Kind::Unknown:
      return false;
  }

  return false;
}

bool ReflectCallChecker::isClassRefAccepting(const Type *arg, const Type *param) const {
  const ClassDecl *classDecl = arg->classRef();
  if (classDecl == nullptr || classDecl->type == nullptr) {
    return false;
  }
  if (param->classRef() != nullptr) {
    return classDecl == param->classRef();
  }
  const Type *made = param->constructed();

  return made->isAnyObject() ||
         (made->classDecl() != nullptr && classDecl->type->isSubtypeOf(made));
}

bool ReflectCallChecker::isObjectAccepting(const Type *arg, const Type *param) const {
  const ClassDecl *classDecl = arg->classDecl();
  if (classDecl == nullptr || classDecl->literal() != nullptr || classDecl->builtin() ||
      !arg->isClass()) {
    return param->isDate() && arg->isDate();
  }
  if (param->isAnyObject()) {
    return true;
  }

  return (param->isClass() || param->isBase() || !param->bases().empty()) &&
         arg->isSubtypeOf(param);
}

Expr *ReflectCallChecker::checkReflectCall(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  call->type = Type::error();
  auto function = ReflectFunctionTable::find(member->name);
  if (!function.has_value()) {
    return this->reportReflectName(call, member, ErrorTextReflectCall::unknown(member->name));
  }
  switch (*function) {
    case ReflectFunction::GET:
    case ReflectFunction::APPLY:
    case ReflectFunction::CONSTRUCT:
    case ReflectFunction::FILL:
      return this->reflectChecker->checkReflection(call, member, expected);
    case ReflectFunction::METADATA:
      return this->reportReflectName(call, member, ErrorTextReflectCall::decorator());
    case ReflectFunction::GET_METADATA_KEYS:
    case ReflectFunction::GET_OWN_METADATA_KEYS:
      return this->reportReflectName(call, member, ErrorTextReflectCall::keys(member->name));
    case ReflectFunction::DEFINE_METADATA:
      return this->checkMetadataCall(call, member, expected, {Builtin::MetaDefine, 3, 4});
    case ReflectFunction::GET_METADATA:
      return this->checkMetadataCall(call, member, expected, {Builtin::MetaGet, 2, 3});
    case ReflectFunction::GET_OWN_METADATA:
      return this->checkMetadataCall(call, member, expected, {Builtin::MetaGetOwn, 2, 3});
    case ReflectFunction::HAS_METADATA:
      return this->checkMetadataCall(call, member, expected, {Builtin::MetaHas, 2, 3});
    case ReflectFunction::HAS_OWN_METADATA:
      return this->checkMetadataCall(call, member, expected, {Builtin::MetaHasOwn, 2, 3});
    case ReflectFunction::DELETE_METADATA:
      return this->checkMetadataCall(call, member, expected, {Builtin::MetaDelete, 2, 3});
  }

  return call;
}

Expr *ReflectCallChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *ReflectCallChecker::reportReflectName(CallExpr *call, MemberExpr *member,
                                            const std::string &message) {
  this->state.getDiagnostics().error(member->loc(), message);

  return this->checkArgsLoosely(call);
}

Expr *ReflectCallChecker::checkMetadataCall(CallExpr *call, MemberExpr *member,
                                            const Type *expected, MetadataFunction function) {
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity =
      isRejected ? ok(call) : this->argumentChecker->checkArity(call, function.min, function.max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  bool isDefine = function.builtin == Builtin::MetaDefine;
  auto key = this->readMetadataKey(call, isDefine);
  if (key.failed) {
    return this->checkArgsLoosely(call);
  }
  if (isDefine) {
    this->walkMetadataValue(call, key);
  }
  this->walkMetadataTarget(call, isDefine ? 2 : 1);
  call->target = BuiltinCall{.builtin = function.builtin};
  call->type = this->metadataResultType(function.builtin, key, expected);

  return call;
}

ReflectCallChecker::MetadataKey ReflectCallChecker::readMetadataKey(CallExpr *call, bool define) {
  Expr *&keyArg = call->args[0];
  MetadataKey key{
      .loc = keyArg->loc(),
  };
  auto named = this->metadataChecker->metadataKey(keyArg);
  key.key = named.key;
  key.label = named.label;
  key.design = this->designType(keyArg);
  if (key.key != nullptr) {
    call->metaKey = key.key;
    keyArg = this->state.getAst().make<StringLitExpr>(key.loc, u"");
    keyArg->type = Type::string();
    return key;
  }
  keyArg = this->exprChecker->checkExpr(keyArg);
  auto records = this->checkKeyOfRecords(keyArg->type);
  if (records.isErr()) {
    this->state.getDiagnostics().error(key.loc, records.error().message);
    key.failed = true;
    return key;
  }
  if (define && this->isDesignKey(keyArg)) {
    auto message = ErrorTextMetadataKey::design();
    this->state.getDiagnostics().error(key.loc, message);
  }

  return key;
}

void ReflectCallChecker::walkMetadataValue(CallExpr *call, const MetadataKey &key) {
  Expr *&value = call->args[1];
  const MetadataType *known =
      key.key != nullptr ? this->state.getReflectStore().findMetadataType(key.key) : nullptr;
  value = this->exprChecker->checkExpr(value, known != nullptr ? known->type : nullptr);
  const Type *type = value->type;
  if (type->isVoid()) {
    auto message = ErrorTextMetadataValue::voidValue();
    this->state.getDiagnostics().error(value->loc(), message);
  } else if (known != nullptr && !type->isError()) {
    value = this->coerceKnownMetadata(value, known, key.label);
  } else if (key.key != nullptr && !type->isError() && !TypeQueries::mentionsParam(type)) {
    this->rememberMetadataType(value, key);
  }
  if (!value->type->isVoid()) {
    value = this->typeRelations->convert(value, Type::unknown());
  }
}

Expr *ReflectCallChecker::coerceKnownMetadata(Expr *value, const MetadataType *known,
                                              const std::string &label) {
  auto &diagnostics = this->state.getDiagnostics();
  auto records = ErrorTextMetadataValue::label(label);
  diagnostics.pushList(DiagnosticRoute::PARENT);
  auto *coerced = this->typeRelations->coerce(value, known->type, records);
  auto coerceDiagnostics = diagnostics.popList();
  bool isReachingProgram = diagnostics.getCurrentDestination() == DiagnosticRoute::PROGRAM;
  if (Diagnostics::hasErrors(coerceDiagnostics) && isReachingProgram) {
    auto message = ErrorTextMetadataValue::since(label, known->type->name());
    this->state.getDiagnostics().note(known->loc, message);
  }

  return coerced;
}

void ReflectCallChecker::rememberMetadataType(const Expr *value, const MetadataKey &key) {
  if (value->type->isUndefined()) {
    auto message = ErrorTextMetadataValue::undefined(key.label);
    this->state.getDiagnostics().error(value->loc(), message);
    return;
  }
  this->state.getReflectStore().rememberMetadataType(key.key, value->type, value->loc());
}

void ReflectCallChecker::walkMetadataTarget(CallExpr *call, size_t at) {
  Expr *&target = call->args[at];
  target = this->exprChecker->checkExpr(target);
  auto object = this->checkTargetObject(target->type);
  if (object.isErr()) {
    this->state.getDiagnostics().error(target->loc(), object.error().message);
  } else {
    target = this->typeRelations->convert(target, Type::anyObject());
  }
  if (call->args.size() == at + 1) {
    call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
  }
  call->args[at + 1] =
      this->argumentChecker->checkArg(call->args[at + 1], Type::optional(Type::string()));
}

const Type *ReflectCallChecker::metadataResultType(Builtin builtin, const MetadataKey &key,
                                                   const Type *expected) {
  if (builtin == Builtin::MetaDefine) {
    return Type::voidTy();
  }
  bool isReading = builtin == Builtin::MetaGet || builtin == Builtin::MetaGetOwn;
  if (!isReading) {
    return Type::boolTy();
  }
  if (key.key == nullptr) {
    return key.design != nullptr ? key.design : Type::unknown();
  }
  auto typed = this->metadataChecker->checkMetadataType(key.key, key.label, key.loc, expected);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(key.loc, typed.error().message);
  }
  const Type *records = typed.isOk() ? typed.value() : Type::error();

  return Type::optional(records);
}

std::optional<ReflectCallChecker::KnownMethod>
ReflectCallChecker::knownReflectMethod(ClassDecl *classDecl, llvm::StringRef name) {
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  auto isGeneric = std::bind_front(&ReflectCallChecker::isGenericMethod, this);
  llvm::erase_if(methods, isGeneric);
  if (methods.empty()) {
    return std::nullopt;
  }
  auto isCallable = std::bind_front(&ReflectCallChecker::isReflectCallable, this, classDecl);
  if (!llvm::all_of(methods, isCallable)) {
    return std::nullopt;
  }
  if (this->isDeclaredBelow(classDecl, name)) {
    return std::nullopt;
  }
  bool hasSeveral = methods.size() > 1;

  return KnownMethod{.method = methods.front(), .hasSeveral = hasSeveral};
}

bool ReflectCallChecker::isGenericMethod(const FunctionDecl *functionDecl) const {
  return !functionDecl->typeParams.empty();
}

bool ReflectCallChecker::isReflectCallable(ClassDecl *classDecl, FunctionDecl *functionDecl) const {
  bool isOpen = functionDecl->sym != nullptr && !functionDecl->isAbstract() &&
                functionDecl->access() == Access::Public &&
                this->classLinks->soleImplementation(classDecl, functionDecl) == functionDecl;
  if (!isOpen) {
    return false;
  }
  const Type *returnType = functionDecl->sym->getReturnType();

  return returnType != nullptr && !returnType->isError();
}

bool ReflectCallChecker::isDeclaredBelow(const ClassDecl *classDecl, llvm::StringRef name) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    if (this->hasOwnMethodNamed(child, name)) {
      return true;
    }
  }

  return false;
}

bool ReflectCallChecker::hasOwnMethodNamed(const ClassDecl *owner, llvm::StringRef name) const {
  for (const FunctionDecl *method : this->classLinks->ownMethods(owner)) {
    if (method->name == name) {
      return true;
    }
  }

  return false;
}

const FunctionDecl *ReflectCallChecker::heldReflectMethod(const Expr *expr) {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  const auto *name = dyn_cast<NameExpr>(bare);
  if (name != nullptr) {
    return name->var() != nullptr ? name->var()->reflectMethod() : nullptr;
  }
  const auto *got = dyn_cast<CallExpr>(bare);
  if (got == nullptr || got->builtin() != Builtin::ReflectMethod ||
      this->state.getExpressionMarks().hasSeveralVariants(got)) {
    return nullptr;
  }
  const auto *object = ExprParts::withoutImplicitCasts(got->args[0]);

  return isa<NameExpr>(object) ? got->method()->decl : nullptr;
}

Expr *ReflectCallChecker::directReflectApply(CallExpr *call, const FunctionDecl *functionDecl) {
  auto *self = this->stripImplicit(call->args[1]);
  const ClassDecl *classDecl = self->type->isOptional() ? nullptr : self->type->classDecl();
  if (classDecl == nullptr || classDecl->literal() != nullptr ||
      !classDecl->derivesFrom(functionDecl->cls())) {
    return nullptr;
  }
  auto *list = dyn_cast<ArrayLitExpr>(call->args[2]);
  if (list == nullptr) {
    return nullptr;
  }
  FuncSymbol *function = functionDecl->sym;
  auto args = std::vector<Expr *>();
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread || element.hole) {
      return nullptr;
    }
    auto *arg = this->stripImplicit(element.expr);
    args.push_back(arg);
  }
  size_t fixed = function->hasRest ? function->params.size() - 1 : function->params.size();
  if (args.size() < function->minArgs || (!function->hasRest && args.size() > fixed)) {
    return nullptr;
  }
  for (size_t index = 0; index < args.size(); ++index) {
    const Type *param = index < fixed ? function->params[index] : function->params.back()->inner();
    if (param == nullptr || !this->isReflectAccepting(args[index]->type, param)) {
      return nullptr;
    }
  }
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), self, functionDecl->name);
  member->type = this->returnInferrer->functionType(function);
  auto *direct = this->state.getAst().make<CallExpr>(call->loc(), member, std::move(args));
  direct->target = MethodCall{.method = function};
  direct->type = function->getReturnType();
  std::vector<bool> given(direct->args.size(), true);
  auto *result = this->argumentChecker->checkArguments(direct, function->params, function->minArgs,
                                                       function->hasRest, call->loc(), &given);
  if (!result->type->isVoid()) {
    return this->typeRelations->convert(result, Type::unknown());
  }
  auto *undefined = this->state.getAst().make<UndefinedExpr>(call->loc());
  auto *checked = this->exprChecker->checkExpr(undefined);
  auto *nothing = this->typeRelations->convert(checked, Type::unknown());
  auto *sequence =
      this->state.getAst().make<SequenceExpr>(call->loc(), std::vector<Expr *>{result, nothing});
  sequence->type = Type::unknown();

  return sequence;
}

Expr *ReflectCallChecker::checkReflectArgs(Expr *args) {
  if (isa<ArrayLitExpr>(args)) {
    return this->exprChecker->checkExpr(args, Type::array(Type::unknown()));
  }
  args = this->exprChecker->checkExpr(args);
  auto list = this->checkArgumentList(args->type);
  if (list.isOk()) {
    return args;
  }
  this->state.getDiagnostics().error(args->loc(), list.error().message);
  args->type = Type::error();

  return args;
}

}
