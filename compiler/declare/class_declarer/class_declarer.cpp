#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_check_helpers/class_check_helpers.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

ClassDeclarer::ClassDeclarer(Checker &checker) : checker_(checker) {}

ClassDecl *ClassDeclarer::classOf(const Type *type) {
  return type ? const_cast<ClassDecl *>(type->classDecl()) : nullptr;
}

std::vector<FunctionDecl *> ClassDeclarer::methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                                        bool isStatic) {
  std::vector<FunctionDecl *> found;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<FunctionDecl *> own;
    for (FunctionDecl *fn : (isStatic ? c->staticOwner() : c)->methods) {
      if (fn->name == name && fn->isStatic == isStatic) {
        own.push_back(fn);
      }
    }
    if (!isStatic) {
      for (const ClassField &f : c->fields) {
        if (f.method && f.name == name) {
          own.push_back(f.method);
        }
      }
    }
    for (FunctionDecl *fn : own) {
      bool replaced = llvm::any_of(found, [&](const FunctionDecl *v) {
        if (!isStatic) {
          return ClassLinks::overridesTransitively(v, fn);
        }
        const FuncSymbol *a = v->sym, *b = fn->sym;

        return a && b && a->params == b->params && a->minArgs == b->minArgs &&
               a->hasRest == b->hasRest;
      });
      if (!replaced) {
        found.push_back(fn);
      }
    }
  }

  return found;
}

void ClassDeclarer::declareClass(ClassDecl *cls) {
  cls->id = ++checker_.classCount_;
  cls->type = Type::classType(cls->name, cls);
  checker_.classes_.push_back(cls);
  checker_.scopes().declare(cls->name, cls->loc(), {nullptr, nullptr, nullptr, cls});
}

void ClassDeclarer::resolveClass(ClassDecl *cls) {
  if (cls->resolved) {
    return;
  }
  cls->resolved = true;
  checker_.genericClassChecker().declareClassTypeParams(cls);
  checker_.typeScopes_.emplace_back();
  checker_.genericClassChecker().bindClassTypeParams(cls);
  checker_.classBases().resolveClassBases(cls);
  for (size_t i = 0; i < cls->fields.size(); ++i) {
    ClassField &f = cls->fields[i];
    for (size_t j = 0; j < i; ++j) {
      if (cls->fields[j].name == f.name && cls->fields[j].isStatic == f.isStatic) {
        checker_.diags_.error(f.loc,
                              "duplicate member '" + f.name + "' in class '" + cls->name + "'");
        checker_.diags_.note(cls->fields[j].loc, "previously declared here");
        break;
      }
    }
    for (FunctionDecl *fn : cls->methods) {
      if (fn->name == f.name && fn->isStatic == f.isStatic) {
        checker_.diags_.error(fn->loc(), "duplicate member '" + f.name + "' in class '" +
                                             cls->name +
                                             "': a field and a method cannot share a name");
        checker_.diags_.note(f.loc, "the field is declared here");
      }
    }
  }

  for (ClassField &f : cls->fields) {
    if (f.definite) {
      checker_.diags_.error(f.loc,
                            "'" + f.name +
                                "!' (\"set later\") is not supported: a field gets its "
                                "value where it is declared or in the constructor; if it may "
                                "have none, make it optional: '" +
                                f.name + "?: T'");
    }
    const Type *type = nullptr;
    if (f.parameter) {
      const Param *param = nullptr;
      for (FunctionDecl *ctor : cls->constructors) {
        for (const Param &p : ctor->params) {
          if (p.property && p.name == f.name && !param) {
            param = &p;
          }
        }
      }
      f.optional = param->optional;
      if (param->type) {
        type = checker_.typeResolver().resolveType(*param->type, false);
      } else if (param->defaultValue) {
        type = literalType(param->defaultValue);
      }
    } else if (f.type) {
      type = checker_.typeResolver().resolveType(*f.type, false);
    } else if (f.init) {
      type = literalType(f.init);
    } else {
      checker_.diags_.error(f.loc,
                            "field '" + f.name + "' needs a type, e.g. '" + f.name + ": int'");
      type = Type::error();
    }
    if (type && f.optional) {
      type = Type::optional(type);
    }
    f.resolved = type;

    auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(f.init);
    if (fe && fe->fn->name.empty()) {
      fe->fn->name = f.name;
    }
    if (fe && fe->fn->isArrow && !f.isStatic && !f.parameter && !f.optional && !f.redeclared &&
        !checker_.assignedFields_.contains(f.name) && (!type || type->isFunction())) {
      FunctionDecl *fn = fe->fn;
      fn->name = f.name;
      fn->cls = cls;
      fn->access = f.access;
      fn->isOverride = f.isOverride;
      fn->isExpression = false;
      f.method = fn;
      f.resolved = nullptr;
    }

    if (f.isStatic && type && !cls->typeArgs.empty() && mentionsParam(type)) {
      checker_.diags_.error(f.loc, "static field '" + f.name +
                                       "' cannot use the type parameters of '" + cls->name +
                                       "': every instance of the class shares it");
      f.resolved = type = Type::error();
    }
    if (f.isStatic) {
      VarSymbol *var = checker_.ctx_.newVar();
      var->name = checker_.ctx_.keepName((cls->name + "." + f.name).str());
      var->loc = f.loc;
      var->type = type;
      var->isConst = f.readonly;
      var->isGlobal = true;
      var->usedInFunction = true;
      var->declSeq = ++checker_.seq_;
      f.var = var;
      if (!f.init && !f.optional && type && !type->isError()) {
        checker_.diags_.error(f.loc, "static field '" + f.name + "' needs a value, e.g. 'static " +
                                         f.name + ": " + type->name() +
                                         " = ...', or make it optional: 'static " + f.name +
                                         "?: " + type->name() + "'");
      }
    }
  }
  checker_.typeScopes_.pop_back();
}

