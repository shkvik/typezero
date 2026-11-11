#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

ReflectChecker::ReflectChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.getPrelude();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->classDeclarer = &all.getClassDeclarer();
  this->typeRelations = &all.getTypeRelations();
  this->argumentChecker = &all.getArgumentChecker();
  this->classValueChecker = &all.getClassValueChecker();
  this->classMemberChecker = &all.getClassMemberChecker();
  this->reflectCallChecker = &all.getReflectCallChecker();
}

void ReflectChecker::classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out) {
  TypeParts parts = TypeParts::collect(type, kReflectClassesRoute);
  for (const Type *part : parts.getTypes()) {
    auto *cls = const_cast<ClassDecl *>(part->classDecl());
    if (cls != nullptr && !cls->literal()) {
      out.insert(cls);
    }
  }
}

void ReflectChecker::constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out) {
  TypeParts parts = TypeParts::collect(type, kReflectConstructorsRoute);
  for (const Type *part : parts.getTypes()) {
    if (part->isConstructor()) {
      out.push_back(part->constructed());
    }
  }
}

bool ReflectChecker::instanceOf(const Type *made, const Type *of) {
  return made == of || of->isAnyObject() || made->isSubtypeOf(of);
}

Expr *ReflectChecker::stripImplicit(Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

bool ReflectChecker::onlyChecked() const {
  return this->state.getFunctionStack().isInsideTrial() ||
         llvm::any_of(this->state.getFunctionStack().getFunctions(), [](const FunctionDecl *fn) {
           return (!fn->typeParams.empty() && !fn->isInstance) ||
                  (fn->cls() && (fn->cls()->isGeneric() || fn->cls()->checkOnly));
         });
}

void ReflectChecker::noteReflectedConversion(const Type *from, const Type *to,
                                             const CastExpr *cast) {
  if (this->onlyChecked()) {
    return;
  }
  if (to->nonOptional()->isConstructor()) {
    if (ClassDecl *cls = const_cast<ClassDecl *>(from->nonOptional()->classRef())) {
      this->state.getReflectStore().rememberConstructorCast(cast, cls);
    }
    return;
  }
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown())) {
    llvm::SmallPtrSet<ClassDecl *, 8> escaped;
    ReflectChecker::classesIn(from, escaped);
    for (ClassDecl *cls : escaped) {
      this->state.getReflectStore().addEscapedClass(cls);
    }
  }
}

void ReflectChecker::noteConstructorFlow(const Type *from, const Type *to) {
  llvm::SmallVector<const Type *, 4> sources, targets;
  ReflectChecker::constructorsIn(to, targets);
  if (targets.empty()) {
    return;
  }
  ReflectChecker::constructorsIn(from, sources);
  for (const Type *x : sources) {
    for (const Type *y : targets) {
      if (x != y && ReflectChecker::instanceOf(x, y)) {
        this->state.getReflectStore().addConstructorWidening(x, y);
      } else if (x != y && ReflectChecker::instanceOf(y, x)) {
        this->state.getReflectStore().addConstructorWidening(y, x);
      }
    }
  }
}

void ReflectChecker::finishReflection() {
  ReflectStore &reflect = this->state.getReflectStore();
  for (const ConstructorCast &entry : reflect.getConstructorCasts()) {
    entry.cls->reflectNew = true;
  }
  if (reflect.isAnyObjectFilled()) {
    for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
      cls->reflectFill = !cls->builtin();
    }
  }
  if (!reflect.isReflectionUsed()) {
    for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
      cls->reflectNew = cls->reflectMethods = false;
    }
    return;
  }
  if (reflect.isAnyObjectRead()) {
    for (ClassDecl *cls : reflect.getEscapedClasses()) {
      ReflectChecker::markReflectMethods(cls);
    }
    for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
      if (cls->reflectNew) {
        ReflectChecker::markReflectMethods(cls);
      }
    }
  }
  for (ClassDecl *cls : reflect.getReflectedObjects()) {
    ReflectChecker::markReflectMethods(cls);
  }
  this->checkReflectNews();
}

void ReflectChecker::markReflectMethods(ClassDecl *cls) {
  std::vector<ClassDecl *> stack = {cls};
  while (!stack.empty()) {
    ClassDecl *next = stack.back();
    stack.pop_back();
    if (!next->reflectMethods) {
      next->reflectMethods = true;
      stack.insert(stack.end(), next->children.rbegin(), next->children.rend());
    }
  }
}

