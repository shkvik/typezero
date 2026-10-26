#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

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

}

DecoratorChecker::DecoratorChecker(Checker &checker) : checker_(checker) {}

const Type *DecoratorChecker::methodDescriptorType(const Type *args, const Type *ret) {
  const Type *value = Type::function({Type::unknown(), args}, 1, true, ret, true);
  const Type *flag = Type::boolTy();
  const Type *type =
      Type::object({{"value", value, false},
                    {"writable", flag, false},
                    {"enumerable", flag, false},
                    {"configurable", flag, false}},
                   "MethodDescriptor<" + args->name().str() + ", " + ret->name().str() + ">");
  checker_.descriptorOrigins_.try_emplace(type, args, ret);

  return type;
}

bool DecoratorChecker::decoratorParams(Expr *callee, std::vector<const Type *> &params) {
  const FuncSymbol *sym = nullptr;
  if (auto *name = dyn_cast<NameExpr>(callee)) {
    Checker::ScopeEntry entry = checker_.scopes().lookup(name->name);
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
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? checker_.scopes().lookup(object->name).cls : nullptr) {
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, true);
      if (methods.size() == 1) {
        sym = methods[0]->sym;
      }
    }
  }
  if (!sym) {
    return false;
  }
  checker_.returnInferrer().resolveParams(const_cast<FuncSymbol *>(sym));
  if (sym->hasRest || llvm::is_contained(sym->params, nullptr)) {
    return false;
  }
  params = sym->params;

  return true;
}