void ClassDeclarer::declareTyped(FunctionDecl *fn, const Type *declared) {
  FuncSymbol *sym = checker_.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *given = i < declared->params().size() ? declared->params()[i] : nullptr;
    const Type *type = param.type ? checker_.typeResolver().resolveType(*param.type, false) : given;
    if (!type) {
      checker_.diags_.error(param.loc, "parameter '" + param.label() +
                                           "' takes more arguments than '" + declared->name() +
                                           "' passes");
      type = Type::error();
    }
    if (param.optional && !type->isOptional()) {
      type = Type::optional(type);
    }
    param.declaredType = param.defaultValue ? type->nonOptional() : type;
    sym->params.push_back(type);
  }
  for (size_t i = fn->params.size(); i < declared->params().size(); ++i) {
    Param hidden;
    hidden.name = checker_.ctx_.hiddenName();
    hidden.loc = fn->loc();
    hidden.declaredType = declared->params()[i];
    fn->params.push_back(hidden);
    sym->params.push_back(declared->params()[i]);
  }
  sym->minArgs = declared->minArgs();
  sym->hasRest = declared->hasRest();
  sym->ret = declared->ret();
  sym->discardsResult = declared->ret()->isVoid();
}

void ClassDeclarer::declareMethods(ClassDecl *cls) {
  for (FunctionDecl *ctor : cls->constructors) {
    checker_.functionChecker().declareFunction(ctor, nullptr, false);
    ctor->sym->ret = Type::voidTy();
  }
  for (FunctionDecl *fn : cls->methods) {
    checker_.functionChecker().declareFunction(fn, nullptr, false);
    if (fn->isAbstract && !fn->returnType && cls->isGeneric()) {
      checker_.diags_.error(fn->loc(), "abstract method '" + fn->name + "' of generic class '" +
                                           cls->name + "' needs a return type, e.g. '): T'");
    } else if (fn->isAbstract && !fn->returnType) {
      fn->sym->ret = nullptr;
    }
    if (!fn->typeParams.empty() && (fn->isAbstract || fn->isOverride)) {
      checker_.diags_.error(fn->loc(), std::string("a generic method cannot be ") +
                                           (fn->isAbstract ? "abstract" : "an override") +
                                           ": each call runs a copy of it made for its types");
    }
  }
  for (ClassField &f : cls->fields) {
    if (!f.method) {
      continue;
    }
    FunctionDecl *fn = f.method;
    checker_.typeScopes_.emplace_back();
    checker_.genericClassChecker().bindClassTypeParams(cls);
    const Type *declared = f.type ? checker_.typeResolver().resolveType(*f.type, false) : nullptr;
    checker_.typeScopes_.pop_back();
    if (!declared || !declared->isFunction()) {
      checker_.functionChecker().declareFunction(fn, nullptr, false);
      continue;
    }
    declareTyped(fn, declared);
  }

  std::vector<Stmt *> sets;
  for (const ClassField &f : cls->fields) {
    if (f.isStatic || !f.init || f.method) {
      continue;
    }
    auto *target =
        checker_.ctx_.make<MemberExpr>(f.loc, checker_.ctx_.make<NameExpr>(f.loc, "this"), f.name);
    auto *set = checker_.ctx_.make<AssignExpr>(f.loc, std::nullopt, target, f.init);
    sets.push_back(checker_.ctx_.make<ExprStmt>(f.loc, set));
  }
  if (!sets.empty()) {
    auto *init = checker_.ctx_.make<FunctionDecl>(cls->loc(), "init");
    init->cls = cls;
    init->body = checker_.ctx_.make<BlockStmt>(cls->loc());
    init->body->body = std::move(sets);
    checker_.functionChecker().declareFunction(init, nullptr, false);
    init->sym->ret = Type::voidTy();
    cls->init = init;
  }

  auto number = [](llvm::ArrayRef<FunctionDecl *> fns) {
    for (size_t i = 0; i < fns.size(); ++i) {
      for (size_t j = 0; j < i; ++j) {
        if (fns[j]->name == fns[i]->name && fns[j]->isStatic == fns[i]->isStatic) {
          fns[i]->variant = fns[j]->variant + 1;
        }
      }
    }
  };
  number(cls->methods);
  number(cls->constructors);
  checkVariants(cls);
}

