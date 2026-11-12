#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
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
  this->prelude = &all.prelude;
  this->classLinks = &all.classLinks;
  this->classBases = &all.classBases;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->argumentChecker = &all.argumentChecker;
  this->classValueChecker = &all.classValueChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->reflectCallChecker = &all.reflectCallChecker;
}

void ReflectChecker::classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out) const {
  auto parts = TypeParts::collect(type, kReflectClassesRoute);
  for (const Type *part : parts.getTypes()) {
    auto *classDecl = const_cast<ClassDecl *>(part->classDecl());
    if (classDecl != nullptr && classDecl->literal() == nullptr) {
      out.insert(classDecl);
    }
  }
}

void ReflectChecker::constructorsIn(const Type *type,
                                    llvm::SmallVectorImpl<const Type *> &out) const {
  auto parts = TypeParts::collect(type, kReflectConstructorsRoute);
  for (const Type *part : parts.getTypes()) {
    if (part->isConstructor()) {
      out.push_back(part->constructed());
    }
  }
}

bool ReflectChecker::isInstanceOf(const Type *made, const Type *of) const {
  return made == of || of->isAnyObject() || made->isSubtypeOf(of);
}

Expr *ReflectChecker::stripImplicit(Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

bool ReflectChecker::isOnlyChecked() const {
  auto isCheckOnly = std::bind_front(&ReflectChecker::isCheckOnlyFunction, this);

  return this->state.getFunctionStack().isInsideTrial() ||
         llvm::any_of(this->state.getFunctionStack().getFunctions(), isCheckOnly);
}

bool ReflectChecker::isCheckOnlyFunction(const FunctionDecl *functionDecl) const {
  const ClassDecl *owner = functionDecl->cls();

  return (!functionDecl->typeParams.empty() && !functionDecl->isInstance) ||
         (owner != nullptr && (owner->isGeneric() || owner->checkOnly));
}

void ReflectChecker::noteReflectedConversion(const Type *from, const Type *to,
                                             const CastExpr *castExpr) {
  if (this->isOnlyChecked()) {
    return;
  }
  if (to->nonOptional()->isConstructor()) {
    auto *classDecl = const_cast<ClassDecl *>(from->nonOptional()->classRef());
    if (classDecl != nullptr) {
      this->state.getReflectStore().rememberConstructorCast(castExpr, classDecl);
    }
    return;
  }
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown())) {
    auto escaped = llvm::SmallPtrSet<ClassDecl *, 8>();
    this->classesIn(from, escaped);
    for (ClassDecl *classDecl : escaped) {
      this->state.getReflectStore().addEscapedClass(classDecl);
    }
  }
}

void ReflectChecker::noteConstructorFlow(const Type *from, const Type *to) {
  auto targets = llvm::SmallVector<const Type *, 4>();
  this->constructorsIn(to, targets);
  if (targets.empty()) {
    return;
  }
  auto sources = llvm::SmallVector<const Type *, 4>();
  this->constructorsIn(from, sources);
  for (const Type *source : sources) {
    for (const Type *target : targets) {
      this->noteWidening(source, target);
    }
  }
}

void ReflectChecker::noteWidening(const Type *source, const Type *target) {
  if (source != target && this->isInstanceOf(source, target)) {
    this->state.getReflectStore().addConstructorWidening(source, target);
  } else if (source != target && this->isInstanceOf(target, source)) {
    this->state.getReflectStore().addConstructorWidening(target, source);
  }
}

void ReflectChecker::finishReflection() {
  auto &reflect = this->state.getReflectStore();
  for (const ConstructorCast &entry : reflect.getConstructorCasts()) {
    entry.cls->reflectNew = true;
  }
  if (reflect.isAnyObjectFilled()) {
    for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
      classDecl->reflectFill = !classDecl->builtin();
    }
  }
  if (!reflect.isReflectionUsed()) {
    for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
      classDecl->reflectNew = classDecl->reflectMethods = false;
    }
    return;
  }
  if (reflect.isAnyObjectRead()) {
    for (ClassDecl *classDecl : reflect.getEscapedClasses()) {
      this->markReflectMethods(classDecl);
    }
    for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
      if (classDecl->reflectNew) {
        this->markReflectMethods(classDecl);
      }
    }
  }
  for (ClassDecl *classDecl : reflect.getReflectedObjects()) {
    this->markReflectMethods(classDecl);
  }
  this->walkReflectNews();
}

