#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// What a decorator gets where it stands, as TypeScript's __decorate passes it
// (docs/decorators.md, section 1): the class; the class and the name of a
// field; those and the descriptor of a method; or the class, the name of the
// method (undefined for a constructor) and the position of a parameter.
enum class Place { Class, Field, Method, Parameter };

size_t argumentsOf(Place place) {
  switch (place) {
  case Place::Class:
    return 1;
  case Place::Field:
    return 2;
  default:
    return 3;
  }
}

const char *placeText(Place place) {
  switch (place) {
  case Place::Class:
    return "a class";
  case Place::Field:
    return "a field";
  case Place::Method:
    return "a method";
  default:
    return "a parameter";
  }
}

const char *argumentsText(Place place) {
  switch (place) {
  case Place::Class:
    return "the class";
  case Place::Field:
    return "the class and the name of the field";
  case Place::Method:
    return "the class, the name of the method and its descriptor";
  default:
    return "the class, the name of the method (undefined for a constructor) and the position of "
           "the parameter";
  }
}

// `Log`, `Table`, `Reflect.metadata`: a decorator in messages, and what
// tells two apart.
std::string decoratorName(const Expr *expr) {
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    return decoratorName(call->callee);
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.starts_with("%") ? "" : name->name.str();
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::string object = decoratorName(member->object);
    return object.empty() ? "" : object + "." + member->name.str();
  }

  return "";
}

// A decorator used as it is, which no call makes: `@Log`, `@rules.required`.
bool isPlainReference(const Expr *expr) {
  if (isa<NameExpr>(expr)) {
    return true;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return isPlainReference(member->object);
  }

  return false;
}

bool decoratedParams(const FunctionDecl *fn) {
  return llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
}

} // namespace

DecoratorChecker::DecoratorChecker(Sema &sema) : sema_(sema) {}

const Type *DecoratorChecker::methodDescriptorType(const Type *args, const Type *ret) {
  const Type *value =
      Type::function({Type::unknown(), args}, 1, /*hasRest=*/true, ret, /*hasThis=*/true);
  const Type *flag = Type::boolTy();
  const Type *type =
      Type::object({{"value", value, false},
                    {"writable", flag, false},
                    {"enumerable", flag, false},
                    {"configurable", flag, false}},
                   "MethodDescriptor<" + args->name().str() + ", " + ret->name().str() + ">");
  sema_.descriptorOrigins_.try_emplace(type, args, ret);

  return type;
}

bool DecoratorChecker::decoratorParams(Expr *callee, std::vector<const Type *> &params) {
  const FuncSymbol *sym = nullptr;
  if (auto *name = dyn_cast<NameExpr>(callee)) {
    Sema::ScopeEntry entry = sema_.scopes().lookup(name->name);
    if (entry.var) {
      const Type *type = entry.var->type;
      if (!type || !type->isFunction() || type->hasRest()) {
        return false;
      }
      params.assign(type->params().begin(), type->params().end());
      return true;
    }
    sym = entry.func;
  } else if (auto *member = dyn_cast<MemberExpr>(callee)) {
    // A static method of a class.
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? sema_.scopes().lookup(object->name).cls : nullptr) {
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, /*isStatic=*/true);
      if (methods.size() == 1) {
        sym = methods[0]->sym;
      }
    }
  }
  if (!sym) {
    return false;
  }
  sema_.returnInferrer().resolveParams(const_cast<FuncSymbol *>(sym));
  if (sym->hasRest || llvm::is_contained(sym->params, nullptr)) {
    return false;
  }
  params = sym->params;

  return true;
}