const Type *ReflectChecker::reflectError() {
  return this->prelude->builtinType("ReflectError");
}

Expr *ReflectChecker::okOf(Expr *value) {
  if (value->type->isError()) {
    return value;
  }
  auto *call = this->state.getAst().make<CallExpr>(
      value->loc(), this->state.getAst().make<NameExpr>(value->loc(), "ok"),
      std::vector<Expr *>{value});
  call->target = BuiltinCall{.builtin = Builtin::ResultOk};
  call->type = Type::result(value->type, this->reflectError());

  return call;
}

ReflectChecker::ResultConstructible
ReflectChecker::constructible(ClassDecl *cls, llvm::ArrayRef<const Type *> args) {
  if (cls->builtin()) {
    std::string message = ErrorTextConstructible::builtin();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::BUILTIN_CLASS,
        .message = message,
    });
  }
  if (cls->isAbstract) {
    std::string message = ErrorTextConstructible::abstract();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::ABSTRACT,
        .message = message,
    });
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty() && !args.empty()) {
    std::string message = ErrorTextConstructible::noArgs();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::NO_ARGUMENTS,
        .message = message,
    });
  }
  if (ctors.empty()) {
    return ok(cls);
  }
  size_t fitting = 0;
  std::vector<const FuncSymbol *> symbols;
  for (FunctionDecl *ctor : ctors) {
    FuncSymbol *sym = ctor->sym;
    if (!sym) {
      std::string message = ErrorTextConstructible::unknown();

      return err(ReflectCheckerConstructibleError{
          .code = ReflectCheckerConstructibleErrorCode::UNKNOWN_CONSTRUCTOR,
          .message = message,
      });
    }
    symbols.push_back(sym);
    size_t fixed = sym->hasRest ? sym->params.size() - 1 : sym->params.size();
    bool fits = args.size() >= sym->minArgs && (sym->hasRest || args.size() <= fixed);
    for (size_t i = 0; i < args.size() && fits; ++i) {
      const Type *param = i < fixed ? sym->params[i] : sym->params.back()->inner();
      fits = param && ReflectCallChecker::reflectAccepts(args[i], param);
    }
    fitting += fits;
  }
  if (fitting == 0) {
    std::string message = ErrorTextConstructible::takes(symbols);

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::NO_FIT,
        .message = message,
    });
  }
  if (fitting > 1) {
    std::string message = ErrorTextConstructible::several();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::SEVERAL_FIT,
        .message = message,
    });
  }

  return ok(cls);
}

ReflectChecker::ResultFillableClass ReflectChecker::fillableClass(ClassDecl *made) {
  if (!made->type || made->builtin()) {
    std::string why = ErrorTextReflectFill::javascript();
    std::string message = ErrorTextReflectFill::cannot(made->name, why);

    return err(ReflectCheckerFillableClassError{
        .code = ReflectCheckerFillableClassErrorCode::JAVASCRIPT_CLASS,
        .message = message,
    });
  }
  if (made->isAbstract) {
    std::string why = ErrorTextReflectFill::abstract();
    std::string message = ErrorTextReflectFill::cannot(made->name, why);

    return err(ReflectCheckerFillableClassError{
        .code = ReflectCheckerFillableClassErrorCode::ABSTRACT,
        .message = message,
    });
  }
  if (!ClassLinks::constructorsOf(made).empty()) {
    std::string why = ErrorTextReflectFill::ownCtor(made->name);
    std::string message = ErrorTextReflectFill::cannot(made->name, why);

    return err(ReflectCheckerFillableClassError{
        .code = ReflectCheckerFillableClassErrorCode::HAS_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(made);
}

void ReflectChecker::checkReflectNews() {
  ReflectStore &reflect = this->state.getReflectStore();
  if (!reflect.hasReflectedNews()) {
    return;
  }
  llvm::DenseMap<const Type *, llvm::SmallPtrSet<const Type *, 8>> reach;
  auto reachable = [&](const Type *start) -> const llvm::SmallPtrSet<const Type *, 8> & {
    auto [it, fresh] = reach.try_emplace(start);
    if (fresh) {
      std::vector<const Type *> work{start};
      it->second.insert(start);
      while (!work.empty()) {
        const Type *at = work.back();
        work.pop_back();
        for (const Type *to : reflect.getConstructorWidenings(at)) {
          if (it->second.insert(to).second) {
            work.push_back(to);
          }
        }
      }
    }
    return it->second;
  };
  std::vector<std::pair<ClassDecl *, const Type *>> entries;
  for (const ConstructorCast &entry : reflect.getConstructorCasts()) {
    const Type *target = entry.cast->type ? entry.cast->type->nonOptional() : nullptr;
    if (target && target->isConstructor()) {
      entries.push_back({entry.cls, target->constructed()});
    }
  }
  for (const ReflectedNew &reflected : reflect.getReflectedNews()) {
    const NewExpr *expr = reflected.expr;
    const std::vector<const Type *> &args = reflected.args;
    const Type *made = expr->type;
    if (!made || made->isError()) {
      continue;
    }
    llvm::SmallPtrSet<ClassDecl *, 8> checked;
    for (const auto &[cls, entry] : entries) {
      if (!cls->type || !reachable(entry).contains(made) || !checked.insert(cls).second) {
        continue;
      }
      ResultConstructible constructed = this->constructible(cls, args);
      if (constructed.isErr()) {
        std::string message =
            ErrorTextReflectNew::mayFail(cls->type->name(), constructed.error().message);
        this->state.getDiagnostics().error(expr->loc(), message);
        break;
      }
    }
  }
}

Expr *ReflectChecker::checkReflection(CallExpr *call, MemberExpr *member, const Type *expected) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  std::optional<ReflectFunction> function = ReflectFunctionTable::find(member->name);
  if (function == ReflectFunction::GET) {
    return this->checkReflectGet(call, member);
  }
  if (function == ReflectFunction::FILL) {
    return this->checkReflectFill(call, member);
  }
  if (function == ReflectFunction::APPLY) {
    return this->checkReflectApply(call, member);
  }

  return this->checkReflectConstruct(call, member, expected);
}

void ReflectChecker::walkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
}

bool ReflectChecker::hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max) {
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }

  return arity.isErr();
}

