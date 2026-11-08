#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
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
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

void ReflectChecker::classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out) {
  type = type->nonOptional();
  if (type->isArray()) {
    ReflectChecker::classesIn(type->inner(), out);
    return;
  }
  if (type->isUnion()) {
    for (const Type *m : type->members()) {
      ReflectChecker::classesIn(m, out);
    }
    return;
  }
  if (ClassDecl *cls = const_cast<ClassDecl *>(type->classDecl()); cls && !cls->literal()) {
    out.insert(cls);
  }
}

void ReflectChecker::constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out,
                                    llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!type || !seen.insert(type).second) {
    return;
  }
  if (type->isConstructor()) {
    out.push_back(type->constructed());
    return;
  }
  if (type->isOptional() || type->isArray() || type->isPromise()) {
    ReflectChecker::constructorsIn(type->inner(), out, seen);
    return;
  }
  if (type->isResult()) {
    ReflectChecker::constructorsIn(type->okType(), out, seen);
    ReflectChecker::constructorsIn(type->errType(), out, seen);
    return;
  }
  if (type->isDict()) {
    ReflectChecker::constructorsIn(type->keyType(), out, seen);
    ReflectChecker::constructorsIn(type->valueType(), out, seen);
    return;
  }
  if (type->isFunction()) {
    for (const Type *param : type->params()) {
      ReflectChecker::constructorsIn(param, out, seen);
    }
    ReflectChecker::constructorsIn(type->ret(), out, seen);
    return;
  }
  if (type->isIterator() || type->isAsyncIterator()) {
    ReflectChecker::constructorsIn(type->yielded(), out, seen);
    return;
  }
  for (const Type *m : type->members()) {
    ReflectChecker::constructorsIn(m, out, seen);
  }
  if (type->isObject() && !type->isClass()) {
    for (const Field &f : type->fields()) {
      ReflectChecker::constructorsIn(f.type, out, seen);
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

ReflectChecker::ReflectChecker(SemanticState &owner)
    : state(owner) {}

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
  llvm::SmallPtrSet<const Type *, 16> seenFrom, seenTo;
  ReflectChecker::constructorsIn(to, targets, seenTo);
  if (targets.empty()) {
    return;
  }
  ReflectChecker::constructorsIn(from, sources, seenFrom);
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
  std::function<void(ClassDecl *)> methods = [&](ClassDecl *cls) {
    if (cls->reflectMethods) {
      return;
    }
    cls->reflectMethods = true;
    for (ClassDecl *child : cls->children) {
      methods(child);
    }
  };
  if (reflect.isAnyObjectRead()) {
    for (ClassDecl *cls : reflect.getEscapedClasses()) {
      methods(cls);
    }
    for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
      if (cls->reflectNew) {
        methods(cls);
      }
    }
  }
  for (ClassDecl *cls : reflect.getReflectedObjects()) {
    methods(cls);
  }
  this->checkReflectNews();
}