void DecoratorChecker::checkDecoratedNames(ClassDecl *cls) {
  // The decorated members and the names of their decorators.
  struct Member {
    llvm::StringRef name;
    llvm::SMLoc loc;
    bool isStatic;
    std::vector<std::string> decorators;
  };
  std::vector<Member> members;
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc, bool isStatic,
                 llvm::ArrayRef<Decorator> decorators, const FunctionDecl *fn) {
    Member m{name, loc, isStatic, {}};
    for (const Decorator &d : decorators) {
      m.decorators.push_back(decoratorName(d.expr));
    }
    if (fn) {
      for (const Param &p : fn->params) {
        for (const Decorator &d : p.decorators) {
          m.decorators.push_back(decoratorName(d.expr));
        }
      }
    }
    if (!m.decorators.empty()) {
      members.push_back(std::move(m));
    }
  };
  for (const ClassField &f : cls->fields) {
    add(f.name, f.loc, f.isStatic, f.decorators, nullptr);
  }
  for (const FunctionDecl *fn : cls->methods) {
    add(fn->name, fn->loc(), fn->isStatic, fn->decorators, fn);
  }
  for (size_t i = 0; i < members.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      const Member &a = members[j], &b = members[i];
      if (a.name != b.name) {
        continue;
      }
      std::string what = a.isStatic != b.isStatic ? "both the static member '" + a.name.str() +
                                                        "' and the member '" + a.name.str() + "'"
                                                  : "two variants of '" + a.name.str() + "'";
      std::string shared;
      for (const std::string &d : b.decorators) {
        if (!d.empty() && llvm::is_contained(a.decorators, d)) {
          shared = d;
        }
      }
      std::string records = shared.empty() ? "the records 'design:*' the compiler writes"
                                           : "the records of '@" + shared + "'";
      sema_.diags_.error(b.loc, what + " of '" + cls->name.str() + "' are decorated, and " + records +
                              " for both would be one: metadata is kept on the class "
                              "itself, by the member's name (docs/decorators.md, section 3); "
                              "give one of them another name");
      sema_.diags_.note(a.loc, "the other one is here");
      break;
    }
  }
  // The decorators of the parameters of a constructor write on the class
  // itself, and the compiler writes its 'design:paramtypes' there: of two
  // constructors, those would be one (docs/overloads.md, section 6).
  const FunctionDecl *first = nullptr;
  for (const FunctionDecl *ctor : cls->constructors) {
    if (!decoratedParams(ctor)) {
      continue;
    }
    if (!first) {
      first = ctor;
      continue;
    }
    sema_.diags_.error(ctor->loc(), "two constructors of '" + cls->name.str() +
                                  "' have decorated parameters, and the record "
                                  "'design:paramtypes' the compiler writes for the class would be "
                                  "one: metadata is kept on the class itself (docs/decorators.md, "
                                  "section 3); decorate the parameters of one of them");
    sema_.diags_.note(first->loc(), "the other one is here");
    break;
  }
}

void DecoratorChecker::decorateClass(ClassDecl *cls) {
  checkDecoratedNames(cls);
  struct Member {
    ClassField *field;
    FunctionDecl *method;
    bool isStatic;
    const char *at;
  };
  std::vector<Member> members;
  for (ClassField &f : cls->fields) {
    if (!f.decorators.empty()) {
      members.push_back({&f, nullptr, f.isStatic, f.loc.getPointer()});
    }
  }
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->decorators.empty() || decoratedParams(fn)) {
      members.push_back({nullptr, fn, fn->isStatic, fn->loc().getPointer()});
    }
  }
  // As TypeScript applies them: the members of the instances, then the
  // static ones, each in the order written; then the class.
  llvm::stable_sort(members, [](const Member &a, const Member &b) {
    return std::make_pair(a.isStatic, a.at) < std::make_pair(b.isStatic, b.at);
  });
  for (Member &m : members) {
    std::vector<Sema::DecoratorUse> uses;
    for (const Decorator &d : m.field ? m.field->decorators : m.method->decorators) {
      uses.push_back({&d});
    }
    if (m.method) {
      for (size_t i = 0; i < m.method->params.size(); ++i) {
        for (const Decorator &d : m.method->params[i].decorators) {
          uses.push_back({&d, int(i), m.method});
        }
      }
    }
    decorate(cls, m.field, m.method, uses);
  }
  // The class gets the decorators of the parameters of its constructor.
  std::vector<Sema::DecoratorUse> uses;
  for (const Decorator &d : cls->decorators) {
    uses.push_back({&d});
  }
  for (const FunctionDecl *ctor : cls->constructors) {
    for (size_t i = 0; i < ctor->params.size(); ++i) {
      for (const Decorator &d : ctor->params[i].decorators) {
        uses.push_back({&d, int(i), ctor});
      }
    }
  }
  decorate(cls, nullptr, nullptr, uses);
}