void DecoratorChecker::checkDecoratedNames(ClassDecl *cls) {
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
      checker_.diags_.error(b.loc,
                            what + " of '" + cls->name.str() + "' are decorated, and " + records +
                                " for both would be one: metadata is kept on the class "
                                "itself, by the member's name (docs/decorators.md, section 3); "
                                "give one of them another name");
      checker_.diags_.note(a.loc, "the other one is here");
      break;
    }
  }
  const FunctionDecl *first = nullptr;
  for (const FunctionDecl *ctor : cls->constructors) {
    if (!decoratedParams(ctor)) {
      continue;
    }
    if (!first) {
      first = ctor;
      continue;
    }
    checker_.diags_.error(ctor->loc(),
                          "two constructors of '" + cls->name.str() +
                              "' have decorated parameters, and the record "
                              "'design:paramtypes' the compiler writes for the class would be "
                              "one: metadata is kept on the class itself (docs/decorators.md, "
                              "section 3); decorate the parameters of one of them");
    checker_.diags_.note(first->loc(), "the other one is here");
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
  llvm::stable_sort(members, [](const Member &a, const Member &b) {
    return std::make_pair(a.isStatic, a.at) < std::make_pair(b.isStatic, b.at);
  });
  for (Member &m : members) {
    std::vector<Checker::DecoratorUse> uses;
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
  std::vector<Checker::DecoratorUse> uses;
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
                                llvm::ArrayRef<Checker::DecoratorUse> uses) {
  if (uses.empty()) {
    return;
  }
  std::vector<Stmt *> &out = cls->decorations;
  Place own = field ? Place::Field : method ? Place::Method : Place::Class;
  llvm::StringRef memberName = field ? field->name : method ? method->name : llvm::StringRef();
  llvm::SMLoc at = uses[0].decorator->loc;
  if (field && field->accessor) {
    checker_.diags_.error(
        at, "a decorator cannot decorate an 'accessor' field: in tz it is an ordinary "
            "field, and reading or writing it never runs code");
    return;
  }
  if (method && method->isAbstract) {
    checker_.diags_.error(
        at, "a decorator cannot decorate an abstract method, which has no code; decorate "
            "the methods that implement it");
    return;
  }
  if (memberName.starts_with("#")) {
    checker_.diags_.error(
        at, "a decorator cannot decorate a '#private' member, whose name is no key of "
            "the class; make it 'private " +
                memberName.drop_front(1) + "'");
    return;
  }

  auto isMetadata = [&](const Expr *expr) {
    auto *call = dyn_cast<CallExpr>(expr);
    auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;

    return member && member->name == "metadata" &&
           checker_.callChecker().isGlobalName(member->object, "Reflect");
  };
  std::vector<Expr *> callees;
  for (const Checker::DecoratorUse &use : uses) {
    Expr *expr = use.decorator->expr;
    if (isMetadata(expr)) {
      callees.push_back(nullptr);
      continue;
    }
    if (isPlainReference(expr)) {
      callees.push_back(expr);
      continue;
    }
    llvm::StringRef hidden = checker_.ctx_.hiddenName();
    auto *decl = checker_.ctx_.make<VarDeclStmt>(use.decorator->loc, true);
    VarDeclarator d;
    d.name = hidden;
    d.loc = use.decorator->loc;
    d.init = expr;
    decl->decls.push_back(std::move(d));
    checker_.stmtChecker().checkStmt(decl);
    out.push_back(decl);
    callees.push_back(checker_.ctx_.make<NameExpr>(use.decorator->loc, hidden));
  }

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
      checker_.diags_.error(loc, label + " cannot decorate " + placeText(place) + ": it takes " +
                                     std::to_string(count) + " arguments, and a decorator of " +
                                     placeText(place) + " gets " + std::to_string(given) + ", " +
                                     argumentsText(place));
      ok = false;
    } else if (third && place == Place::Parameter && third->isObject() && third->field("value")) {
      checker_.diags_.error(loc, label +
                                     " takes the descriptor of a method: it decorates methods, not "
                                     "parameters");
      ok = false;
    } else if (third && place == Place::Method && third->isNumeric()) {
      checker_.diags_.error(
          loc, label + " takes the position of a parameter: it decorates parameters, not "
                       "methods");
      ok = false;
    }
    takes.push_back(std::min(count, given));
    descriptor |= place == Place::Method && count >= 3;
  }
  if (!ok) {
    return;
  }
  if (descriptor && cls->isGeneric()) {
    checker_.diags_.error(
        at, "a decorator that takes the descriptor of a method of a generic class is not "
            "supported yet: each instance of '" +
                cls->name +
                "' has a method of its own; decorate a method of a class "
                "that is not generic, or take only the class and the name");
    return;
  }

  VarSymbol *descVar = nullptr;
  const Type *descType = nullptr;
  if (descriptor) {
    FuncSymbol *sym = method->sym;
    checker_.returnInferrer().resolveParams(sym);
    const Type *ret = checker_.returnInferrer().returnType(sym);
    if (!ret || ret->isError() || llvm::is_contained(sym->params, nullptr)) {
      return;
    }
    descType = methodDescriptorType(Type::tuple(sym->params), ret);
    VarSymbol *impl = checker_.ctx_.newVar();
    impl->name = checker_.ctx_.keepName("%" + cls->name.str() + "." + method->name.str() + "." +
                                        std::to_string(method->variant));
    impl->loc = method->loc();
    impl->type = descType->field("value")->type;
    impl->isGlobal = true;
    impl->usedInFunction = true;
    impl->reassigned = true;
    impl->declSeq = ++checker_.seq_;
    checker_.scopes().declare(impl->name, impl->loc, {impl});
    method->impl = impl;
    auto *lit = checker_.ctx_.make<ObjectLitExpr>(at);
    auto prop = [&](llvm::StringRef name, Expr *value) {
      ObjectLitExpr::Property p;
      p.name = name;
      p.loc = at;
      p.value = value;
      lit->props.push_back(p);
    };
    prop("value", checker_.ctx_.make<NameExpr>(at, impl->name));
    prop("writable", checker_.ctx_.make<BoolLitExpr>(at, true));
    prop("enumerable", checker_.ctx_.make<BoolLitExpr>(at, false));
    prop("configurable", checker_.ctx_.make<BoolLitExpr>(at, true));
    Expr *init =
        checker_.typeRelations().coerce(checker_.exprChecker().checkExpr(lit, descType), descType);
    descVar = checker_.scopes().declareVar(checker_.ctx_.hiddenName(), at, descType, false);
    descVar->reassigned = true;
    auto *decl = checker_.ctx_.make<VarDeclStmt>(at, false);
    VarDeclarator d;
    d.name = descVar->name;
    d.loc = at;
    d.init = init;
    d.sym = descVar;
    decl->decls.push_back(std::move(d));
    out.push_back(decl);
  }

  const FunctionDecl *ctor = nullptr;
  if (own == Place::Class) {
    for (const FunctionDecl *c : cls->constructors) {
      if (decoratedParams(c) || cls->constructors.size() == 1) {
        ctor = ctor ? nullptr : c;
      }
    }
  }
  checker_.metadataChecker().writeDesignRecords(cls, field, method, ctor, at);

  ClassDecl *current = cls;
  for (size_t i = uses.size(); i-- > 0;) {
    const Checker::DecoratorUse &use = uses[i];
    llvm::SMLoc loc = use.decorator->loc;
    Place place = use.param >= 0 ? Place::Parameter : own;
    if (!callees[i]) {
      auto *metadata = cast<CallExpr>(use.decorator->expr);
      if (metadata->args.size() != 2 || metadata->hasSpread()) {
        checker_.diags_.error(loc,
                              "'Reflect.metadata' takes the key and the value of the record, e.g. "
                              "'@Reflect.metadata(Role, \"admin\")'");
        continue;
      }
      auto *callee = checker_.ctx_.make<MemberExpr>(
          loc, checker_.ctx_.make<NameExpr>(loc, "Reflect"), "defineMetadata");
      std::vector<Expr *> args{metadata->args[0], metadata->args[1],
                               checker_.classValueChecker().classValueAt(cls, loc)};
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (place != Place::Class && !(use.param >= 0 && use.owner->isConstructor)) {
        args.push_back(
            checker_.ctx_.make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
      }
      out.push_back(checker_.ctx_.make<ExprStmt>(
          loc, checker_.exprChecker().checkExpr(checker_.ctx_.make<CallExpr>(loc, callee, args))));
      continue;
    }
    std::vector<Expr *> args{
        checker_.classValueChecker().classValueAt(place == Place::Class ? current : cls, loc)};
    if (place != Place::Class) {
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (use.param >= 0 && use.owner->isConstructor) {
        args.push_back(checker_.ctx_.make<UndefinedExpr>(loc));
      } else {
        args.push_back(
            checker_.ctx_.make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
      }
    }
    if (place == Place::Parameter) {
      args.push_back(checker_.ctx_.make<IntLitExpr>(loc, uint64_t(use.param)));
    } else if (place == Place::Method && descVar) {
      args.push_back(checker_.ctx_.make<NameExpr>(loc, descVar->name));
    }
    args.resize(std::min(args.size(), takes[i]));
    auto *call = checker_.ctx_.make<CallExpr>(loc, callees[i], std::move(args));
    Expr *applied = checker_.exprChecker().checkExpr(call);
    const Type *result = applied->type;
    if (descVar && place == Place::Method && result == descType) {
      auto *target = checker_.ctx_.make<NameExpr>(loc, descVar->name);
      target->var = descVar;
      target->type = descType;
      auto *assign = checker_.ctx_.make<AssignExpr>(loc, std::nullopt, target, applied);
      assign->type = descType;
      applied = assign;
    } else if (place == Place::Class && result->classRef() &&
               result->classRef()->derivesFrom(current)) {
      if (ClassDecl *given = const_cast<ClassDecl *>(result->classRef()); given != current) {
        checker_.lateClasses().replaceClass(current, given, loc);
        current = given;
      }
    } else if (!result->isVoid() && !result->isUndefined() && !result->isError()) {
      std::string name = decoratorName(use.decorator->expr);
      std::string label = name.empty() ? "the decorator" : "'" + name + "'";
      if (place == Place::Class && result->isConstructor()) {
        checker_.diags_.error(
            loc, label + " returns '" + result->name().str() +
                     "', some class known only when the program runs, and the class it "
                     "gives in place of '" +
                     current->name.str() +
                     "' must be known when compiling: make the decorator generic, "
                     "'<T extends Constructor>(cls: T)', and return 'cls' or a class "
                     "that extends it");
      } else if (place == Place::Class) {
        checker_.diags_.error(loc,
                              label + " returns '" + result->name().str() +
                                  "': a class decorator gives back nothing, the class it got, or a "
                                  "class that extends it, which then replaces it");
      } else if (place == Place::Method && descVar) {
        checker_.diags_.error(loc,
                              label + " returns '" + result->name().str() +
                                  "': a decorator of a method returns nothing, or the descriptor "
                                  "it got");
      } else {
        checker_.diags_.error(loc, label + " returns '" + result->name().str() +
                                       "', which a decorator of " + placeText(place) +
                                       " cannot give back; return nothing");
      }
    }
    out.push_back(checker_.ctx_.make<ExprStmt>(loc, applied));
  }
  if (descVar) {
    auto *target = checker_.ctx_.make<NameExpr>(at, method->impl->name);
    auto *value = checker_.ctx_.make<MemberExpr>(
        at, checker_.ctx_.make<NameExpr>(at, descVar->name), "value");
    auto *assign = checker_.ctx_.make<AssignExpr>(at, std::nullopt, target, value);
    out.push_back(checker_.ctx_.make<ExprStmt>(at, checker_.exprChecker().checkExpr(assign)));
  }
}

}