void ReflectChecker::markReflectMethods(ClassDecl *classDecl) const {
  std::vector<ClassDecl *> stack = {classDecl};
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
  return this->prelude->builtinType(PreludeType::REFLECT_ERROR);
}

Expr *ReflectChecker::okOf(Expr *value) {
  if (value->type->isError()) {
    return value;
  }
  auto *call = this->state.getAst().make<CallExpr>(
      value->loc(),
      this->state.getAst().make<NameExpr>(value->loc(), GlobalNameTable::getName(GlobalName::OK)),
      std::vector<Expr *>{value});
  call->target = BuiltinCall{.builtin = Builtin::ResultOk};
  call->type = Type::result(value->type, this->reflectError());

  return call;
}

ReflectChecker::ResultConstructible
ReflectChecker::checkConstructible(ClassDecl *classDecl, llvm::ArrayRef<const Type *> args) {
  if (classDecl->builtin()) {
    auto message = ErrorTextConstructible::builtin();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::BUILTIN_CLASS,
        .message = message,
    });
  }
  if (classDecl->isAbstract) {
    auto message = ErrorTextConstructible::abstract();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::ABSTRACT,
        .message = message,
    });
  }
  auto constructors = this->classLinks->constructorsOf(classDecl);
  if (constructors.empty() && !args.empty()) {
    auto message = ErrorTextConstructible::noArgs();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::NO_ARGUMENTS,
        .message = message,
    });
  }
  if (constructors.empty()) {
    return ok(classDecl);
  }
  size_t fitting = 0;
  auto symbols = std::vector<const FuncSymbol *>();
  for (FunctionDecl *constructor : constructors) {
    FuncSymbol *function = constructor->sym;
    if (function == nullptr) {
      auto message = ErrorTextConstructible::unknown();

      return err(ReflectCheckerConstructibleError{
          .code = ReflectCheckerConstructibleErrorCode::UNKNOWN_CONSTRUCTOR,
          .message = message,
      });
    }
    symbols.push_back(function);
    size_t fixed = function->hasRest ? function->params.size() - 1 : function->params.size();
    bool isFitting =
        args.size() >= function->minArgs && (function->hasRest || args.size() <= fixed);
    for (size_t index = 0; index < args.size() && isFitting; ++index) {
      const Type *param =
          index < fixed ? function->params[index] : function->params.back()->inner();
      isFitting =
          param != nullptr && this->reflectCallChecker->isReflectAccepting(args[index], param);
    }
    fitting += isFitting;
  }
  if (fitting == 0) {
    auto message = ErrorTextConstructible::takes(symbols);

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::NO_FIT,
        .message = message,
    });
  }
  if (fitting > 1) {
    auto message = ErrorTextConstructible::several();

    return err(ReflectCheckerConstructibleError{
        .code = ReflectCheckerConstructibleErrorCode::SEVERAL_FIT,
        .message = message,
    });
  }

  return ok(classDecl);
}