void DecoratorChecker::decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                    llvm::ArrayRef<Sema::DecoratorUse> uses) {
  if (uses.empty()) {
    return;
  }
  std::vector<Stmt *> &out = cls->decorations;
  Place own = field ? Place::Field : method ? Place::Method : Place::Class;
  llvm::StringRef memberName = field ? field->name : method ? method->name : llvm::StringRef();
  llvm::SMLoc at = uses[0].decorator->loc;
  if (field && field->accessor) {
    sema_.diags_.error(at, "a decorator cannot decorate an 'accessor' field: in tz it is an ordinary "
                     "field, and reading or writing it never runs code");
    return;
  }
  if (method && method->isAbstract) {
    sema_.diags_.error(at, "a decorator cannot decorate an abstract method, which has no code; decorate "
                     "the methods that implement it");
    return;
  }
  if (memberName.starts_with("#")) {
    sema_.diags_.error(at, "a decorator cannot decorate a '#private' member, whose name is no key of "
                     "the class; make it 'private " +
                         memberName.drop_front(1) + "'");
    return;
  }

  // The decorators are evaluated top to bottom: one that a call makes is
  // made now, and applied later (from the bottom up). `@Reflect.metadata(k,
  // v)` writes its record when it is applied (null here).
  auto isMetadata = [&](const Expr *expr) {
    auto *call = dyn_cast<CallExpr>(expr);
    auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;

    return member && member->name == "metadata" && sema_.callChecker().isGlobalName(member->object, "Reflect");
  };
  std::vector<Expr *> callees;
  for (const Sema::DecoratorUse &use : uses) {
    Expr *expr = use.decorator->expr;
    if (isMetadata(expr)) {
      callees.push_back(nullptr);
      continue;
    }
    if (isPlainReference(expr)) {
      callees.push_back(expr);
      continue;
    }
    llvm::StringRef hidden = sema_.ctx_.hiddenName();
    auto *decl = sema_.ctx_.make<VarDeclStmt>(use.decorator->loc, /*isConst=*/true);
    VarDeclarator d;
    d.name = hidden;
    d.loc = use.decorator->loc;
    d.init = expr;
    decl->decls.push_back(std::move(d));
    sema_.stmtChecker().checkStmt(decl);
    out.push_back(decl);
    callees.push_back(sema_.ctx_.make<NameExpr>(use.decorator->loc, hidden));
  }

  // What each takes: as many arguments as it declares, up to what its place
  // gives. A decorator of a method that takes its descriptor may replace
  // what the method does: then every call goes through it.
  std::vector<size_t> takes;
  bool descriptor = false, ok = true;
  for (size_t i = 0; i < uses.size(); ++i) {
    Place place = uses[i].param >= 0 ? Place::Parameter : own;
    size_t given = argumentsOf(place);
    if (!callees[i]) {
      takes.push_back(0);
      continue;
    }
    std::vector<const Type *> params;
    bool known = decoratorParams(callees[i], params);
    size_t count = known ? params.size() : given;
    std::string name = decoratorName(uses[i].decorator->expr);
    std::string label = name.empty() ? "the decorator" : "'" + name + "'";
    llvm::SMLoc loc = uses[i].decorator->loc;
    const Type *third = known && params.size() >= 3 ? params[2]->nonOptional() : nullptr;
    if (count > given) {
      sema_.diags_.error(loc, label + " cannot decorate " + placeText(place) + ": it takes " +
                            std::to_string(count) + " arguments, and a decorator of " +
                            placeText(place) + " gets " + std::to_string(given) + ", " +
                            argumentsText(place));
      ok = false;
    } else if (third && place == Place::Parameter && third->isObject() && third->field("value")) {
      sema_.diags_.error(loc, label + " takes the descriptor of a method: it decorates methods, not "
                                "parameters");
      ok = false;
    } else if (third && place == Place::Method && third->isNumeric()) {
      sema_.diags_.error(loc, label + " takes the position of a parameter: it decorates parameters, not "
                                "methods");
      ok = false;
    }
    takes.push_back(std::min(count, given));
    descriptor |= place == Place::Method && count >= 3;
  }
  if (!ok) {
    return;
  }
  // A method of a generic class is a method of each of its instances, with
  // their own types: one descriptor cannot stand for them all.
  if (descriptor && cls->isGeneric()) {
    sema_.diags_.error(at, "a decorator that takes the descriptor of a method of a generic class is not "
                     "supported yet: each instance of '" +
                         cls->name +
                         "' has a method of its own; decorate a method of a class "
                         "that is not generic, or take only the class and the name");
    return;
  }

  // The descriptor: `value` is what the method runs, its own code until a
  // decorator replaces it (see codegen_decorate.cpp).
  VarSymbol *descVar = nullptr;
  const Type *descType = nullptr;
  if (descriptor) {
    FuncSymbol *sym = method->sym;
    sema_.returnInferrer().resolveParams(sym);
    const Type *ret = sema_.returnInferrer().returnType(sym);
    if (!ret || ret->isError() || llvm::is_contained(sym->params, nullptr)) {
      return;
    }
    descType = methodDescriptorType(Type::tuple(sym->params), ret);
    VarSymbol *impl = sema_.ctx_.newVar();
    impl->name = sema_.ctx_.keepName("%" + cls->name.str() + "." + method->name.str() + "." +
                               std::to_string(method->variant));
    impl->loc = method->loc();
    impl->type = descType->field("value")->type;
    impl->isGlobal = true;
    impl->usedInFunction = true;
    impl->reassigned = true;
    impl->declSeq = ++sema_.seq_;
    sema_.scopes().declare(impl->name, impl->loc, {impl});
    method->impl = impl;
    auto *lit = sema_.ctx_.make<ObjectLitExpr>(at);
    auto prop = [&](llvm::StringRef name, Expr *value) {
      ObjectLitExpr::Property p;
      p.name = name;
      p.loc = at;
      p.value = value;
      lit->props.push_back(p);
    };
    prop("value", sema_.ctx_.make<NameExpr>(at, impl->name));
    prop("writable", sema_.ctx_.make<BoolLitExpr>(at, true));
    prop("enumerable", sema_.ctx_.make<BoolLitExpr>(at, false));
    prop("configurable", sema_.ctx_.make<BoolLitExpr>(at, true));
    Expr *init = sema_.typeRelations().coerce(sema_.exprChecker().checkExpr(lit, descType), descType);
    descVar = sema_.scopes().declareVar(sema_.ctx_.hiddenName(), at, descType, /*isConst=*/false);
    descVar->reassigned = true;
    auto *decl = sema_.ctx_.make<VarDeclStmt>(at, /*isConst=*/false);
    VarDeclarator d;
    d.name = descVar->name;
    d.loc = at;
    d.init = init;
    d.sym = descVar;
    decl->decls.push_back(std::move(d));
    out.push_back(decl);
  }

  // The records the compiler writes come first: TypeScript puts them last
  // among the decorators, which it applies from the bottom up. Those of the
  // class are of its constructor (with several, of the one whose
  // parameters are decorated).
  const FunctionDecl *ctor = nullptr;
  if (own == Place::Class) {
    for (const FunctionDecl *c : cls->constructors) {
      if (decoratedParams(c) || cls->constructors.size() == 1) {
        ctor = ctor ? nullptr : c;
      }
    }
  }
  sema_.metadataChecker().writeDesignRecords(cls, field, method, ctor, at);

  // Applied from the bottom up, the parameters' first. A class decorator
  // gets the class the one below it gave, if any.
  ClassDecl *current = cls;
  for (size_t i = uses.size(); i-- > 0;) {
    const Sema::DecoratorUse &use = uses[i];
    llvm::SMLoc loc = use.decorator->loc;
    Place place = use.param >= 0 ? Place::Parameter : own;
    if (!callees[i]) {
      // `@Reflect.metadata(key, value)`: Reflect.defineMetadata(key, value,
      // cls, name) right here.
      auto *metadata = cast<CallExpr>(use.decorator->expr);
      if (metadata->args.size() != 2 || metadata->hasSpread()) {
        sema_.diags_.error(loc, "'Reflect.metadata' takes the key and the value of the record, e.g. "
                          "'@Reflect.metadata(Role, \"admin\")'");
        continue;
      }
      auto *callee =
          sema_.ctx_.make<MemberExpr>(loc, sema_.ctx_.make<NameExpr>(loc, "Reflect"), "defineMetadata");
      std::vector<Expr *> args{metadata->args[0], metadata->args[1], sema_.classValueChecker().classValueAt(cls, loc)};
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (place != Place::Class && !(use.param >= 0 && use.owner->isConstructor)) {
        args.push_back(sema_.ctx_.make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
      }
      out.push_back(sema_.ctx_.make<ExprStmt>(loc, sema_.exprChecker().checkExpr(sema_.ctx_.make<CallExpr>(loc, callee, args))));
      continue;
    }
    std::vector<Expr *> args{sema_.classValueChecker().classValueAt(place == Place::Class ? current : cls, loc)};
    if (place != Place::Class) {
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (use.param >= 0 && use.owner->isConstructor) {
        args.push_back(sema_.ctx_.make<UndefinedExpr>(loc));
      } else {
        args.push_back(sema_.ctx_.make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
      }
    }
    if (place == Place::Parameter) {
      args.push_back(sema_.ctx_.make<IntLitExpr>(loc, uint64_t(use.param)));
    } else if (place == Place::Method && descVar) {
      args.push_back(sema_.ctx_.make<NameExpr>(loc, descVar->name));
    }
    args.resize(std::min(args.size(), takes[i]));
    auto *call = sema_.ctx_.make<CallExpr>(loc, callees[i], std::move(args));
    Expr *applied = sema_.exprChecker().checkExpr(call);
    const Type *result = applied->type;
    if (descVar && place == Place::Method && result == descType) {
      // `return descriptor`: what it gives back is the descriptor from now on.
      auto *target = sema_.ctx_.make<NameExpr>(loc, descVar->name);
      target->var = descVar;
      target->type = descType;
      auto *assign = sema_.ctx_.make<AssignExpr>(loc, std::nullopt, target, applied);
      assign->type = descType;
      applied = assign;
    } else if (place == Place::Class && result->classRef() &&
               result->classRef()->derivesFrom(current)) {
      // A class that extends the one it got, or that one: the name means it
      // from here on (docs/decorators.md, section 5).
      if (ClassDecl *given = const_cast<ClassDecl *>(result->classRef()); given != current) {
        sema_.lateClasses().replaceClass(current, given, loc);
        current = given;
      }
    } else if (!result->isVoid() && !result->isUndefined() && !result->isError()) {
      std::string name = decoratorName(use.decorator->expr);
      std::string label = name.empty() ? "the decorator" : "'" + name + "'";
      if (place == Place::Class && result->isConstructor()) {
        sema_.diags_.error(loc, label + " returns '" + result->name().str() +
                              "', some class known only when the program runs, and the class it "
                              "gives in place of '" +
                              current->name.str() +
                              "' must be known when compiling: make the decorator generic, "
                              "'<T extends Constructor>(cls: T)', and return 'cls' or a class "
                              "that extends it");
      } else if (place == Place::Class) {
        sema_.diags_.error(loc, label + " returns '" + result->name().str() +
                              "': a class decorator gives back nothing, the class it got, or a "
                              "class that extends it, which then replaces it");
      } else if (place == Place::Method && descVar) {
        sema_.diags_.error(loc, label + " returns '" + result->name().str() +
                              "': a decorator of a method returns nothing, or the descriptor "
                              "it got");
      } else {
        sema_.diags_.error(loc, label + " returns '" + result->name().str() + "', which a decorator of " +
                              placeText(place) + " cannot give back; return nothing");
      }
    }
    out.push_back(sema_.ctx_.make<ExprStmt>(loc, applied));
  }
  if (descVar) {
    // What the calls of the method run from now on.
    auto *target = sema_.ctx_.make<NameExpr>(at, method->impl->name);
    auto *value = sema_.ctx_.make<MemberExpr>(at, sema_.ctx_.make<NameExpr>(at, descVar->name), "value");
    auto *assign = sema_.ctx_.make<AssignExpr>(at, std::nullopt, target, value);
    out.push_back(sema_.ctx_.make<ExprStmt>(at, sema_.exprChecker().checkExpr(assign)));
  }
}

} // namespace tz
