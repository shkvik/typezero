#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/json/json_type_checker/json_type_checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/reflect/reflect_call_checker/reflect_call_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The classes whose objects a value of `type` may be.
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

Expr *stripImplicit(Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

} // namespace

ReflectChecker::ReflectChecker(Sema &sema) : sema_(sema) {}

bool ReflectChecker::onlyChecked() const {
  return sema_.trial_ || llvm::any_of(sema_.functions_, [](const FunctionDecl *fn) {
           return (!fn->typeParams.empty() && !fn->isInstance) ||
                  (fn->cls && (fn->cls->isGeneric() || fn->cls->checkOnly));
         });
}

void ReflectChecker::noteReflectedConversion(const Type *from, const Type *to, const CastExpr *cast) {
  // Code that is never emitted reaches no class.
  if (onlyChecked()) {
    return;
  }
  // A class that becomes some class known only when the program runs, which
  // `new` and Reflect.construct make objects of; unless a copy for the class
  // known when compiling drops the conversion (see exactClassArgs).
  if (to->nonOptional()->isConstructor()) {
    if (ClassDecl *cls = const_cast<ClassDecl *>(from->nonOptional()->classRef())) {
      sema_.constructorCasts_[cast] = cls;
    }
    return;
  }
  // Objects that code looks into only through reflection.
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown())) {
    classesIn(from, escaped_);
  }
}

void ReflectChecker::finishReflection() {
  for (const auto &entry : sema_.constructorCasts_) {
    entry.second->reflectNew = true;
  }
  if (!sema_.reflectionUsed_) {
    // Nothing looks into a class by name: none is described.
    for (ClassDecl *cls : sema_.classes_) {
      cls->reflectNew = cls->reflectMethods = cls->reflectJson = false;
    }
    return;
  }
  // The objects reflection may look into: of the classes they escape as, of
  // those it makes, and of the classes down their chains.
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
    for (ClassDecl *cls : sema_.classes_) {
      if (cls->reflectNew) {
        methods(cls);
      }
    }
  }
  for (ClassDecl *cls : reflectedObjects_) {
    methods(cls);
  }
  // fromJson(cls, j) of a class known only when the program runs: any class
  // that may be one, if a Json can make it.
  if (jsonReflect_) {
    for (ClassDecl *cls : sema_.classes_) {
      if (cls->reflectNew && !cls->isAbstract && classJsonProblem(cls).empty()) {
        cls->reflectJson = true;
      }
    }
  }
}

std::string ReflectChecker::classJsonProblem(const ClassDecl *cls) {
  auto *c = const_cast<ClassDecl *>(cls);
  if (auto found = jsonProblems_.find(cls); found != jsonProblems_.end()) {
    return found->second;
  }
  jsonProblems_[cls] = ""; // a class that holds itself, as it is being found
  std::string name = cls->type ? cls->type->name().str() : cls->name.str();
  std::string problem;
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls);
  if (cls->builtin || sema_.fsDeclarer().isRuntimeClass(cls)) {
    problem = "'" + name + "' is made only by the runtime";
  } else if (cls->isAbstract) {
    problem = "'" + name + "' is abstract, so no object is only a '" + name + "'";
  } else if (ctors.size() > 1) {
    problem = "'" + name + "' has several constructors, and fromJson calls the one a class has";
  }
  const FunctionDecl *ctor = ctors.empty() ? nullptr : ctors.front();
  c->jsonParams.clear();
  if (problem.empty() && ctor && ctor->sym) {
    sema_.returnInferrer().resolveParams(ctor->sym);
    // The key of each parameter is its name; those of `...args:
    // ConstructorArgs` have the names of the parameters they are up the
    // chain.
    std::function<std::string(const FunctionDecl *, size_t)> keyOf = [&](const FunctionDecl *fn,
                                                                         size_t k) {
      if (!fn->listName.empty() && k >= fn->listStart && fn->cls && fn->cls->parent) {
        std::vector<FunctionDecl *> up = ClassLinks::constructorsOf(fn->cls->parent);
        if (up.size() == 1 && k - fn->listStart < up[0]->params.size()) {
          return keyOf(up[0], k - fn->listStart);
        }
      }

      return fn->params[k].name.str();
    };
    for (size_t i = 0; i < ctor->sym->params.size() && i < ctor->params.size(); ++i) {
      c->jsonParams.push_back(keyOf(ctor, i));
    }
    for (size_t i = 0; i < ctor->sym->params.size() && problem.empty(); ++i) {
      const Type *type = ctor->sym->params[i];
      if (!type || (ctor->sym->hasRest && i + 1 == ctor->sym->params.size())) {
        problem = "its constructor takes a rest parameter, which no key of a Json names";
        break;
      }
      std::string why =
          JsonTypeChecker::jsonProblem(type, [&](const ClassDecl *inner) { return classJsonProblem(inner); });
      if (!why.empty()) {
        problem = "parameter '" + c->jsonParams[i] + "' of its constructor: " + why;
      }
    }
  }
  if (problem.empty()) {
    // The fields code may write that a Json can give, which the keys of the
    // Json set after the constructor ran.
    sema_.classDeclarer().completeClass(c);
    c->jsonFields.clear();
    for (const Field &f : c->type->fields()) {
      if (llvm::is_contained(c->jsonParams, f.name)) {
        continue;
      }
      const ClassDecl *owner = nullptr;
      const ClassField *field = ClassBases::findField(cls, f.name, &owner);
      if (!field || field->isStatic || field->method || f.readonly ||
          field->access != Access::Public || f.name[0] == '#') {
        continue;
      }
      if (JsonTypeChecker::jsonProblem(f.type, [&](const ClassDecl *inner) {
            return classJsonProblem(inner);
          }).empty()) {
        c->jsonFields.push_back(f.name);
      }
    }
  }
  jsonProblems_[cls] = problem;

  return problem;
}

