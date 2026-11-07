#include "compiler/pipeline/check_program/topics/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/metadata_checker/metadata_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/reflect_checker/reflect_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

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
  if (param->isUnknown()) {
    return true;
  }
  if (param->isOptional()) {
    return arg->isUndefined() || ReflectCallChecker::reflectAccepts(
                                     arg->isOptional() ? arg->inner() : arg, param->inner());
  }
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

ReflectCallChecker::ReflectCallChecker(CheckState &owner)
    : state(owner) {}

Expr *ReflectCallChecker::checkReflectCall(CallExpr *call, MemberExpr *member,
                                           const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
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
    return ReflectChecker(this->state).checkReflection(call, member, expected);
  }
  const Function *function =
      llvm::find_if(functions, [&](const Function &f) { return f.name == fn; });
  if (function == std::end(functions)) {
    if (fn == "metadata") {
      this->state.getDiagnostics().error(
          member->loc(), "'Reflect.metadata(key, value)' makes a decorator; write it as "
                         "one, before a class or a member: '@Reflect.metadata(key, value)'");
    } else if (fn == "getMetadataKeys" || fn == "getOwnMetadataKeys") {
      this->state.getDiagnostics().error(
          member->loc(), "'Reflect." + fn +
                             "' is not supported: the keys of records are "
                             "decorators, classes and strings, which no one type holds; ask "
                             "for each key with 'Reflect.hasMetadata(key, target)'");
    } else {
      this->state.getDiagnostics().error(
          member->loc(), "'Reflect." + fn +
                             "' is not supported; Reflect has get, apply, "
                             "construct, fill, defineMetadata, getMetadata, getOwnMetadata, "
                             "hasMetadata, hasOwnMetadata and deleteMetadata");
    }
    checkArgsLoosely();
    return call;
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call)
               : ArgumentChecker(this->state).checkArity(call, function->min, function->max);
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
  const void *key = MetadataChecker(this->state).metadataKey(keyArg, label);
  const Type *design = ReflectCallChecker::designType(keyArg);
  if (key) {
    call->metaKey = key;
    keyArg = this->state.getAst().make<StringLitExpr>(keyLoc, u"");
    keyArg->type = Type::string();
  } else {
    keyArg = ExprChecker(this->state).checkExpr(keyArg);
    if (!keyArg->type->isString() && !keyArg->type->isError()) {
      this->state.getDiagnostics().error(
          keyLoc, "a key of metadata is a decorator, a class or a constant, whose records "
                  "have the type of what is written, or a string, whose records are "
                  "'unknown'; found '" +
                      keyArg->type->name() + "'");
      checkArgsLoosely();
      return call;
    }
    if (define && ReflectCallChecker::isDesignKey(keyArg)) {
      this->state.getDiagnostics().error(
          keyLoc, "the records 'design:*' are what the compiler writes for decorated "
                  "members; write a key of your own");
    }
  }

  if (define) {
    Expr *&value = call->args[1];
    const MetadataType *known = key ? this->state.getReflectStore().findMetadataType(key) : nullptr;
    value = ExprChecker(this->state).checkExpr(value, known ? known->type : nullptr);
    const Type *type = value->type;
    if (type->isVoid()) {
      this->state.getDiagnostics().error(value->loc(),
                                         "an expression of type 'void' has no value to record");
    } else if (known && !type->isError()) {
      unsigned before = this->state.getDiagnostics().errorCount();
      value =
          TypeRelations(this->state).coerce(value, known->type, "the records of '" + label + "'");
      if (this->state.getDiagnostics().errorCount() > before) {
        this->state.getDiagnostics().note(known->loc, "the records of '" + label + "' are '" +
                                                          known->type->name() +
                                                          "' since the first one, here");
      }
    } else if (key && !type->isError() && !mentionsParam(type)) {
      if (type->isUndefined()) {
        this->state.getDiagnostics().error(
            value->loc(), "cannot tell the type of the records of '" + label +
                              "' from 'undefined'; write a value of the type first");
      } else {
        this->state.getReflectStore().rememberMetadataType(key, type, value->loc());
      }
    }
    if (!value->type->isVoid()) {
      value = TypeRelations(this->state).convert(value, Type::unknown());
    }
  }

  size_t at = define ? 2 : 1;
  Expr *&target = call->args[at];
  target = ExprChecker(this->state).checkExpr(target);
  const Type *targetType = target->type;
  if (!targetType->isError() && !UnknownChecker::isObjectReference(targetType) &&
      !targetType->isAnyObject()) {
    this->state.getDiagnostics().error(
        target->loc(),
        "the target of metadata is an object, such as a class, found '" + targetType->name() + "'");
  } else {
    target = TypeRelations(this->state).convert(target, Type::anyObject());
  }
  if (call->args.size() == at + 1) {
    call->args.push_back(this->state.getAst().make<UndefinedExpr>(call->loc()));
  }
  call->args[at + 1] =
      ArgumentChecker(this->state).checkArg(call->args[at + 1], Type::optional(Type::string()));

  call->target = BuiltinCall{.builtin = function->builtin};
  switch (function->builtin) {
  case Builtin::MetaDefine:
    call->type = Type::voidTy();
    break;
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn:
    call->type = key      ? Type::optional(
                                MetadataChecker(this->state).metadataType(key, label, keyLoc, expected))
                 : design ? design
                          : Type::unknown();
    break;
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
    const Type *ret = ReturnInferrer(this->state).returnType(fn->sym);
    if (!ret || ret->isError()) {
      return nullptr;
    }
  }
  std::function<bool(const ClassDecl *)> declaresAgain = [&](const ClassDecl *c) {
    return llvm::any_of(c->children, [&](const ClassDecl *child) {
      return llvm::any_of(ClassLinks::ownMethods(child),
                          [&](const FunctionDecl *m) { return m->name == name; }) ||
             declaresAgain(child);
    });
  };
  if (declaresAgain(cls)) {
    return nullptr;
  }
  several = methods.size() > 1;

  return methods.front();
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
  ReturnInferrer(this->state).resolveParams(sym);
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
  member->type = ReturnInferrer(this->state).functionType(sym);
  auto *direct = this->state.getAst().make<CallExpr>(call->loc(), member, std::move(args));
  direct->target = MethodCall{.method = sym};
  direct->type = ReturnInferrer(this->state).returnType(sym);
  std::vector<bool> given(direct->args.size(), true);
  Expr *result =
      ArgumentChecker(this->state)
          .checkArguments(direct, sym->params, sym->minArgs, sym->hasRest, call->loc(), &given);
  if (!result->type->isVoid()) {
    return TypeRelations(this->state).convert(result, Type::unknown());
  }
  Expr *nothing =
      TypeRelations(this->state)
          .convert(ExprChecker(this->state)
                       .checkExpr(this->state.getAst().make<UndefinedExpr>(call->loc())),
                   Type::unknown());
  auto *seq =
      this->state.getAst().make<SequenceExpr>(call->loc(), std::vector<Expr *>{result, nothing});
  seq->type = Type::unknown();

  return seq;
}

Expr *ReflectCallChecker::checkReflectArgs(Expr *args) {
  if (isa<ArrayLitExpr>(args)) {
    return ExprChecker(this->state).checkExpr(args, Type::array(Type::unknown()));
  }
  args = ExprChecker(this->state).checkExpr(args);
  const Type *type = args->type;
  if (type->isError() || type->isArray()) {
    return args;
  }
  if (type->isTuple()) {
    this->state.getDiagnostics().error(
        args->loc(), "the arguments are an array, and this is a tuple; copy it into one, "
                     "'[...t]'");
  } else {
    this->state.getDiagnostics().error(
        args->loc(), "the arguments are an array, e.g. '[a, b]', found '" + type->name() + "'");
  }
  args->type = Type::error();

  return args;
}

}