void ClassDeclarer::checkVariants(ClassDecl *cls) {
  auto check = [&](llvm::ArrayRef<FunctionDecl *> fns) {
    for (size_t i = 0; i < fns.size(); ++i) {
      for (size_t j = 0; j < i; ++j) {
        if (fns[j]->name == fns[i]->name && fns[j]->isStatic == fns[i]->isStatic) {
          checkVariantPair(fns[j], fns[i]);
        }
      }
    }
  };
  check(cls->methods);
  check(cls->constructors);
}

void ClassDeclarer::checkVariantPair(FunctionDecl *a, FunctionDecl *b) {
  if (!a->sym || !b->sym) {
    return;
  }
  const FuncSymbol *x = a->sym, *y = b->sym;
  if (llvm::is_contained(x->params, nullptr) || llvm::is_contained(y->params, nullptr)) {
    return;
  }
  std::string what = a->isConstructor ? "the constructor" : "'" + a->name.str() + "'";
  if (x->params == y->params && x->minArgs == y->minArgs && x->hasRest == y->hasRest) {
    if (x->ret && y->ret && x->ret != y->ret) {
      checker_.diags_.error(b->loc(), "variants of " + what +
                                          " cannot differ only in what they return: a call "
                                          "chooses a variant by its arguments");
    } else {
      checker_.diags_.error(b->loc(), "duplicate variant " + variantText(b) +
                                          ": variants must differ in their parameters");
    }
    checker_.diags_.note(a->loc(), "the other variant is here");
    return;
  }
  size_t fixedX = x->params.size() - x->hasRest, fixedY = y->params.size() - y->hasRest;
  for (size_t n = std::max(x->minArgs, y->minArgs); n <= std::min(fixedX, fixedY); ++n) {
    bool same = true;
    for (size_t k = 0; k < n && same; ++k) {
      same = x->params[k] == y->params[k];
    }
    if (!same) {
      continue;
    }
    checker_.diags_.error(b->loc(), "variants " + variantText(a) + " and " + variantText(b) +
                                        " fit a call with " + std::to_string(n) + " argument" +
                                        (n == 1 ? "" : "s") +
                                        " equally well; give them parameters of different types");
    checker_.diags_.note(a->loc(), "the other variant is here");
    break;
  }
}