Expr *ReflectChecker::checkReflectGet(CallExpr *call, MemberExpr *member) {
  if (this->hasWrongArity(call, member, 2, 2)) {
    return call;
  }
  auto *object = dyn_cast<NameExpr>(call->args[0]);
  auto *key = dyn_cast<StringLitExpr>(call->args[1]);
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Expr *target = call->args[0];
  const Type *type = target->type;
  if (type->isError()) {
    call->args[1] = this->exprChecker->checkExpr(call->args[1]);
    return call;
  }
  const Type *base = GenericChecker::apparentType(type->nonOptional());
  if (base->isJson()) {
    std::string message = ErrorTextReflectGet::json();
    this->state.getDiagnostics().error(target->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (!type->isUnknown() && !UnknownChecker::isObjectReference(base) && !base->isAnyObject()) {
    std::string message = ErrorTextReflectGet::notObject(type->name());
    this->state.getDiagnostics().error(target->loc(), message);
    call->args[1] = this->exprChecker->checkExpr(call->args[1]);
    return call;
  }
  ReflectGet get{
      .object = object,
      .key = key,
      .type = type,
      .base = base,
  };
  Expr *known = key != nullptr ? this->checkKnownGet(call, get) : nullptr;
  if (known != nullptr) {
    return known;
  }

  return this->checkDynamicGet(call, get);
}

std::optional<std::string> ReflectChecker::asciiName(const std::u16string &text) {
  std::string name;
  for (char16_t c : text) {
    if (c >= 128) {
      return std::nullopt;
    }
    name.push_back(char(c));
  }

  return name;
}

Expr *ReflectChecker::checkKnownGet(CallExpr *call, const ReflectGet &get) {
  Expr *seen = ReflectChecker::stripImplicit(call->args[0]);
  const Type *seenType = seen->type;
  ClassDecl *cls =
      seenType->isObject() && !seenType->isAnyObject() ? ClassDeclarer::classOf(seenType) : nullptr;
  std::optional<std::string> ascii = ReflectChecker::asciiName(get.key->value);
  if (seenType->isOptional() || !ascii.has_value()) {
    return nullptr;
  }
  const std::string &name = *ascii;
  if (cls != nullptr) {
    this->classDeclarer->completeClass(cls);
  }
  const ClassField *declared = cls != nullptr ? ClassBases::findField(cls, name) : nullptr;
  bool open = cls == nullptr || (declared != nullptr && declared->access == Access::Public);
  const Type *base = get.base;
  bool named = get.object != nullptr && !get.type->isOptional() && base->isObject() &&
               !base->isAnyObject() && base->field(name) != nullptr && open;
  if (named) {
    return this->readNamedField(get.object, get.key, name);
  }
  bool classField = cls != nullptr && !cls->literal() && seenType->field(name) != nullptr && open;
  if (classField) {
    return this->readClassField(seen, get.key, name);
  }
  if (cls == nullptr || cls->literal() || seenType->field(name) != nullptr) {
    return nullptr;
  }

  return this->checkReflectMethodGet(call, seen, cls, name);
}

Expr *ReflectChecker::readNamedField(const NameExpr *object, const StringLitExpr *key,
                                     const std::string &name) {
  auto *root = this->state.getAst().make<NameExpr>(object->loc(), object->name);
  auto *read =
      this->state.getAst().make<MemberExpr>(key->loc(), root, this->state.getAst().keepName(name));
  Expr *checked = this->exprChecker->checkExpr(read);

  return this->typeRelations->convert(checked, Type::unknown());
}

Expr *ReflectChecker::readClassField(Expr *seen, const StringLitExpr *key,
                                     const std::string &name) {
  auto *read =
      this->state.getAst().make<MemberExpr>(key->loc(), seen, this->state.getAst().keepName(name));
  Expr *checked = this->classMemberChecker->checkClassMember(read, nullptr);

  return this->typeRelations->convert(checked, Type::unknown());
}

Expr *ReflectChecker::checkReflectMethodGet(CallExpr *call, Expr *seen, ClassDecl *cls,
                                            const std::string &name) {
  bool several = false;
  FunctionDecl *method = this->reflectCallChecker->knownReflectMethod(cls, name, several);
  if (method == nullptr) {
    return nullptr;
  }
  call->args.resize(1);
  call->args[0] = seen;
  call->target = BuiltinCall{.builtin = Builtin::ReflectMethod, .method = method->sym};
  call->type = Type::unknown();
  if (several) {
    this->state.getExpressionMarks().markSeveralVariants(call);
  }

  return call;
}

Expr *ReflectChecker::checkDynamicGet(CallExpr *call, const ReflectGet &get) {
  call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::string());
  this->noteReflectedRead(get.type, get.base);
  call->args[0] = this->typeRelations->convert(call->args[0], Type::unknown());
  if (!this->onlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectGet};
  call->type = Type::unknown();

  return call;
}

void ReflectChecker::noteReflectedRead(const Type *type, const Type *base) {
  if (this->onlyChecked()) {
    return;
  }
  if (type->isUnknown() || base->isAnyObject() || base->classDecl() == nullptr) {
    this->state.getReflectStore().markAnyObjectRead();
    return;
  }
  llvm::SmallPtrSet<ClassDecl *, 8> reflected;
  ReflectChecker::classesIn(base, reflected);
  for (ClassDecl *reflectedClass : reflected) {
    this->state.getReflectStore().addReflectedObject(reflectedClass);
  }
}

Expr *ReflectChecker::checkReflectFill(CallExpr *call, MemberExpr *member) {
  if (this->hasWrongArity(call, member, 2, 2)) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  call->args[1] = this->argumentChecker->checkArg(
      call->args[1], Type::dict(Type::DictKind::Map, Type::string(), Type::unknown()));
  const Expr *cls = call->args[0];
  const Type *type = cls->type;
  if (type->isError() || call->args[1]->type->isError()) {
    return call;
  }
  if (!type->isClassRef()) {
    std::string message = ErrorTextReflectFill::notClass(type->name());
    this->state.getDiagnostics().error(cls->loc(), message);
    return call;
  }
  std::optional<const Type *> fails = this->fillFailure(cls, type);
  if (!fails.has_value()) {
    return call;
  }
  this->flowChecker->calledFunction();
  call->target = BuiltinCall{.builtin = Builtin::ReflectFill};
  call->type = Type::result(ClassValueChecker::constructedBy(type), *fails);

  return call;
}

std::optional<const Type *> ReflectChecker::fillFailure(const Expr *cls, const Type *type) {
  const Type *fails = this->prelude->builtinType("ReflectFillError");
  auto *made = const_cast<ClassDecl *>(type->classRef());
  if (made == nullptr) {
    if (!this->onlyChecked()) {
      this->state.getReflectStore().markAnyObjectFilled();
      this->state.getReflectStore().markReflectionUsed();
    }
    return fails;
  }
  ResultFillableClass fillable = ReflectChecker::fillableClass(made);
  if (fillable.isErr()) {
    this->state.getDiagnostics().error(cls->loc(), fillable.error().message);
    return std::nullopt;
  }
  made->reflectFill = true;

  return this->prelude->builtinType("ReflectFillValueError");
}

Expr *ReflectChecker::checkReflectApply(CallExpr *call, MemberExpr *member) {
  if (this->hasWrongArity(call, member, 3, 3)) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const FunctionDecl *held = this->reflectCallChecker->heldReflectMethod(call->args[0]);
  const Type *gives = ReflectChecker::appliedResult(call->args[0]->type);
  this->walkAppliedFunction(call);
  this->walkAppliedThis(call);
  call->args[2] = this->reflectCallChecker->checkReflectArgs(call->args[2]);
  this->flowChecker->calledFunction();
  bool clean = held != nullptr && gives->isUnknown() && !call->args[0]->type->isError() &&
               !call->args[1]->type->isError() && !call->args[2]->type->isError();
  Expr *direct = clean ? this->reflectCallChecker->directReflectApply(call, held) : nullptr;
  if (direct != nullptr) {
    return this->okOf(direct);
  }
  if (!this->onlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
    this->state.getReflectStore().markAnyObjectRead();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectApply};
  call->type = Type::result(gives, this->reflectError());

  return call;
}

const Type *ReflectChecker::appliedResult(const Type *type) {
  bool returns = type->isFunction() && type->ret() != nullptr && !type->ret()->isError();

  return returns ? type->ret() : Type::unknown();
}

void ReflectChecker::walkAppliedFunction(CallExpr *call) {
  const Type *type = call->args[0]->type;
  if (!type->isError() && !type->isUnknown() && !type->isFunction()) {
    std::string message = ErrorTextReflectApply::notFunc(type->name());
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return;
  }
  call->args[0] = this->typeRelations->convert(call->args[0], Type::unknown());
}

void ReflectChecker::walkAppliedThis(CallExpr *call) {
  call->args[1] = this->exprChecker->checkExpr(call->args[1]);
  if (call->args[1]->type->isVoid()) {
    std::string message = ErrorTextReflectApply::voidThis();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return;
  }
  call->args[1] = this->typeRelations->convert(call->args[1], Type::unknown());
}

Expr *ReflectChecker::checkReflectConstruct(CallExpr *call, MemberExpr *member,
                                            const Type *expected) {
  if (call->args.size() == 3) {
    std::string message = ErrorTextReflectNew::third();
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (this->hasWrongArity(call, member, 2, 2)) {
    return call;
  }
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
    return call;
  }
  if (!type->isClassRef()) {
    std::string message = ErrorTextReflectNew::notClass(type->name());
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
    return call;
  }
  bool literal = name != nullptr && list != nullptr && type->classRef() != nullptr &&
                 !ReflectChecker::hasSpread(list);
  if (literal) {
    return this->checkLiteralConstruct(call, name, list, expected);
  }

  return this->checkDynamicConstruct(call, type);
}

bool ReflectChecker::hasSpread(const ArrayLitExpr *list) {
  for (const ArrayLitExpr::Element &e : list->elements) {
    if (e.spread) {
      return true;
    }
  }

  return false;
}

Expr *ReflectChecker::checkLiteralConstruct(CallExpr *call, const NameExpr *name,
                                            const ArrayLitExpr *list, const Type *expected) {
  std::vector<Expr *> args;
  for (const ArrayLitExpr::Element &e : list->elements) {
    args.push_back(e.expr);
  }
  auto *create =
      this->state.getAst().make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
  create->callee = this->state.getAst().make<NameExpr>(name->loc(), name->name);
  const Type *wanted = expected != nullptr && expected->nonOptional()->isResult()
                           ? expected->nonOptional()->okType()
                           : nullptr;
  Expr *made = this->classValueChecker->checkValueNew(create, wanted);

  return this->okOf(made);
}

Expr *ReflectChecker::checkDynamicConstruct(CallExpr *call, const Type *type) {
  bool known = false;
  const ClassDecl *made = type->classRef();
  if (made != nullptr) {
    if (made->type == nullptr) {
      std::string message = ErrorTextReflectNew::javascript(made->name);
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
      call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
      return call;
    }
    known = !made->isAbstract;
    if (!known) {
      call->args[0] = this->typeRelations->convert(call->args[0], Type::constructorOf(made->type));
    }
  }
  call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
  this->flowChecker->calledFunction();
  if (!known && !this->onlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectConstruct};
  const Type *constructed = ClassValueChecker::constructedBy(call->args[0]->type);
  call->type = Type::result(constructed, this->reflectError());

  return call;
}
}