Expr *ReflectChecker::checkReflection(CallExpr *call, MemberExpr *member, const Type *expected) {
  call->type = Type::error();
  llvm::StringRef fn = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
  };
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (fn == "get") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    // A field known when compiling: read as it is.
    auto *object = dyn_cast<NameExpr>(call->args[0]);
    auto *key = dyn_cast<StringLitExpr>(call->args[1]);
    Expr *&target = call->args[0];
    target = sema_.exprChecker().checkExpr(target);
    const Type *type = target->type;
    if (type->isError()) {
      call->args[1] = sema_.exprChecker().checkExpr(call->args[1]);
      return call;
    }
    // `T extends object` in a generic function: its copies take the classes.
    const Type *base = GenericChecker::apparentType(type->nonOptional());
    if (base->isJson()) {
      sema_.diags_.error(target->loc(), "Reflect.get takes an object of the language; read a key of a "
                                  "Json as JavaScript does, 'j[key]'");
      checkArgsLoosely();
      return call;
    }
    if (!type->isUnknown() && !UnknownChecker::isObjectReference(base) && !base->isAnyObject()) {
      sema_.diags_.error(target->loc(),
                   "Reflect.get takes an object, and this is a '" + type->name() + "'");
      call->args[1] = sema_.exprChecker().checkExpr(call->args[1]);
      return call;
    }
    // An object of a class known when compiling, also where a copy of a
    // generic function passes its T on as an unknown, as the generic code
    // does (see noteForced).
    Expr *seen = stripImplicit(target);
    const Type *seenType = seen->type;
    ClassDecl *cls = seenType->isObject() && !seenType->isAnyObject() ? ClassDeclarer::classOf(seenType) : nullptr;
    if (key && !seenType->isOptional() &&
        llvm::all_of(key->value, [](char16_t c) { return c < 128; })) {
      std::string name;
      for (char16_t c : key->value) {
        name.push_back(char(c));
      }
      if (cls) {
        sema_.classDeclarer().completeClass(cls);
      }
      const ClassField *declared = cls ? ClassBases::findField(cls, name) : nullptr;
      bool open = !cls || (declared && declared->access == Access::Public);
      // A field: read as it is.
      if (object && !type->isOptional() && base->isObject() && !base->isAnyObject() &&
          base->field(name) && open) {
        auto *read = sema_.ctx_.make<MemberExpr>(
            key->loc(), sema_.ctx_.make<NameExpr>(object->loc(), object->name), sema_.ctx_.keepName(name));
        return sema_.typeRelations().convert(sema_.exprChecker().checkExpr(read), Type::unknown());
      }
      if (cls && !cls->literal && seenType->field(name) && open) {
        auto *read = sema_.ctx_.make<MemberExpr>(key->loc(), seen, sema_.ctx_.keepName(name));
        return sema_.typeRelations().convert(sema_.classMemberChecker().checkClassMember(read, nullptr), Type::unknown());
      }
      // A method: its function, as the description would give it.
      bool several = false;
      if (FunctionDecl *method = cls && !cls->literal && !seenType->field(name)
                                     ? sema_.reflectCallChecker().knownReflectMethod(cls, name, several)
                                     : nullptr) {
        call->args.resize(1);
        target = seen;
        call->builtin = Builtin::ReflectMethod;
        call->method = method->sym;
        call->type = Type::unknown();
        if (several) {
          sema_.severalVariants_.insert(call);
        }
        return call;
      }
    }
    call->args[1] = sema_.argumentChecker().checkArg(call->args[1], Type::string());
    // Objects of these classes are looked into by name.
    if (onlyChecked()) {
      // Never emitted.
    } else if (type->isUnknown() || base->isAnyObject() || !base->classDecl()) {
      reflectGetAny_ = true;
    } else {
      classesIn(base, reflectedObjects_);
    }
    target = sema_.typeRelations().convert(target, Type::unknown());
    sema_.reflectionUsed_ |= !onlyChecked();
    call->builtin = Builtin::ReflectGet;
    call->type = Type::unknown();
    return call;
  }
  if (fn == "apply") {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 3, 3)) {
      return call;
    }
    Expr *&f = call->args[0];
    f = sema_.exprChecker().checkExpr(f);
    const FunctionDecl *held = sema_.reflectCallChecker().heldReflectMethod(f);
    if (!f->type->isError() && !f->type->isUnknown() && !f->type->isFunction()) {
      sema_.diags_.error(f->loc(),
                   "Reflect.apply calls a function, and this is a '" + f->type->name() + "'");
    } else {
      f = sema_.typeRelations().convert(f, Type::unknown());
    }
    Expr *&self = call->args[1];
    self = sema_.exprChecker().checkExpr(self);
    if (self->type->isVoid()) {
      sema_.diags_.error(self->loc(), "an expression of type 'void' has no value to pass as 'this'");
    } else {
      self = sema_.typeRelations().convert(self, Type::unknown());
    }
    call->args[2] = sema_.reflectCallChecker().checkReflectArgs(call->args[2]);
    sema_.flowChecker().calledFunction();
    if (held && !f->type->isError() && !self->type->isError() && !call->args[2]->type->isError()) {
      if (Expr *direct = sema_.reflectCallChecker().directReflectApply(call, held)) {
        return direct;
      }
    }
    if (!onlyChecked()) {
      sema_.reflectionUsed_ = true;
      reflectGetAny_ = true; // the method of an object it got by name
    }
    call->builtin = Builtin::ReflectApply;
    call->type = Type::unknown();
    return call;
  }
  // Reflect.construct(cls, args)
  if (call->args.size() == 3) {
    sema_.diags_.error(call->args[2]->loc(), "the third argument of Reflect.construct, the class whose "
                                       "prototype the object gets, is not supported: there are no "
                                       "prototypes in tz; make an object of that class");
    checkArgsLoosely();
    return call;
  }
  if (!sema_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
    return call;
  }
  // A class known when compiling, and the arguments written out: `new`.
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  Expr *&cls = call->args[0];
  cls = sema_.exprChecker().checkExpr(cls);
  const Type *type = cls->type;
  if (type->isError()) {
    call->args[1] = sema_.reflectCallChecker().checkReflectArgs(call->args[1]);
    return call;
  }
  if (!type->isClassRef()) {
    sema_.diags_.error(cls->loc(), "Reflect.construct makes an object of a class, and this is a '" +
                                 type->name() + "'");
    call->args[1] = sema_.reflectCallChecker().checkReflectArgs(call->args[1]);
    return call;
  }
  if (name && list && type->classRef() &&
      llvm::none_of(list->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
    std::vector<Expr *> args;
    for (const ArrayLitExpr::Element &e : list->elements) {
      args.push_back(e.expr);
    }
    auto *create = sema_.ctx_.make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
    create->callee = sema_.ctx_.make<NameExpr>(name->loc(), name->name);
    return sema_.classValueChecker().checkValueNew(create, expected);
  }
  // A class known when compiling, with arguments known only when the
  // program runs: the function of the class that checks them makes the
  // object, without its description. An abstract one fails when the program
  // runs, through the description, as JavaScript fails then.
  bool known = false;
  if (const ClassDecl *made = type->classRef()) {
    if (!made->type) {
      sema_.diags_.error(cls->loc(), "'" + made->name +
                                   "' of JavaScript cannot be made through "
                                   "reflection");
      call->args[1] = sema_.reflectCallChecker().checkReflectArgs(call->args[1]);
      return call;
    }
    known = !made->isAbstract;
    if (!known) {
      cls = sema_.typeRelations().convert(cls, Type::constructorOf(made->type));
    }
  }
  call->args[1] = sema_.reflectCallChecker().checkReflectArgs(call->args[1]);
  sema_.flowChecker().calledFunction();
  sema_.reflectionUsed_ |= !known && !onlyChecked();
  call->builtin = Builtin::ReflectConstruct;
  call->type = ClassValueChecker::constructedBy(cls->type);

  return call;
}