void ClassDeclarer::completeClass(ClassDecl *cls) {
  auto *type = const_cast<Type *>(cls->type);
  if (!type->isIncomplete() || cls->literal || !completing_.insert(cls).second) {
    return;
  }
  if (cls->parent) {
    completeClass(cls->parent);
  }
  bool muted = false;
  if (ClassDecl *generic = cls->generic) {
    completeClass(generic);
    for (size_t i = 0; i < cls->fields.size() && i < generic->fields.size(); ++i) {
      if (!cls->fields[i].isStatic && !cls->fields[i].method && !cls->fields[i].resolved) {
        cls->fields[i].resolved =
            generic->fields[i].resolved
                ? checker_.genericClassChecker().instanceType(cls, generic->fields[i].resolved)
                : Type::error();
      }
    }
    muted = checker_.diags_.setMuted(true);
  }
  for (ClassField &f : cls->fields) {
    if (!f.parameter || f.resolved) {
      continue;
    }
    for (FunctionDecl *ctor : cls->constructors) {
      if (f.resolved || !ctor->sym) {
        continue;
      }
      checker_.returnInferrer().resolveParams(ctor->sym);
      for (const Param &param : ctor->params) {
        if (param.property && param.name == f.name && param.declaredType) {
          f.resolved = param.declaredType;
        }
      }
    }
    if (!f.resolved) {
      f.resolved = Type::error();
    }
  }
  bool pending = llvm::any_of(
      cls->fields, [](const ClassField &f) { return !f.isStatic && !f.method && !f.resolved; });
  if (pending && cls->init && !cls->init->checked) {
    Checker::BodyState saved = checker_.returnInferrer().enterBody(cls->init);
    checker_.flow_ = Checker::Flow();
    checker_.functionChecker().checkFunctionBody(cls->init);
    checker_.returnInferrer().leaveBody(saved);
  }
  std::vector<Field> fields;
  if (cls->parent) {
    fields = cls->parent->type->fields();
  }
  for (ClassField &f : cls->fields) {
    if (f.isStatic || f.method) {
      continue;
    }
    if (!f.resolved) {
      f.resolved = Type::error();
    }
    if (!f.redeclared) {
      if (cls->parent && !methodsNamed(cls->parent, f.name, false).empty()) {
        checker_.diags_.error(f.loc,
                              "'" + f.name + "' is a method of '" + cls->parent->name +
                                  "', so a class that extends it cannot have a field of that name");
      }
      fields.push_back({f.name.str(), f.resolved, f.readonly});
      continue;
    }
    auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
    if (it == fields.end() || f.resolved->isError() || it->type->isError()) {
      continue;
    }
    const ClassDecl *owner = nullptr;
    ClassBases::findField(cls->parent, f.name, &owner);
    if (it->type != f.resolved && !WideWrites::sameReference(f.resolved, it->type)) {
      checker_.diags_.error(
          f.loc, "field '" + f.name + "' of '" + cls->name + "' is '" + f.resolved->name() +
                     "', and '" + owner->name + "' declares it as '" + it->type->name() +
                     "'; a class can narrow a field of the class it extends only to "
                     "objects of a type down the chain of its type, such as 'Dog' for "
                     "'Animal', which lie in memory as they do");
      continue;
    }
    checker_.narrowedFields_ |= it->type != f.resolved || (f.readonly && !it->readonly);
    it->type = f.resolved;
    it->readonly = f.readonly;
  }
  type->defineFields(std::move(fields));
  completing_.erase(cls);
  if (cls->generic) {
    checker_.diags_.setMuted(muted);
  }
}

}
