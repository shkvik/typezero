#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
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
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->typeRelations = &all.getTypeRelations();
  this->reflectChecker = &all.getReflectChecker();
  this->returnInferrer = &all.getReturnInferrer();
  this->argumentChecker = &all.getArgumentChecker();
  this->metadataChecker = &all.getMetadataChecker();
}

ReflectCallChecker::ResultKeyOfRecords ReflectCallChecker::keyOfRecords(const Type *type) {
  if (!type->isString() && !type->isError()) {
    std::string message = ErrorTextMetadataKey::type(type->name());

    return err(ReflectCallCheckerKeyOfRecordsError{
        .code = ReflectCallCheckerKeyOfRecordsErrorCode::NOT_A_KEY,
        .message = message,
    });
  }

  return ok(type);
}

ReflectCallChecker::ResultTargetObject ReflectCallChecker::targetObject(const Type *type) {
  if (!type->isError() && !UnknownChecker::isObjectReference(type) && !type->isAnyObject()) {
    std::string message = ErrorTextMetadataTarget::notObject(type->name());

    return err(ReflectCallCheckerTargetObjectError{
        .code = ReflectCallCheckerTargetObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

ReflectCallChecker::ResultArgumentList ReflectCallChecker::argumentList(const Type *type) {
  if (type->isError() || type->isArray()) {
    return ok(type);
  }
  if (type->isTuple()) {
    std::string message = ErrorTextReflectArgs::tuple();

    return err(ReflectCallCheckerArgumentListError{
        .code = ReflectCallCheckerArgumentListErrorCode::TUPLE,
        .message = message,
    });
  }
  std::string message = ErrorTextReflectArgs::notArray(type->name());

  return err(ReflectCallCheckerArgumentListError{
      .code = ReflectCallCheckerArgumentListErrorCode::NOT_AN_ARRAY,
      .message = message,
  });
}

const Type *ReflectCallChecker::designType(const Expr *key) {
  auto *lit = dyn_cast<StringLitExpr>(key);
  if (!lit) {
    return nullptr;
  }
  const Type *any = Type::constructorOf(Type::anyObject());
  if (lit->value == u"design:type" || lit->value == u"design:returntype") {
    return Type::optional(any);
  }
  if (lit->value == u"design:paramtypes") {
    return Type::optional(Type::array(any));
  }

  return nullptr;
}

bool ReflectCallChecker::isDesignKey(const Expr *key) {
  auto *lit = dyn_cast<StringLitExpr>(key);

  return lit && lit->value.rfind(u"design:", 0) == 0;
}

Expr *ReflectCallChecker::stripImplicit(Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool ReflectCallChecker::reflectAccepts(const Type *arg, const Type *param) {
  const Type *given = arg;
  const Type *wanted = param;
  while (wanted->isOptional()) {
    if (given->isUndefined()) {
      return true;
    }
    given = given->isOptional() ? given->inner() : given;
    wanted = wanted->inner();
  }

  return wanted->isUnknown() || ReflectCallChecker::acceptsPlain(given, wanted);
}

bool ReflectCallChecker::acceptsPlain(const Type *arg, const Type *param) {
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
  case Type::Kind::ClassRef: {
    const ClassDecl *cls = arg->classRef();
    if (!cls || !cls->type) {
      return false;
    }
    if (param->classRef()) {
      return cls == param->classRef();
    }
    const Type *made = param->constructed();
    return made->isAnyObject() || (made->classDecl() && cls->type->isSubtypeOf(made));
  }
  case Type::Kind::Object: {
    const ClassDecl *cls = arg->classDecl();
    if (!cls || cls->literal() || cls->builtin() || !arg->isClass()) {
      return param->isDate() && arg->isDate();
    }
    if (param->isAnyObject()) {
      return true;
    }
    return (param->isClass() || param->isBase() || !param->bases().empty()) &&
           arg->isSubtypeOf(param);
  }
  default:
    return false;
  }
}

Expr *ReflectCallChecker::checkReflectCall(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  struct Function {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned min, max;
  };
  static const Function functions[] = {
      {"defineMetadata", Builtin::MetaDefine, 3, 4}, {"getMetadata", Builtin::MetaGet, 2, 3},
      {"getOwnMetadata", Builtin::MetaGetOwn, 2, 3}, {"hasMetadata", Builtin::MetaHas, 2, 3},
      {"hasOwnMetadata", Builtin::MetaHasOwn, 2, 3}, {"deleteMetadata", Builtin::MetaDelete, 2, 3}};
  llvm::StringRef fn = member->name;
  if (fn == "get" || fn == "apply" || fn == "construct" || fn == "fill") {
    return this->reflectChecker->checkReflection(call, member, expected);
  }
  const Function *function =
      llvm::find_if(functions, [&](const Function &f) { return f.name == fn; });
  if (function == std::end(functions)) {
    std::string message = fn == "metadata" ? ErrorTextReflectCall::decorator()
                          : fn == "getMetadataKeys" || fn == "getOwnMetadataKeys"
                              ? ErrorTextReflectCall::keys(fn)
                              : ErrorTextReflectCall::unknown(fn);
    this->state.getDiagnostics().error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, function->min, function->max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  bool define = function->builtin == Builtin::MetaDefine;

  Expr *&keyArg = call->args[0];
  llvm::SMLoc keyLoc = keyArg->loc();
  std::string label;
  const void *key = this->metadataChecker->metadataKey(keyArg, label);
  const Type *design = ReflectCallChecker::designType(keyArg);
  if (key) {
    call->metaKey = key;
    keyArg = this->state.getAst().make<StringLitExpr>(keyLoc, u"");
    keyArg->type = Type::string();
  } else {
    keyArg = this->exprChecker->checkExpr(keyArg);
    ResultKeyOfRecords records = ReflectCallChecker::keyOfRecords(keyArg->type);
    if (records.isErr()) {
      this->state.getDiagnostics().error(keyLoc, records.error().message);
      checkArgsLoosely();
      return call;
    }
    if (define && ReflectCallChecker::isDesignKey(keyArg)) {
      std::string message = ErrorTextMetadataKey::design();
      this->state.getDiagnostics().error(keyLoc, message);
    }
  }

  if (define) {
    Expr *&value = call->args[1];
    const MetadataType *known = key ? this->state.getReflectStore().findMetadataType(key) : nullptr;
    value = this->exprChecker->checkExpr(value, known ? known->type : nullptr);
    const Type *type = value->type;
    if (type->isVoid()) {
      std::string message = ErrorTextMetadataValue::voidValue();
      this->state.getDiagnostics().error(value->loc(), message);
    } else if (known && !type->isError()) {
      unsigned before = this->state.getDiagnostics().errorCount();
      std::string records = ErrorTextMetadataValue::label(label);
      value = this->typeRelations->coerce(value, known->type, records);
      if (this->state.getDiagnostics().errorCount() > before) {
        std::string message = ErrorTextMetadataValue::since(label, known->type->name());
        this->state.getDiagnostics().note(known->loc, message);
      }
    } else if (key && !type->isError() && !TypeQueries::mentionsParam(type)) {
      if (type->isUndefined()) {
        std::string message = ErrorTextMetadataValue::undefined(label);
        this->state.getDiagnostics().error(value->loc(), message);
      } else {
        this->state.getReflectStore().rememberMetadataType(key, type, value->loc());
      }
    }
    if (!value->type->isVoid()) {
      value = this->typeRelations->convert(value, Type::unknown());
    }
  }

  size_t at = define ? 2 : 1;
  Expr *&target = call->args[at];
  target = this->exprChecker->checkExpr(target);
  const Type *targetType = target->type;
  ResultTargetObject object = ReflectCallChecker::targetObject(targetType);
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

  call->target = BuiltinCall{.builtin = function->builtin};
  switch (function->builtin) {
  case Builtin::MetaDefine:
    call->type = Type::voidTy();
    break;
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn: {
    if (!key) {
      call->type = design ? design : Type::unknown();
      break;
    }
    MetadataChecker::ResultMetadataTyped typed =
        this->metadataChecker->metadataType(key, label, keyLoc, expected);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(keyLoc, typed.error().message);
    }
    const Type *records = typed.isOk() ? typed.value() : Type::error();
    call->type = Type::optional(records);
    break;
  }
  default:
    call->type = Type::boolTy();
    break;
  }

  return call;
}

FunctionDecl *ReflectCallChecker::knownReflectMethod(ClassDecl *cls, llvm::StringRef name,
                                                     bool &several) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  llvm::erase_if(methods, [](const FunctionDecl *fn) { return !fn->typeParams.empty(); });
  if (methods.empty()) {
    return nullptr;
  }
  for (FunctionDecl *fn : methods) {
    if (!fn->sym || fn->isAbstract() || fn->access() != Access::Public ||
        ClassLinks::soleImplementation(cls, fn) != fn) {
      return nullptr;
    }
    const Type *ret = fn->sym->getReturnType();
    if (!ret || ret->isError()) {
      return nullptr;
    }
  }
  if (ReflectCallChecker::declaredBelow(cls, name)) {
    return nullptr;
  }
  several = methods.size() > 1;

  return methods.front();
}

bool ReflectCallChecker::declaredBelow(const ClassDecl *cls, llvm::StringRef name) {
  for (const ClassDecl *child : ClassTree::descendants(cls)) {
    for (const FunctionDecl *method : ClassLinks::ownMethods(child)) {
      if (method->name == name) {
        return true;
      }
    }
  }

  return false;
}

const FunctionDecl *ReflectCallChecker::heldReflectMethod(const Expr *f) {
  f = ReflectCallChecker::stripImplicit(const_cast<Expr *>(f));
  if (auto *name = dyn_cast<NameExpr>(f)) {
    return name->var() ? name->var()->reflectMethod() : nullptr;
  }
  if (auto *got = dyn_cast<CallExpr>(f);
      got && got->builtin() == Builtin::ReflectMethod &&
      !this->state.getExpressionMarks().hasSeveralVariants(got) &&
      isa<NameExpr>(ReflectCallChecker::stripImplicit(got->args[0]))) {
    return got->method()->decl;
  }

  return nullptr;
}

Expr *ReflectCallChecker::directReflectApply(CallExpr *call, const FunctionDecl *fn) {
  Expr *self = ReflectCallChecker::stripImplicit(call->args[1]);
  const ClassDecl *cls = self->type->isOptional() ? nullptr : self->type->classDecl();
  if (!cls || cls->literal() || !cls->derivesFrom(fn->cls())) {
    return nullptr;
  }
  auto *list = dyn_cast<ArrayLitExpr>(call->args[2]);
  if (!list) {
    return nullptr;
  }
  FuncSymbol *sym = fn->sym;
  this->returnInferrer->resolveParams(sym);
  std::vector<Expr *> args;
  for (const ArrayLitExpr::Element &e : list->elements) {
    if (e.spread || e.hole) {
      return nullptr;
    }
    args.push_back(ReflectCallChecker::stripImplicit(e.expr));
  }
  size_t fixed = sym->hasRest ? sym->params.size() - 1 : sym->params.size();
  if (args.size() < sym->minArgs || (!sym->hasRest && args.size() > fixed)) {
    return nullptr;
  }
  for (size_t i = 0; i < args.size(); ++i) {
    const Type *param = i < fixed ? sym->params[i] : sym->params.back()->inner();
    if (!param || !ReflectCallChecker::reflectAccepts(args[i]->type, param)) {
      return nullptr;
    }
  }
  auto *member = this->state.getAst().make<MemberExpr>(call->loc(), self, fn->name);
  member->type = this->returnInferrer->functionType(sym);
  auto *direct = this->state.getAst().make<CallExpr>(call->loc(), member, std::move(args));
  direct->target = MethodCall{.method = sym};
  direct->type = sym->getReturnType();
  std::vector<bool> given(direct->args.size(), true);
  Expr *result = this->argumentChecker->checkArguments(direct, sym->params, sym->minArgs,
                                                       sym->hasRest, call->loc(), &given);
  if (!result->type->isVoid()) {
    return this->typeRelations->convert(result, Type::unknown());
  }
  Expr *nothing = this->typeRelations->convert(
      this->exprChecker->checkExpr(this->state.getAst().make<UndefinedExpr>(call->loc())),
      Type::unknown());
  auto *seq =
      this->state.getAst().make<SequenceExpr>(call->loc(), std::vector<Expr *>{result, nothing});
  seq->type = Type::unknown();

  return seq;
}

Expr *ReflectCallChecker::checkReflectArgs(Expr *args) {
  if (isa<ArrayLitExpr>(args)) {
    return this->exprChecker->checkExpr(args, Type::array(Type::unknown()));
  }
  args = this->exprChecker->checkExpr(args);
  ResultArgumentList list = ReflectCallChecker::argumentList(args->type);
  if (list.isOk()) {
    return args;
  }
  this->state.getDiagnostics().error(args->loc(), list.error().message);
  args->type = Type::error();

  return args;
}

}
