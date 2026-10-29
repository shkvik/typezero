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
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

void classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out) {
  type = type->nonOptional();
  if (type->isArray()) {
    classesIn(type->inner(), out);
    return;
  }
  if (type->isUnion()) {
    for (const Type *m : type->members()) {
      classesIn(m, out);
    }
    return;
  }
  if (ClassDecl *cls = const_cast<ClassDecl *>(type->classDecl()); cls && !cls->literal) {
    out.insert(cls);
  }
}

void constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out,
                    llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!type || !seen.insert(type).second) {
    return;
  }
  if (type->isConstructor()) {
    out.push_back(type->constructed());
    return;
  }
  if (type->isOptional() || type->isArray() || type->isPromise()) {
    constructorsIn(type->inner(), out, seen);
    return;
  }
  if (type->isResult()) {
    constructorsIn(type->okType(), out, seen);
    constructorsIn(type->errType(), out, seen);
    return;
  }
  if (type->isDict()) {
    constructorsIn(type->keyType(), out, seen);
    constructorsIn(type->valueType(), out, seen);
    return;
  }
  if (type->isFunction()) {
    for (const Type *param : type->params()) {
      constructorsIn(param, out, seen);
    }
    constructorsIn(type->ret(), out, seen);
    return;
  }
  if (type->isIterator() || type->isAsyncIterator()) {
    constructorsIn(type->yielded(), out, seen);
    return;
  }
  for (const Type *m : type->members()) {
    constructorsIn(m, out, seen);
  }
  if (type->isObject() && !type->isClass()) {
    for (const Field &f : type->fields()) {
      constructorsIn(f.type, out, seen);
    }
  }
}

bool instanceOf(const Type *made, const Type *of) {
  return made == of || of->isAnyObject() || made->isSubtypeOf(of);
}

Expr *stripImplicit(Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

}

ReflectChecker::ReflectChecker(Checker &checker) : checker_(checker) {}

bool ReflectChecker::onlyChecked() const {
  return checker_.trial_ || llvm::any_of(checker_.functions_, [](const FunctionDecl *fn) {
           return (!fn->typeParams.empty() && !fn->isInstance) ||
                  (fn->cls && (fn->cls->isGeneric() || fn->cls->checkOnly));
         });
}

void ReflectChecker::noteReflectedConversion(const Type *from, const Type *to,
                                             const CastExpr *cast) {
  if (onlyChecked()) {
    return;
  }
  if (to->nonOptional()->isConstructor()) {
    if (ClassDecl *cls = const_cast<ClassDecl *>(from->nonOptional()->classRef())) {
      checker_.constructorCasts_[cast] = cls;
    }
    return;
  }
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown())) {
    classesIn(from, escaped_);
  }
}

void ReflectChecker::noteConstructorFlow(const Type *from, const Type *to) {
  llvm::SmallVector<const Type *, 4> sources, targets;
  llvm::SmallPtrSet<const Type *, 16> seenFrom, seenTo;
  constructorsIn(to, targets, seenTo);
  if (targets.empty()) {
    return;
  }
  constructorsIn(from, sources, seenFrom);
  for (const Type *x : sources) {
    for (const Type *y : targets) {
      if (x != y && instanceOf(x, y)) {
        checker_.constructorWidenings_[x].insert(y);
      } else if (x != y && instanceOf(y, x)) {
        checker_.constructorWidenings_[y].insert(x);
      }
    }
  }
}