ReflectChecker::ResultFillableClass ReflectChecker::checkFillableClass(ClassDecl *made) const {
  if (made->type == nullptr || made->builtin()) {
    auto why = ErrorTextReflectFill::javascript();
    auto message = ErrorTextReflectFill::cannot(made->name, why);

    return err(ReflectCheckerFillableClassError{
        .code = ReflectCheckerFillableClassErrorCode::JAVASCRIPT_CLASS,
        .message = message,
    });
  }
  if (made->isAbstract) {
    auto why = ErrorTextReflectFill::abstract();
    auto message = ErrorTextReflectFill::cannot(made->name, why);

    return err(ReflectCheckerFillableClassError{
        .code = ReflectCheckerFillableClassErrorCode::ABSTRACT,
        .message = message,
    });
  }
  if (!this->classLinks->constructorsOf(made).empty()) {
    auto why = ErrorTextReflectFill::ownCtor(made->name);
    auto message = ErrorTextReflectFill::cannot(made->name, why);

    return err(ReflectCheckerFillableClassError{
        .code = ReflectCheckerFillableClassErrorCode::HAS_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(made);
}

void ReflectChecker::walkReflectNews() {
  auto &reflect = this->state.getReflectStore();
  if (!reflect.hasReflectedNews()) {
    return;
  }
  auto reach = ReachMap();
  auto entries = this->constructorEntries();
  for (const ReflectedNew &reflected : reflect.getReflectedNews()) {
    this->walkReflectedNew(reflected, entries, reach);
  }
}

std::vector<ReflectChecker::ConstructorEntry> ReflectChecker::constructorEntries() const {
  auto entries = std::vector<ConstructorEntry>();
  for (const ConstructorCast &entry : this->state.getReflectStore().getConstructorCasts()) {
    const Type *target = entry.cast->type != nullptr ? entry.cast->type->nonOptional() : nullptr;
    if (target != nullptr && target->isConstructor()) {
      entries.push_back({.classDecl = entry.cls, .constructed = target->constructed()});
    }
  }

  return entries;
}

void ReflectChecker::walkReflectedNew(const ReflectedNew &reflected,
                                      llvm::ArrayRef<ConstructorEntry> entries, ReachMap &reach) {
  const Type *made = reflected.expr->type;
  if (made == nullptr || made->isError()) {
    return;
  }
  auto classes = this->reachingClasses(made, entries, reach);
  for (ClassDecl *classDecl : classes) {
    auto constructed = this->checkConstructible(classDecl, reflected.args);
    if (constructed.isErr()) {
      auto message =
          ErrorTextReflectNew::mayFail(classDecl->type->name(), constructed.error().message);
      this->state.getDiagnostics().error(reflected.expr->loc(), message);
      return;
    }
  }
}

std::vector<ClassDecl *> ReflectChecker::reachingClasses(const Type *made,
                                                         llvm::ArrayRef<ConstructorEntry> entries,
                                                         ReachMap &reach) const {
  auto classes = std::vector<ClassDecl *>();
  for (const ConstructorEntry &entry : entries) {
    bool isReaching = entry.classDecl->type != nullptr &&
                      this->reachableFrom(entry.constructed, reach).contains(made);
    if (isReaching && !llvm::is_contained(classes, entry.classDecl)) {
      classes.push_back(entry.classDecl);
    }
  }

  return classes;
}

const ReflectChecker::TypeSet &ReflectChecker::reachableFrom(const Type *start,
                                                             ReachMap &reach) const {
  auto [found, isFresh] = reach.try_emplace(start);
  if (isFresh) {
    this->fillReachable(start, found->second);
  }

  return found->second;
}

void ReflectChecker::fillReachable(const Type *start, TypeSet &reached) const {
  auto &reflect = this->state.getReflectStore();
  auto work = std::vector<const Type *>{start};
  reached.insert(start);
  while (!work.empty()) {
    const Type *current = work.back();
    work.pop_back();
    for (const Type *next : reflect.getConstructorWidenings(current)) {
      bool isInserted = reached.insert(next).second;
      if (isInserted) {
        work.push_back(next);
      }
    }
  }
}

Expr *ReflectChecker::checkReflection(CallExpr *call, MemberExpr *member, const Type *expected) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto function = ReflectFunctionTable::find(member->name);
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
  auto arity = this->argumentChecker->checkArity(call, min, max);
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
  const auto *base = this->genericChecker->apparentType(type->nonOptional());
  if (base->isJson()) {
    auto message = ErrorTextReflectGet::json();
    this->state.getDiagnostics().error(target->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (!type->isUnknown() && !UnknownChecker::isObjectReference(base) && !base->isAnyObject()) {
    auto message = ErrorTextReflectGet::notObject(type->name());
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

std::optional<std::string> ReflectChecker::asciiName(const std::u16string &text) const {
  auto name = std::string();
  for (char16_t unit : text) {
    if (unit >= 128) {
      return std::nullopt;
    }
    name.push_back(char(unit));
  }

  return name;
}

Expr *ReflectChecker::checkKnownGet(CallExpr *call, const ReflectGet &get) {
  auto *seen = this->stripImplicit(call->args[0]);
  const Type *seenType = seen->type;
  ClassDecl *classDecl = seenType->isObject() && !seenType->isAnyObject()
                             ? this->classDeclarer->classOf(seenType)
                             : nullptr;
  auto ascii = this->asciiName(get.key->value);
  if (seenType->isOptional() || !ascii.has_value()) {
    return nullptr;
  }
  const std::string &name = *ascii;
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }
  const ClassField *declared =
      classDecl != nullptr ? this->classBases->findField(classDecl, name) : nullptr;
  bool isOpen = classDecl == nullptr || (declared != nullptr && declared->access == Access::Public);
  const Type *base = get.base;
  bool isNamed = get.object != nullptr && !get.type->isOptional() && base->isObject() &&
                 !base->isAnyObject() && base->field(name) != nullptr && isOpen;
  if (isNamed) {
    return this->readNamedField(get.object, get.key, name);
  }
  bool isClassField = classDecl != nullptr && classDecl->literal() == nullptr &&
                      seenType->field(name) != nullptr && isOpen;
  if (isClassField) {
    return this->readClassField(seen, get.key, name);
  }
  if (classDecl == nullptr || classDecl->literal() != nullptr || seenType->field(name) != nullptr) {
    return nullptr;
  }

  return this->checkReflectMethodGet(call, seen, classDecl, name);
}

Expr *ReflectChecker::readNamedField(const NameExpr *object, const StringLitExpr *key,
                                     const std::string &name) {
  auto *root = this->state.getAst().make<NameExpr>(object->loc(), object->name);
  auto *read =
      this->state.getAst().make<MemberExpr>(key->loc(), root, this->state.getAst().keepName(name));
  auto *checked = this->exprChecker->checkExpr(read);

  return this->typeRelations->convert(checked, Type::unknown());
}

Expr *ReflectChecker::readClassField(Expr *seen, const StringLitExpr *key,
                                     const std::string &name) {
  auto *read =
      this->state.getAst().make<MemberExpr>(key->loc(), seen, this->state.getAst().keepName(name));
  auto *checked = this->classMemberChecker->checkClassMember(read, nullptr);

  return this->typeRelations->convert(checked, Type::unknown());
}

Expr *ReflectChecker::checkReflectMethodGet(CallExpr *call, Expr *seen, ClassDecl *classDecl,
                                            const std::string &name) {
  auto known = this->reflectCallChecker->knownReflectMethod(classDecl, name);
  if (!known.has_value()) {
    return nullptr;
  }
  call->args.resize(1);
  call->args[0] = seen;
  call->target = BuiltinCall{.builtin = Builtin::ReflectMethod, .method = known->method->sym};
  call->type = Type::unknown();
  if (known->hasSeveral) {
    this->state.getExpressionMarks().markSeveralVariants(call);
  }

  return call;
}

Expr *ReflectChecker::checkDynamicGet(CallExpr *call, const ReflectGet &get) {
  call->args[1] = this->argumentChecker->checkArg(call->args[1], Type::string());
  this->noteReflectedRead(get.type, get.base);
  call->args[0] = this->typeRelations->convert(call->args[0], Type::unknown());
  if (!this->isOnlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectGet};
  call->type = Type::unknown();

  return call;
}

void ReflectChecker::noteReflectedRead(const Type *type, const Type *base) {
  if (this->isOnlyChecked()) {
    return;
  }
  if (type->isUnknown() || base->isAnyObject() || base->classDecl() == nullptr) {
    this->state.getReflectStore().markAnyObjectRead();
    return;
  }
  auto reflected = llvm::SmallPtrSet<ClassDecl *, 8>();
  this->classesIn(base, reflected);
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
    auto message = ErrorTextReflectFill::notClass(type->name());
    this->state.getDiagnostics().error(cls->loc(), message);
    return call;
  }
  auto fails = this->fillFailure(cls, type);
  if (!fails.has_value()) {
    return call;
  }
  this->flowChecker->calledFunction();
  call->target = BuiltinCall{.builtin = Builtin::ReflectFill};
  call->type = Type::result(ClassValueChecker::constructedBy(type), *fails);

  return call;
}

std::optional<const Type *> ReflectChecker::fillFailure(const Expr *cls, const Type *type) {
  const auto *fails = this->prelude->builtinType(PreludeType::REFLECT_FILL_ERROR);
  auto *made = const_cast<ClassDecl *>(type->classRef());
  if (made == nullptr) {
    if (!this->isOnlyChecked()) {
      this->state.getReflectStore().markAnyObjectFilled();
      this->state.getReflectStore().markReflectionUsed();
    }
    return fails;
  }
  auto fillable = this->checkFillableClass(made);
  if (fillable.isErr()) {
    this->state.getDiagnostics().error(cls->loc(), fillable.error().message);
    return std::nullopt;
  }
  made->reflectFill = true;

  return this->prelude->builtinType(PreludeType::REFLECT_FILL_VALUE_ERROR);
}

Expr *ReflectChecker::checkReflectApply(CallExpr *call, MemberExpr *member) {
  if (this->hasWrongArity(call, member, 3, 3)) {
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  const auto *held = this->reflectCallChecker->heldReflectMethod(call->args[0]);
  const auto *gives = this->appliedResult(call->args[0]->type);
  this->walkAppliedFunction(call);
  this->walkAppliedThis(call);
  call->args[2] = this->reflectCallChecker->checkReflectArgs(call->args[2]);
  this->flowChecker->calledFunction();
  bool isClean = held != nullptr && gives->isUnknown() && !call->args[0]->type->isError() &&
                 !call->args[1]->type->isError() && !call->args[2]->type->isError();
  Expr *direct = isClean ? this->reflectCallChecker->directReflectApply(call, held) : nullptr;
  if (direct != nullptr) {
    return this->okOf(direct);
  }
  if (!this->isOnlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
    this->state.getReflectStore().markAnyObjectRead();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectApply};
  call->type = Type::result(gives, this->reflectError());

  return call;
}

const Type *ReflectChecker::appliedResult(const Type *type) const {
  bool isReturning = type->isFunction() && type->ret() != nullptr && !type->ret()->isError();

  return isReturning ? type->ret() : Type::unknown();
}

void ReflectChecker::walkAppliedFunction(CallExpr *call) {
  const Type *type = call->args[0]->type;
  if (!type->isError() && !type->isUnknown() && !type->isFunction()) {
    auto message = ErrorTextReflectApply::notFunc(type->name());
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return;
  }
  call->args[0] = this->typeRelations->convert(call->args[0], Type::unknown());
}

void ReflectChecker::walkAppliedThis(CallExpr *call) {
  call->args[1] = this->exprChecker->checkExpr(call->args[1]);
  if (call->args[1]->type->isVoid()) {
    auto message = ErrorTextReflectApply::voidThis();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return;
  }
  call->args[1] = this->typeRelations->convert(call->args[1], Type::unknown());
}

Expr *ReflectChecker::checkReflectConstruct(CallExpr *call, MemberExpr *member,
                                            const Type *expected) {
  if (call->args.size() == 3) {
    auto message = ErrorTextReflectNew::third();
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
    auto message = ErrorTextReflectNew::notClass(type->name());
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
    return call;
  }
  bool isLiteral =
      name != nullptr && list != nullptr && type->classRef() != nullptr && !this->hasSpread(list);
  if (isLiteral) {
    return this->checkLiteralConstruct(call, name, list, expected);
  }

  return this->checkDynamicConstruct(call, type);
}

bool ReflectChecker::hasSpread(const ArrayLitExpr *list) const {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      return true;
    }
  }

  return false;
}

Expr *ReflectChecker::checkLiteralConstruct(CallExpr *call, const NameExpr *name,
                                            const ArrayLitExpr *list, const Type *expected) {
  auto args = std::vector<Expr *>();
  for (const ArrayLitExpr::Element &element : list->elements) {
    args.push_back(element.expr);
  }
  auto *create =
      this->state.getAst().make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
  create->callee = this->state.getAst().make<NameExpr>(name->loc(), name->name);
  const Type *wanted = expected != nullptr && expected->nonOptional()->isResult()
                           ? expected->nonOptional()->okType()
                           : nullptr;
  auto *made = this->classValueChecker->checkValueNew(create, wanted);

  return this->okOf(made);
}

Expr *ReflectChecker::checkDynamicConstruct(CallExpr *call, const Type *type) {
  bool isKnown = false;
  const ClassDecl *made = type->classRef();
  if (made != nullptr) {
    if (made->type == nullptr) {
      auto message = ErrorTextReflectNew::javascript(made->name);
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
      call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
      return call;
    }
    isKnown = !made->isAbstract;
    if (!isKnown) {
      call->args[0] = this->typeRelations->convert(call->args[0], Type::constructorOf(made->type));
    }
  }
  call->args[1] = this->reflectCallChecker->checkReflectArgs(call->args[1]);
  this->flowChecker->calledFunction();
  if (!isKnown && !this->isOnlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectConstruct};
  const auto *constructed = ClassValueChecker::constructedBy(call->args[0]->type);
  call->type = Type::result(constructed, this->reflectError());

  return call;
}

}
