#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/whole_program/flows/flows.h"
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

ReflectChecker::ReflectChecker(Checker &owner)
    : checker(owner) {}

bool ReflectChecker::onlyChecked() const {
  return this->checker.trial_ || llvm::any_of(this->checker.functions_, [](const FunctionDecl *fn) {
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
      this->checker.constructorCasts_[cast] = cls;
    }
    return;
  }
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown())) {
    ReflectChecker::classesIn(from, this->escaped);
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
        this->checker.constructorWidenings_[x].insert(y);
      } else if (x != y && ReflectChecker::instanceOf(y, x)) {
        this->checker.constructorWidenings_[y].insert(x);
      }
    }
  }
}

void ReflectChecker::finishReflection() {
  for (const auto &entry : this->checker.constructorCasts_) {
    entry.second->reflectNew = true;
  }
  if (this->reflectFillAny) {
    for (ClassDecl *cls : this->checker.classes_) {
      cls->reflectFill = !cls->builtin();
    }
  }
  if (!this->checker.reflectionUsed_) {
    for (ClassDecl *cls : this->checker.classes_) {
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
  if (this->reflectGetAny) {
    for (ClassDecl *cls : this->escaped) {
      methods(cls);
    }
    for (ClassDecl *cls : this->checker.classes_) {
      if (cls->reflectNew) {
        methods(cls);
      }
    }
  }
  for (ClassDecl *cls : this->reflectedObjects) {
    methods(cls);
  }
  this->checkReflectNews();
}

const Type *ReflectChecker::reflectError() {
  return this->checker.prelude().builtinType("ReflectError");
}

Expr *ReflectChecker::okOf(Expr *value) {
  if (value->type->isError()) {
    return value;
  }
  auto *call = this->checker.ctx_.make<CallExpr>(
      value->loc(), this->checker.ctx_.make<NameExpr>(value->loc(), "ok"),
      std::vector<Expr *>{value});
  call->target = BuiltinCall{.builtin = Builtin::ResultOk};
  call->type = Type::result(value->type, this->reflectError());

  return call;
}

std::string ReflectChecker::constructProblem(ClassDecl *cls, llvm::ArrayRef<const Type *> args) {
  if (cls->isAbstract) {
    return "it is abstract";
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (ctors.empty()) {
    return args.empty() ? "" : "its constructor takes no arguments";
  }
  size_t fitting = 0;
  std::string takes;
  for (FunctionDecl *ctor : ctors) {
    FuncSymbol *sym = ctor->sym;
    if (!sym) {
      return "its constructor is unknown";
    }
    this->checker.returnInferrer().resolveParams(sym);
    std::string shown;
    for (const Type *param : sym->params) {
      shown += (shown.empty() ? "" : ", ") + (param ? param->name().str() : "?");
    }
    takes += (takes.empty() ? "(" : " or (") + shown + ")";
    size_t fixed = sym->hasRest ? sym->params.size() - 1 : sym->params.size();
    bool fits = args.size() >= sym->minArgs && (sym->hasRest || args.size() <= fixed);
    for (size_t i = 0; i < args.size() && fits; ++i) {
      const Type *param = i < fixed ? sym->params[i] : sym->params.back()->inner();
      fits = param && ReflectCallChecker::reflectAccepts(args[i], param);
    }
    fitting += fits;
  }
  if (fitting == 1) {
    return "";
  }

  return fitting == 0 ? "its constructor takes " + takes
                      : "the arguments may fit several of its constructors";
}

void ReflectChecker::checkReflectNews() {
  if (this->checker.reflectNews_.empty()) {
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
        auto next = this->checker.constructorWidenings_.find(at);
        if (next == this->checker.constructorWidenings_.end()) {
          continue;
        }
        for (const Type *to : next->second) {
          if (it->second.insert(to).second) {
            work.push_back(to);
          }
        }
      }
    }
    return it->second;
  };
  std::vector<std::pair<ClassDecl *, const Type *>> entries;
  for (const auto &[cast, cls] : this->checker.constructorCasts_) {
    const Type *target = cast->type ? cast->type->nonOptional() : nullptr;
    if (target && target->isConstructor()) {
      entries.push_back({cls, target->constructed()});
    }
  }
  for (const auto &[expr, args] : this->checker.reflectNews_) {
    const Type *made = expr->type;
    if (!made || made->isError()) {
      continue;
    }
    llvm::SmallPtrSet<ClassDecl *, 8> checked;
    for (const auto &[cls, entry] : entries) {
      if (!cls->type || !reachable(entry).contains(made) || !checked.insert(cls).second) {
        continue;
      }
      std::string problem = cls->builtin() ? "it is a class of JavaScript, which only "
                                             "Reflect.construct tries to make"
                                           : this->constructProblem(cls, args);
      if (!problem.empty()) {
        this->checker.diags_.error(
            expr->loc(), "'new' of a class known only when the program runs may fail: '" +
                             cls->type->name().str() + "' can be here, and " + problem +
                             "; write 'Reflect.construct(cls, [...])', which gives a Result");
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
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "get") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    auto *object = dyn_cast<NameExpr>(call->args[0]);
    auto *key = dyn_cast<StringLitExpr>(call->args[1]);
    Expr *&target = call->args[0];
    target = this->checker.exprChecker().checkExpr(target);
    const Type *type = target->type;
    if (type->isError()) {
      call->args[1] = this->checker.exprChecker().checkExpr(call->args[1]);
      return call;
    }
    const Type *base = GenericChecker::apparentType(type->nonOptional());
    if (base->isJson()) {
      this->checker.diags_.error(target->loc(),
                                 "Reflect.get takes an object of the language; read a key of a "
                                 "Json as JavaScript does, 'j[key]'");
      checkArgsLoosely();
      return call;
    }
    if (!type->isUnknown() && !UnknownChecker::isObjectReference(base) && !base->isAnyObject()) {
      this->checker.diags_.error(target->loc(), "Reflect.get takes an object, and this is a '" +
                                                    type->name() + "'");
      call->args[1] = this->checker.exprChecker().checkExpr(call->args[1]);
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
        this->checker.classDeclarer().completeClass(cls);
      }
      const ClassField *declared = cls ? ClassBases::findField(cls, name) : nullptr;
      bool open = !cls || (declared && declared->access == Access::Public);
      if (object && !type->isOptional() && base->isObject() && !base->isAnyObject() &&
          base->field(name) && open) {
        auto *read = this->checker.ctx_.make<MemberExpr>(
            key->loc(), this->checker.ctx_.make<NameExpr>(object->loc(), object->name),
            this->checker.ctx_.keepName(name));
        return this->checker.typeRelations().convert(this->checker.exprChecker().checkExpr(read),
                                                     Type::unknown());
      }
      if (cls && !cls->literal() && seenType->field(name) && open) {
        auto *read = this->checker.ctx_.make<MemberExpr>(key->loc(), seen,
                                                         this->checker.ctx_.keepName(name));
        return this->checker.typeRelations().convert(
            this->checker.classMemberChecker().checkClassMember(read, nullptr), Type::unknown());
      }
      bool several = false;
      if (FunctionDecl *method =
              cls && !cls->literal() && !seenType->field(name)
                  ? this->checker.reflectCallChecker().knownReflectMethod(cls, name, several)
                  : nullptr) {
        call->args.resize(1);
        target = seen;
        call->target = BuiltinCall{.builtin = Builtin::ReflectMethod, .method = method->sym};
        call->type = Type::unknown();
        if (several) {
          this->checker.severalVariants_.insert(call);
        }
        return call;
      }
    }
    call->args[1] = this->checker.argumentChecker().checkArg(call->args[1], Type::string());
    if (this->onlyChecked()) {
    } else if (type->isUnknown() || base->isAnyObject() || !base->classDecl()) {
      this->reflectGetAny = true;
    } else {
      ReflectChecker::classesIn(base, this->reflectedObjects);
    }
    target = this->checker.typeRelations().convert(target, Type::unknown());
    this->checker.reflectionUsed_ |= !this->onlyChecked();
    call->target = BuiltinCall{.builtin = Builtin::ReflectGet};
    call->type = Type::unknown();
    return call;
  }
  if (fn == "fill") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&cls = call->args[0];
    cls = this->checker.exprChecker().checkExpr(cls);
    call->args[1] = this->checker.argumentChecker().checkArg(
        call->args[1], Type::dict(Type::DictKind::Map, Type::string(), Type::unknown()));
    const Type *type = cls->type;
    if (type->isError() || call->args[1]->type->isError()) {
      return call;
    }
    if (!type->isClassRef()) {
      this->checker.diags_.error(cls->loc(),
                                 "Reflect.fill makes an object of a class, and this is a '" +
                                     type->name() + "'");
      return call;
    }
    const Type *fails = this->checker.prelude().builtinType("ReflectFillError");
    if (ClassDecl *made = const_cast<ClassDecl *>(type->classRef())) {
      std::string why = !made->type || made->builtin() ? "it is a class of JavaScript"
                        : made->isAbstract ? "it is abstract; fill a class that extends it"
                        : !ClassLinks::constructorsOf(made).empty()
                            ? "it sets its fields in a constructor; make it with 'new " +
                                  made->name.str() + "(...)'"
                            : "";
      if (!why.empty()) {
        this->checker.diags_.error(cls->loc(),
                                   "Reflect.fill cannot fill '" + made->name + "': " + why +
                                       " (Reflect.fill gives the fields of a class without "
                                       "a constructor their values)");
        return call;
      }
      made->reflectFill = true;
      fails = this->checker.prelude().builtinType("ReflectFillValueError");
    } else if (!this->onlyChecked()) {
      this->reflectFillAny = true;
      this->checker.reflectionUsed_ = true;
    }
    this->checker.flowChecker().calledFunction();
    call->target = BuiltinCall{.builtin = Builtin::ReflectFill};
    call->type = Type::result(ClassValueChecker::constructedBy(type), fails);
    return call;
  }
  if (fn == "apply") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 3, 3);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&f = call->args[0];
    f = this->checker.exprChecker().checkExpr(f);
    const FunctionDecl *held = this->checker.reflectCallChecker().heldReflectMethod(f);
    const Type *gives = f->type->isFunction() && f->type->ret() && !f->type->ret()->isError()
                            ? f->type->ret()
                            : Type::unknown();
    if (!f->type->isError() && !f->type->isUnknown() && !f->type->isFunction()) {
      this->checker.diags_.error(f->loc(), "Reflect.apply calls a function, and this is a '" +
                                               f->type->name() + "'");
    } else {
      f = this->checker.typeRelations().convert(f, Type::unknown());
    }
    Expr *&self = call->args[1];
    self = this->checker.exprChecker().checkExpr(self);
    if (self->type->isVoid()) {
      this->checker.diags_.error(self->loc(),
                                 "an expression of type 'void' has no value to pass as 'this'");
    } else {
      self = this->checker.typeRelations().convert(self, Type::unknown());
    }
    call->args[2] = this->checker.reflectCallChecker().checkReflectArgs(call->args[2]);
    this->checker.flowChecker().calledFunction();
    if (held && gives->isUnknown() && !f->type->isError() && !self->type->isError() &&
        !call->args[2]->type->isError()) {
      if (Expr *direct = this->checker.reflectCallChecker().directReflectApply(call, held)) {
        return this->okOf(direct);
      }
    }
    if (!this->onlyChecked()) {
      this->checker.reflectionUsed_ = true;
      this->reflectGetAny = true;
    }
    call->target = BuiltinCall{.builtin = Builtin::ReflectApply};
    call->type = Type::result(gives, this->reflectError());
    return call;
  }
  if (call->args.size() == 3) {
    this->checker.diags_.error(call->args[2]->loc(),
                               "the third argument of Reflect.construct, the class whose "
                               "prototype the object gets, is not supported: there are no "
                               "prototypes in tz; make an object of that class");
    checkArgsLoosely();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->checker.argumentChecker().checkArity(call, 2, 2);
  if (arity.isErr()) {
    this->checker.diags_.error(member->loc(), arity.error().message);
    return call;
  }
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  Expr *&cls = call->args[0];
  cls = this->checker.exprChecker().checkExpr(cls);
  const Type *type = cls->type;
  if (type->isError()) {
    call->args[1] = this->checker.reflectCallChecker().checkReflectArgs(call->args[1]);
    return call;
  }
  if (!type->isClassRef()) {
    this->checker.diags_.error(cls->loc(),
                               "Reflect.construct makes an object of a class, and this is a '" +
                                   type->name() + "'");
    call->args[1] = this->checker.reflectCallChecker().checkReflectArgs(call->args[1]);
    return call;
  }
  if (name && list && type->classRef() &&
      llvm::none_of(list->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
    std::vector<Expr *> args;
    for (const ArrayLitExpr::Element &e : list->elements) {
      args.push_back(e.expr);
    }
    auto *create =
        this->checker.ctx_.make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
    create->callee = this->checker.ctx_.make<NameExpr>(name->loc(), name->name);
    const Type *wanted = expected && expected->nonOptional()->isResult()
                             ? expected->nonOptional()->okType()
                             : nullptr;
    return this->okOf(this->checker.classValueChecker().checkValueNew(create, wanted));
  }
  bool known = false;
  if (const ClassDecl *made = type->classRef()) {
    if (!made->type) {
      this->checker.diags_.error(cls->loc(), "'" + made->name +
                                                 "' of JavaScript cannot be made through "
                                                 "reflection");
      call->args[1] = this->checker.reflectCallChecker().checkReflectArgs(call->args[1]);
      return call;
    }
    known = !made->isAbstract;
    if (!known) {
      cls = this->checker.typeRelations().convert(cls, Type::constructorOf(made->type));
    }
  }
  call->args[1] = this->checker.reflectCallChecker().checkReflectArgs(call->args[1]);
  this->checker.flowChecker().calledFunction();
  this->checker.reflectionUsed_ |= !known && !this->onlyChecked();
  call->target = BuiltinCall{.builtin = Builtin::ReflectConstruct};
  call->type = Type::result(ClassValueChecker::constructedBy(cls->type), this->reflectError());

  return call;
}

}