void ReflectChecker::finishReflection() {
  for (const auto &entry : checker_.constructorCasts_) {
    entry.second->reflectNew = true;
  }
  if (!checker_.reflectionUsed_) {
    for (ClassDecl *cls : checker_.classes_) {
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
  if (reflectGetAny_) {
    for (ClassDecl *cls : escaped_) {
      methods(cls);
    }
    for (ClassDecl *cls : checker_.classes_) {
      if (cls->reflectNew) {
        methods(cls);
      }
    }
  }
  for (ClassDecl *cls : reflectedObjects_) {
    methods(cls);
  }
  checkReflectNews();
}

const Type *ReflectChecker::reflectError() {
  return checker_.prelude().builtinType("ReflectError");
}

Expr *ReflectChecker::okOf(Expr *value) {
  if (value->type->isError()) {
    return value;
  }
  auto *call = checker_.ctx_.make<CallExpr>(
      value->loc(), checker_.ctx_.make<NameExpr>(value->loc(), "ok"), std::vector<Expr *>{value});
  call->builtin = Builtin::ResultOk;
  call->type = Type::result(value->type, reflectError());

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
    checker_.returnInferrer().resolveParams(sym);
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
  if (checker_.reflectNews_.empty()) {
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
        auto next = checker_.constructorWidenings_.find(at);
        if (next == checker_.constructorWidenings_.end()) {
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
  for (const auto &[cast, cls] : checker_.constructorCasts_) {
    const Type *target = cast->type ? cast->type->nonOptional() : nullptr;
    if (target && target->isConstructor()) {
      entries.push_back({cls, target->constructed()});
    }
  }
  for (const auto &[expr, args] : checker_.reflectNews_) {
    const Type *made = expr->type;
    if (!made || made->isError()) {
      continue;
    }
    llvm::SmallPtrSet<ClassDecl *, 8> checked;
    for (const auto &[cls, entry] : entries) {
      if (!cls->type || !reachable(entry).contains(made) || !checked.insert(cls).second) {
        continue;
      }
      std::string problem = cls->builtin ? "it is a class of JavaScript, which only "
                                           "Reflect.construct tries to make"
                                         : constructProblem(cls, args);
      if (!problem.empty()) {
        checker_.diags_.error(expr->loc(),
                              "'new' of a class known only when the program runs may fail: '" +
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
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (checker_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "get") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    auto *object = dyn_cast<NameExpr>(call->args[0]);
    auto *key = dyn_cast<StringLitExpr>(call->args[1]);
    Expr *&target = call->args[0];
    target = checker_.exprChecker().checkExpr(target);
    const Type *type = target->type;
    if (type->isError()) {
      call->args[1] = checker_.exprChecker().checkExpr(call->args[1]);
      return call;
    }
    const Type *base = GenericChecker::apparentType(type->nonOptional());
    if (base->isJson()) {
      checker_.diags_.error(target->loc(),
                            "Reflect.get takes an object of the language; read a key of a "
                            "Json as JavaScript does, 'j[key]'");
      checkArgsLoosely();
      return call;
    }
    if (!type->isUnknown() && !UnknownChecker::isObjectReference(base) && !base->isAnyObject()) {
      checker_.diags_.error(target->loc(),
                            "Reflect.get takes an object, and this is a '" + type->name() + "'");
      call->args[1] = checker_.exprChecker().checkExpr(call->args[1]);
      return call;
    }
    Expr *seen = stripImplicit(target);
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
        checker_.classDeclarer().completeClass(cls);
      }
      const ClassField *declared = cls ? ClassBases::findField(cls, name) : nullptr;
      bool open = !cls || (declared && declared->access == Access::Public);
      if (object && !type->isOptional() && base->isObject() && !base->isAnyObject() &&
          base->field(name) && open) {
        auto *read = checker_.ctx_.make<MemberExpr>(
            key->loc(), checker_.ctx_.make<NameExpr>(object->loc(), object->name),
            checker_.ctx_.keepName(name));
        return checker_.typeRelations().convert(checker_.exprChecker().checkExpr(read),
                                                Type::unknown());
      }
      if (cls && !cls->literal && seenType->field(name) && open) {
        auto *read = checker_.ctx_.make<MemberExpr>(key->loc(), seen, checker_.ctx_.keepName(name));
        return checker_.typeRelations().convert(
            checker_.classMemberChecker().checkClassMember(read, nullptr), Type::unknown());
      }
      bool several = false;
      if (FunctionDecl *method =
              cls && !cls->literal && !seenType->field(name)
                  ? checker_.reflectCallChecker().knownReflectMethod(cls, name, several)
                  : nullptr) {
        call->args.resize(1);
        target = seen;
        call->builtin = Builtin::ReflectMethod;
        call->method = method->sym;
        call->type = Type::unknown();
        if (several) {
          checker_.severalVariants_.insert(call);
        }
        return call;
      }
    }
    call->args[1] = checker_.argumentChecker().checkArg(call->args[1], Type::string());
    if (onlyChecked()) {
    } else if (type->isUnknown() || base->isAnyObject() || !base->classDecl()) {
      reflectGetAny_ = true;
    } else {
      classesIn(base, reflectedObjects_);
    }
    target = checker_.typeRelations().convert(target, Type::unknown());
    checker_.reflectionUsed_ |= !onlyChecked();
    call->builtin = Builtin::ReflectGet;
    call->type = Type::unknown();
    return call;
  }
  if (fn == "apply") {
    if (!checker_.argumentChecker().checkArity(call, member->loc(), 3, 3)) {
      return call;
    }
    Expr *&f = call->args[0];
    f = checker_.exprChecker().checkExpr(f);
    const FunctionDecl *held = checker_.reflectCallChecker().heldReflectMethod(f);
    if (!f->type->isError() && !f->type->isUnknown() && !f->type->isFunction()) {
      checker_.diags_.error(f->loc(), "Reflect.apply calls a function, and this is a '" +
                                          f->type->name() + "'");
    } else {
      f = checker_.typeRelations().convert(f, Type::unknown());
    }
    Expr *&self = call->args[1];
    self = checker_.exprChecker().checkExpr(self);
    if (self->type->isVoid()) {
      checker_.diags_.error(self->loc(),
                            "an expression of type 'void' has no value to pass as 'this'");
    } else {
      self = checker_.typeRelations().convert(self, Type::unknown());
    }
    call->args[2] = checker_.reflectCallChecker().checkReflectArgs(call->args[2]);
    checker_.flowChecker().calledFunction();
    if (held && !f->type->isError() && !self->type->isError() && !call->args[2]->type->isError()) {
      if (Expr *direct = checker_.reflectCallChecker().directReflectApply(call, held)) {
        return okOf(direct);
      }
    }
    if (!onlyChecked()) {
      checker_.reflectionUsed_ = true;
      reflectGetAny_ = true;
    }
    call->builtin = Builtin::ReflectApply;
    call->type = Type::result(Type::unknown(), reflectError());
    return call;
  }
  if (call->args.size() == 3) {
    checker_.diags_.error(call->args[2]->loc(),
                          "the third argument of Reflect.construct, the class whose "
                          "prototype the object gets, is not supported: there are no "
                          "prototypes in tz; make an object of that class");
    checkArgsLoosely();
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
    return call;
  }
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  Expr *&cls = call->args[0];
  cls = checker_.exprChecker().checkExpr(cls);
  const Type *type = cls->type;
  if (type->isError()) {
    call->args[1] = checker_.reflectCallChecker().checkReflectArgs(call->args[1]);
    return call;
  }
  if (!type->isClassRef()) {
    checker_.diags_.error(cls->loc(),
                          "Reflect.construct makes an object of a class, and this is a '" +
                              type->name() + "'");
    call->args[1] = checker_.reflectCallChecker().checkReflectArgs(call->args[1]);
    return call;
  }
  if (name && list && type->classRef() &&
      llvm::none_of(list->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
    std::vector<Expr *> args;
    for (const ArrayLitExpr::Element &e : list->elements) {
      args.push_back(e.expr);
    }
    auto *create =
        checker_.ctx_.make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
    create->callee = checker_.ctx_.make<NameExpr>(name->loc(), name->name);
    const Type *wanted = expected && expected->nonOptional()->isResult()
                             ? expected->nonOptional()->okType()
                             : nullptr;
    return okOf(checker_.classValueChecker().checkValueNew(create, wanted));
  }
  bool known = false;
  if (const ClassDecl *made = type->classRef()) {
    if (!made->type) {
      checker_.diags_.error(cls->loc(), "'" + made->name +
                                            "' of JavaScript cannot be made through "
                                            "reflection");
      call->args[1] = checker_.reflectCallChecker().checkReflectArgs(call->args[1]);
      return call;
    }
    known = !made->isAbstract;
    if (!known) {
      cls = checker_.typeRelations().convert(cls, Type::constructorOf(made->type));
    }
  }
  call->args[1] = checker_.reflectCallChecker().checkReflectArgs(call->args[1]);
  checker_.flowChecker().calledFunction();
  checker_.reflectionUsed_ |= !known && !onlyChecked();
  call->builtin = Builtin::ReflectConstruct;
  call->type = Type::result(ClassValueChecker::constructedBy(cls->type), reflectError());

  return call;
}

}