Expr *ReflectChecker::checkFromJsonClass(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  Expr *&cls = call->args[0];
  cls = sema_.exprChecker().checkExpr(cls);
  Expr *arg = sema_.exprChecker().checkExpr(call->args[1], Type::json());
  call->args[1] = sema_.typeRelations().coerce(arg, Type::json());
  const Type *type = cls->type;
  if (type->isError()) {
    return call;
  }
  if (!type->isClassRef()) {
    sema_.diags_.error(cls->loc(),
                 "fromJson(cls, j) makes an object of the class 'cls', and this is a '" +
                     type->name() + "'; to build a value of a type, write 'fromJson<" +
                     type->name() + ">(j)'");
    return call;
  }
  if (const ClassDecl *known = type->classRef()) {
    std::string problem = known->type ? classJsonProblem(known)
                                      : "'" + known->name.str() + "' is a class of JavaScript";
    if (!problem.empty()) {
      sema_.diags_.error(name->loc(),
                   "fromJson cannot make a '" + known->name.str() + "' from a Json: " + problem);
      return call;
    }
    call->type = known->type;
  } else {
    // Any class it may be, through its description.
    if (!onlyChecked()) {
      sema_.reflectionUsed_ = true;
      jsonReflect_ = true;
    }
    call->type = type->constructed();
  }
  call->builtin = Builtin::FromJsonClass;

  return call;
}

} // namespace tz