const Type *ReflectChecker::reflectError() {
  return Prelude(this->state).builtinType("ReflectError");
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
    ReturnInferrer(this->state).resolveParams(sym);
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
  llvm::StringRef fn = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  if (fn == "get") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    auto *object = dyn_cast<NameExpr>(call->args[0]);
    auto *key = dyn_cast<StringLitExpr>(call->args[1]);
    Expr *&target = call->args[0];
    target = ExprChecker(this->state).checkExpr(target);
    const Type *type = target->type;
    if (type->isError()) {
      call->args[1] = ExprChecker(this->state).checkExpr(call->args[1]);
      return call;
    }
    const Type *base = GenericChecker::apparentType(type->nonOptional());
    if (base->isJson()) {
      std::string message = ErrorTextReflectGet::json();
      this->state.getDiagnostics().error(target->loc(), message);
      checkArgsLoosely();
      return call;
    }
    if (!type->isUnknown() && !UnknownChecker::isObjectReference(base) && !base->isAnyObject()) {
      std::string message = ErrorTextReflectGet::notObject(type->name());
      this->state.getDiagnostics().error(target->loc(), message);
      call->args[1] = ExprChecker(this->state).checkExpr(call->args[1]);
      return call;
    }
    Expr *seen = ReflectChecker::stripImplicit(target);
    const Type *seenType = seen->type;
    ClassDecl *cls = seenType->isObject() && !seenType->isAnyObject()
                         ? ClassDeclarer::classOf(seenType)
                         : nullptr;
    if (key && !seenType->isOptional() &&
        llvm::all_of(key->value, [](char16_t c) { return c < 128; })) {
      std::string name;
      for (char16_t c : key->value) {
        name.push_back(char(c));
      }
      if (cls) {
        ClassDeclarer(this->state).completeClass(cls);
      }
      const ClassField *declared = cls ? ClassBases::findField(cls, name) : nullptr;
      bool open = !cls || (declared && declared->access == Access::Public);
      if (object && !type->isOptional() && base->isObject() && !base->isAnyObject() &&
          base->field(name) && open) {
        auto *read = this->state.getAst().make<MemberExpr>(
            key->loc(), this->state.getAst().make<NameExpr>(object->loc(), object->name),
            this->state.getAst().keepName(name));
        return TypeRelations(this->state)
            .convert(ExprChecker(this->state).checkExpr(read), Type::unknown());
      }
      if (cls && !cls->literal() && seenType->field(name) && open) {
        auto *read = this->state.getAst().make<MemberExpr>(key->loc(), seen,
                                                           this->state.getAst().keepName(name));
        return TypeRelations(this->state)
            .convert(ClassMemberChecker(this->state).checkClassMember(read, nullptr),
                     Type::unknown());
      }
      bool several = false;
      if (FunctionDecl *method =
              cls && !cls->literal() && !seenType->field(name)
                  ? ReflectCallChecker(this->state).knownReflectMethod(cls, name, several)
                  : nullptr) {
        call->args.resize(1);
        target = seen;
        call->target = BuiltinCall{.builtin = Builtin::ReflectMethod, .method = method->sym};
        call->type = Type::unknown();
        if (several) {
          this->state.getExpressionMarks().markSeveralVariants(call);
        }
        return call;
      }
    }
    call->args[1] = ArgumentChecker(this->state).checkArg(call->args[1], Type::string());
    if (this->onlyChecked()) {
    } else if (type->isUnknown() || base->isAnyObject() || !base->classDecl()) {
      this->state.getReflectStore().markAnyObjectRead();
    } else {
      llvm::SmallPtrSet<ClassDecl *, 8> reflected;
      ReflectChecker::classesIn(base, reflected);
      for (ClassDecl *reflectedClass : reflected) {
        this->state.getReflectStore().addReflectedObject(reflectedClass);
      }
    }
    target = TypeRelations(this->state).convert(target, Type::unknown());
    if (!this->onlyChecked()) {
      this->state.getReflectStore().markReflectionUsed();
    }
    call->target = BuiltinCall{.builtin = Builtin::ReflectGet};
    call->type = Type::unknown();
    return call;
  }
  if (fn == "fill") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&cls = call->args[0];
    cls = ExprChecker(this->state).checkExpr(cls);
    call->args[1] = ArgumentChecker(this->state)
                        .checkArg(call->args[1],
                                  Type::dict(Type::DictKind::Map, Type::string(), Type::unknown()));
    const Type *type = cls->type;
    if (type->isError() || call->args[1]->type->isError()) {
      return call;
    }
    if (!type->isClassRef()) {
      std::string message = ErrorTextReflectFill::notClass(type->name());
      this->state.getDiagnostics().error(cls->loc(), message);
      return call;
    }
    const Type *fails = Prelude(this->state).builtinType("ReflectFillError");
    if (ClassDecl *made = const_cast<ClassDecl *>(type->classRef())) {
      ResultFillableClass fillable = ReflectChecker::fillableClass(made);
      if (fillable.isErr()) {
        this->state.getDiagnostics().error(cls->loc(), fillable.error().message);
        return call;
      }
      made->reflectFill = true;
      fails = Prelude(this->state).builtinType("ReflectFillValueError");
    } else if (!this->onlyChecked()) {
      this->state.getReflectStore().markAnyObjectFilled();
      this->state.getReflectStore().markReflectionUsed();
    }
    FlowChecker(this->state).calledFunction();
    call->target = BuiltinCall{.builtin = Builtin::ReflectFill};
    call->type = Type::result(ClassValueChecker::constructedBy(type), fails);
    return call;
  }
  if (fn == "apply") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 3, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&f = call->args[0];
    f = ExprChecker(this->state).checkExpr(f);
    const FunctionDecl *held = ReflectCallChecker(this->state).heldReflectMethod(f);
    const Type *gives = f->type->isFunction() && f->type->ret() && !f->type->ret()->isError()
                            ? f->type->ret()
                            : Type::unknown();
    if (!f->type->isError() && !f->type->isUnknown() && !f->type->isFunction()) {
      std::string message = ErrorTextReflectApply::notFunc(f->type->name());
      this->state.getDiagnostics().error(f->loc(), message);
    } else {
      f = TypeRelations(this->state).convert(f, Type::unknown());
    }
    Expr *&self = call->args[1];
    self = ExprChecker(this->state).checkExpr(self);
    if (self->type->isVoid()) {
      std::string message = ErrorTextReflectApply::voidThis();
      this->state.getDiagnostics().error(self->loc(), message);
    } else {
      self = TypeRelations(this->state).convert(self, Type::unknown());
    }
    call->args[2] = ReflectCallChecker(this->state).checkReflectArgs(call->args[2]);
    FlowChecker(this->state).calledFunction();
    if (held && gives->isUnknown() && !f->type->isError() && !self->type->isError() &&
        !call->args[2]->type->isError()) {
      if (Expr *direct = ReflectCallChecker(this->state).directReflectApply(call, held)) {
        return this->okOf(direct);
      }
    }
    if (!this->onlyChecked()) {
      this->state.getReflectStore().markReflectionUsed();
      this->state.getReflectStore().markAnyObjectRead();
    }
    call->target = BuiltinCall{.builtin = Builtin::ReflectApply};
    call->type = Type::result(gives, this->reflectError());
    return call;
  }
  if (call->args.size() == 3) {
    std::string message = ErrorTextReflectNew::third();
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    checkArgsLoosely();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  Expr *&cls = call->args[0];
  cls = ExprChecker(this->state).checkExpr(cls);
  const Type *type = cls->type;
  if (type->isError()) {
    call->args[1] = ReflectCallChecker(this->state).checkReflectArgs(call->args[1]);
    return call;
  }
  if (!type->isClassRef()) {
    std::string message = ErrorTextReflectNew::notClass(type->name());
    this->state.getDiagnostics().error(cls->loc(), message);
    call->args[1] = ReflectCallChecker(this->state).checkReflectArgs(call->args[1]);
    return call;
  }
  if (name && list && type->classRef() &&
      llvm::none_of(list->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
    std::vector<Expr *> args;
    for (const ArrayLitExpr::Element &e : list->elements) {
      args.push_back(e.expr);
    }
    auto *create = this->state.getAst().make<NewExpr>(call->loc(), "", std::vector<TypeRef>(),
                                                      std::move(args));
    create->callee = this->state.getAst().make<NameExpr>(name->loc(), name->name);
    const Type *wanted = expected && expected->nonOptional()->isResult()
                             ? expected->nonOptional()->okType()
                             : nullptr;
    return this->okOf(ClassValueChecker(this->state).checkValueNew(create, wanted));
  }
  bool known = false;
  if (const ClassDecl *made = type->classRef()) {
    if (!made->type) {
      std::string message = ErrorTextReflectNew::javascript(made->name);
      this->state.getDiagnostics().error(cls->loc(), message);
      call->args[1] = ReflectCallChecker(this->state).checkReflectArgs(call->args[1]);
      return call;
    }
    known = !made->isAbstract;
    if (!known) {
      cls = TypeRelations(this->state).convert(cls, Type::constructorOf(made->type));
    }
  }
  call->args[1] = ReflectCallChecker(this->state).checkReflectArgs(call->args[1]);
  FlowChecker(this->state).calledFunction();
  if (!known && !this->onlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectConstruct};
  call->type = Type::result(ClassValueChecker::constructedBy(cls->type), this->reflectError());

  return call;
}

}
