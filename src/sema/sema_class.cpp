// Classes (docs/objects.md, sections 5 and 6) and the overloads of their
// methods (docs/overloads.md).
//
// A class is an object type told apart by its name: an instance comes only
// from `new` of its class, and a literal never becomes one. Its fields lie in
// the object as those of any object. Its methods are functions that take the
// object as `this`; the class holds them, not the object (there are no
// prototypes), so a call is direct. Static fields are globals, and static
// methods plain functions.
//
// The fields get their values before the object is used: first the
// initializers, in the order written (Sema makes them the method `init`),
// then the parameter properties and the rest of a constructor. Until every
// field is set, `this` may only be used to set fields and to read those set
// already, as in Swift: no method calls, no passing `this` on.
//
// An arrow in a field that nothing assigns again is compiled as a method:
// otherwise every object would hold a closure that holds the object, a cycle.
//
// An object literal with methods, `{ n: 0, inc() { this.n++ } }`, is an
// instance of a class of its own. Its methods are closures; the variables
// they capture are hidden fields of the object.
//
// A method, a static method or a constructor may have several variants with
// one name, each with a body of its own: a call chooses one by the types of
// its arguments, when compiling, as in C#.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

// `add(v: Vec, k?: double)`, a variant in messages.
std::string variantText(const FunctionDecl *fn) {
  std::string text = (fn->isConstructor ? llvm::StringRef("constructor") : fn->name).str() + "(";
  const FuncSymbol *sym = fn->sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    if (i > 0)
      text += ", ";
    const Param &param = fn->params[i];
    text += (param.rest ? "..." : "") + param.label().str();
    const Type *type = sym && i < sym->params.size() ? sym->params[i] : nullptr;
    if (!type)
      continue;
    if (type->isOptional() && !param.rest && (param.optional || param.defaultValue))
      text += "?: " + type->inner()->name().str();
    else
      text += ": " + type->name().str();
  }
  return text + ")";
}

std::string variantList(llvm::ArrayRef<FunctionDecl *> variants) {
  std::string text;
  for (size_t i = 0; i < variants.size(); ++i)
    text += (i == 0 ? "" : i + 1 == variants.size() ? " and " : ", ") + variantText(variants[i]);
  return text;
}

namespace {

// How well a value of type `from` fits a parameter of type `to`: 0 for the
// same type, more for a conversion (int to long is better than int to
// double), -1 if it does not fit.
int conversionRank(const Type *from, const Type *to) {
  if (from == to || from->isError() || to->isError())
    return 0;
  if (to->isOptional()) {
    if (from->isUndefined())
      return 1;
    if (from->isOptional()) {
      int inner = conversionRank(from->inner(), to->inner());
      return inner < 0 ? -1 : inner;
    }
    int inner = conversionRank(from, to->inner());
    return inner < 0 ? -1 : inner + 1;
  }
  // A value that cannot exist: the half of a Result that is never there.
  if (from->isNever())
    return 1;
  if (from->isInt() && to == Type::longTy())
    return 1;
  // A double and a decimal fit an integer equally well, as in C#: neither
  // converts to the other, so with `f(k: double)` and `f(k: decimal)` a call
  // with an int or a long is ambiguous.
  if (from->isInt() && (to->isFloat() || to->isDecimal()))
    return 2;
  if (from == Type::longTy() && (to->isFloat() || to->isDecimal()))
    return 1;
  // A string, a number, a boolean and a Json[] are Json values: a fit worse
  // than any between typed values, so `f(s: string)` wins over `f(j: Json)`.
  if (to->isJson())
    return from->isString() || from->isNumeric() || from->isBool() ||
                   (from->isArray() && from->inner()->isJson())
               ? 3
               : -1;
  // A value of `T extends C` is a C.
  if (from->isParam() && from->constraint())
    return conversionRank(from->constraint(), to);
  // An iterator seen through a type that drops what it returns or what it
  // is sent.
  if (from->isIterator() && to->isIterator())
    return from->yielded() == to->yielded() &&
                   (from->returned() == to->returned() || to->returned()->isVoid()) &&
                   (from->sent() == to->sent() || to->sent()->isVoid())
               ? 1
               : -1;
  // A Buffer is a Uint8Array.
  if (from->isBytes() || to->isBytes())
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8 ? 1 : -1;
  // A match of a known regex is a match of some regex.
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() ||
      to->isCrypto() || from->isWeakRef() || to->isWeakRef())
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 && !from->field("indices")
               ? 1
               : -1;
  // A type up the declared chain: the nearer, the better, as in C#.
  if (from->isObject() && to->isObject()) {
    int distance = subtypeDistance(from, to);
    if (distance > 0)
      return distance;
  }
  // An object seen through a type with read-only fields.
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      !from->isAnyObject() && !to->isAnyObject() && from->isTuple() == to->isTuple() &&
      from->fields().size() == to->fields().size() && from->overloads().empty() &&
      to->overloads().empty() &&
      llvm::all_of(from->fields(), [&](const Field &f) {
        const Field *t = to->field(f.name);
        return t && t->type == f.type && (!f.readonly || t->readonly);
      }))
    return 1;
  // A Result converts half by half, as good as its worse half:
  // `Result<int, never>` fits `Result<int, string>` better than
  // `Result<double, string>`.
  if (from->isResult() && to->isResult()) {
    auto half = [](const Type *a, const Type *b) {
      if (a == b)
        return 0;
      if (a->isNever())
        return 1;
      return a->isVoid() || b->isVoid() ? -1 : conversionRank(a, b);
    };
    int value = half(from->okType(), to->okType()), error = half(from->errType(), to->errType());
    return value < 0 || error < 0 ? -1 : std::max(value, error);
  }
  return -1;
}

// The type of the parameter that takes argument `i`.
const Type *parameterAt(const FuncSymbol *sym, size_t i) {
  if (sym->hasRest && i + 1 >= sym->params.size()) {
    const Type *rest = sym->params.back();
    return rest->isArray() ? rest->inner() : rest;
  }
  return i < sym->params.size() ? sym->params[i] : nullptr;
}

// Whether a tree of number literals has one written as a double: `1.5`,
// `2.0`, `1e-3`.
bool hasFloatLiteral(const Expr *expr) {
  if (isa<FloatLitExpr>(expr))
    return true;
  if (auto *unary = dyn_cast<UnaryExpr>(expr))
    return hasFloatLiteral(unary->operand);
  if (auto *bin = dyn_cast<BinaryExpr>(expr))
    return hasFloatLiteral(bin->lhs) || hasFloatLiteral(bin->rhs);
  return false;
}

const char *accessWord(Access access) {
  return access == Access::Private ? "private" : "protected";
}

// A static field of a class or of one up its chain, and the class that
// declares it.
ClassField *staticField(ClassDecl *cls, llvm::StringRef name, ClassDecl **owner) {
  // Those of an instance of a generic class are the generic class's.
  for (ClassDecl *c = cls; c; c = c->parent)
    for (ClassField &f : c->staticOwner()->fields)
      if (f.isStatic && f.name == name && f.var) {
        *owner = c->staticOwner();
        return &f;
      }
  return nullptr;
}

} // namespace

void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names) {
  visitExprs(body, [&](const Expr *e) {
    if (auto *call = dyn_cast<CallExpr>(e))
      if (auto *member = dyn_cast<MemberExpr>(call->callee))
        names.insert(member->name);
  });
}

ClassDecl *Sema::classOf(const Type *type) {
  return type ? const_cast<ClassDecl *>(type->classDecl()) : nullptr;
}

// Up the chain too: the variants of a class, then those of the class it
// extends that none of them overrides, and so on.
std::vector<FunctionDecl *> Sema::methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                               bool isStatic) {
  std::vector<FunctionDecl *> found;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<FunctionDecl *> own;
    // The static methods of an instance of a generic class are the generic
    // class's.
    for (FunctionDecl *fn : (isStatic ? c->staticOwner() : c)->methods)
      if (fn->name == name && fn->isStatic == isStatic)
        own.push_back(fn);
    if (!isStatic)
      for (const ClassField &f : c->fields)
        if (f.method && f.name == name)
          own.push_back(f.method);
    for (FunctionDecl *fn : own) {
      bool replaced = llvm::any_of(found, [&](const FunctionDecl *v) {
        if (!isStatic)
          return overridesTransitively(v, fn);
        // A static method of a subclass hides one with the same parameters.
        const FuncSymbol *a = v->sym, *b = fn->sym;
        return a && b && a->params == b->params && a->minArgs == b->minArgs &&
               a->hasRest == b->hasRest;
      });
      if (!replaced)
        found.push_back(fn);
    }
  }
  return found;
}

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

void Sema::declareClass(ClassDecl *cls) {
  cls->id = ++classCount_;
  cls->type = Type::classType(cls->name, cls);
  classes_.push_back(cls);
  declare(cls->name, cls->loc(), {nullptr, nullptr, nullptr, cls});
}

void Sema::resolveClass(ClassDecl *cls) {
  if (cls->resolved)
    return;
  cls->resolved = true;
  // The type parameters of a generic class are visible in its members.
  declareClassTypeParams(cls);
  typeScopes_.emplace_back();
  bindClassTypeParams(cls);
  resolveClassBases(cls);
  // Its members, by name: a name means one thing (a static member and one of
  // the instances may share it, as in JavaScript).
  for (size_t i = 0; i < cls->fields.size(); ++i) {
    ClassField &f = cls->fields[i];
    for (size_t j = 0; j < i; ++j)
      if (cls->fields[j].name == f.name && cls->fields[j].isStatic == f.isStatic) {
        diags_.error(f.loc, "duplicate member '" + f.name + "' in class '" + cls->name + "'");
        diags_.note(cls->fields[j].loc, "previously declared here");
        break;
      }
    for (FunctionDecl *fn : cls->methods)
      if (fn->name == f.name && fn->isStatic == f.isStatic) {
        diags_.error(fn->loc(), "duplicate member '" + f.name + "' in class '" + cls->name +
                                    "': a field and a method cannot share a name");
        diags_.note(f.loc, "the field is declared here");
      }
  }

  for (ClassField &f : cls->fields) {
    if (f.definite)
      diags_.error(f.loc, "'" + f.name + "!' (\"set later\") is not supported: a field gets its "
                              "value where it is declared or in the constructor; if it may "
                              "have none, make it optional: '" + f.name + "?: T'");
    // Null when the type comes from what sets the field (see completeClass).
    const Type *type = nullptr;
    if (f.parameter) {
      // Its type is the parameter's: written, or that of a literal default
      // value (`public n = 10`), or found with the constructor's signature
      // (see completeClass).
      const Param *param = nullptr;
      for (FunctionDecl *ctor : cls->constructors)
        for (const Param &p : ctor->params)
          if (p.property && p.name == f.name && !param)
            param = &p;
      f.optional = param->optional;
      if (param->type)
        type = resolveType(*param->type, /*allowVoid=*/false);
      else if (param->defaultValue)
        type = literalType(param->defaultValue);
    } else if (f.type) {
      type = resolveType(*f.type, /*allowVoid=*/false);
    } else if (f.init) {
      type = literalType(f.init); // or from the initializer, checked later
    } else {
      diags_.error(f.loc, "field '" + f.name + "' needs a type, e.g. '" + f.name + ": int'");
      type = Type::error();
    }
    if (type && f.optional)
      type = Type::optional(type);
    f.resolved = type;

    // An arrow in a field that nothing assigns again is a method. A
    // function in a field is named after it, as in JavaScript.
    auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(f.init);
    if (fe && fe->fn->name.empty())
      fe->fn->name = f.name;
    if (fe && fe->fn->isArrow && !f.isStatic && !f.parameter && !f.optional && !f.redeclared &&
        !assignedFields_.contains(f.name) && (!type || type->isFunction())) {
      FunctionDecl *fn = fe->fn;
      fn->name = f.name;
      fn->cls = cls;
      fn->access = f.access;
      fn->isOverride = f.isOverride;
      fn->isExpression = false;
      f.method = fn;
      f.resolved = nullptr; // the type of the method, once declared
    }

    // Static members are shared by all the instances of a generic class.
    if (f.isStatic && type && !cls->typeArgs.empty() && mentionsParam(type)) {
      diags_.error(f.loc, "static field '" + f.name + "' cannot use the type parameters of '" +
                              cls->name + "': every instance of the class shares it");
      f.resolved = type = Type::error();
    }
    if (f.isStatic) {
      VarSymbol *var = ctx_.newVar();
      var->name = ctx_.keepName((cls->name + "." + f.name).str());
      var->loc = f.loc;
      var->type = type; // null until the class's code runs, for an initializer
      var->isConst = f.readonly;
      var->isGlobal = true;
      var->usedInFunction = true;
      var->declSeq = ++seq_;
      f.var = var;
      if (!f.init && !f.optional && type && !type->isError())
        diags_.error(f.loc, "static field '" + f.name + "' needs a value, e.g. 'static " + f.name +
                                ": " + type->name() + " = ...', or make it optional: 'static " +
                                f.name + "?: " + type->name() + "'");
    }
  }
  typeScopes_.pop_back();
}

void Sema::declareTyped(FunctionDecl *fn, const Type *declared) {
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *given = i < declared->params().size() ? declared->params()[i] : nullptr;
    const Type *type = param.type ? resolveType(*param.type, /*allowVoid=*/false) : given;
    if (!type) {
      diags_.error(param.loc, "parameter '" + param.label() + "' takes more arguments than '" +
                                  declared->name() + "' passes");
      type = Type::error();
    }
    if (param.optional && !type->isOptional())
      type = Type::optional(type);
    param.declaredType = param.defaultValue ? type->nonOptional() : type;
    sym->params.push_back(type);
  }
  // It takes all the arguments its type passes, named or not.
  for (size_t i = fn->params.size(); i < declared->params().size(); ++i) {
    Param hidden;
    hidden.name = ctx_.hiddenName();
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

void Sema::declareMethods(ClassDecl *cls) {
  for (FunctionDecl *ctor : cls->constructors) {
    declareFunction(ctor, nullptr, /*scoped=*/false);
    ctor->sym->ret = Type::voidTy(); // `new` gives the object
  }
  for (FunctionDecl *fn : cls->methods) {
    declareFunction(fn, nullptr, /*scoped=*/false);
    // Without a body to infer it from, an abstract method returns what its
    // implementations return (see linkClasses); those of a generic class are
    // in classes that extend its instances, each with other types.
    if (fn->isAbstract && !fn->returnType && cls->isGeneric())
      diags_.error(fn->loc(), "abstract method '" + fn->name + "' of generic class '" + cls->name +
                                  "' needs a return type, e.g. '): T'");
    else if (fn->isAbstract && !fn->returnType)
      fn->sym->ret = nullptr;
    // A generic method is a copy for each list of types: it cannot replace
    // a method up the chain, nor be replaced.
    if (!fn->typeParams.empty() && (fn->isAbstract || fn->isOverride))
      diags_.error(fn->loc(), std::string("a generic method cannot be ") +
                                  (fn->isAbstract ? "abstract" : "an override") +
                                  ": each call runs a copy of it made for its types");
  }
  for (ClassField &f : cls->fields) {
    if (!f.method)
      continue;
    // An arrow method takes the parameters and the result the field's type
    // gives, if it has one.
    FunctionDecl *fn = f.method;
    typeScopes_.emplace_back();
    bindClassTypeParams(cls);
    const Type *declared = f.type ? resolveType(*f.type, /*allowVoid=*/false) : nullptr;
    typeScopes_.pop_back();
    if (!declared || !declared->isFunction()) {
      declareFunction(fn, nullptr, /*scoped=*/false);
      continue;
    }
    declareTyped(fn, declared);
  }

  // The initializers of the fields, as the method `init`: `this.x = value`
  // for each, in order.
  std::vector<Stmt *> sets;
  for (const ClassField &f : cls->fields) {
    if (f.isStatic || !f.init || f.method)
      continue;
    auto *target = ctx_.make<MemberExpr>(f.loc, ctx_.make<NameExpr>(f.loc, "this"), f.name);
    auto *set = ctx_.make<AssignExpr>(f.loc, std::nullopt, target, f.init);
    sets.push_back(ctx_.make<ExprStmt>(f.loc, set));
  }
  if (!sets.empty()) {
    auto *init = ctx_.make<FunctionDecl>(cls->loc(), "init");
    init->cls = cls;
    init->body = ctx_.make<BlockStmt>(cls->loc());
    init->body->body = std::move(sets);
    declareFunction(init, nullptr, /*scoped=*/false);
    init->sym->ret = Type::voidTy();
    cls->init = init;
  }

  // The variants of each name, numbered.
  auto number = [](llvm::ArrayRef<FunctionDecl *> fns) {
    for (size_t i = 0; i < fns.size(); ++i)
      for (size_t j = 0; j < i; ++j)
        if (fns[j]->name == fns[i]->name && fns[j]->isStatic == fns[i]->isStatic)
          fns[i]->variant = fns[j]->variant + 1;
  };
  number(cls->methods);
  number(cls->constructors);
  checkVariants(cls);
}

// Variants that a call cannot tell apart (docs/overloads.md, section 3): the
// same parameters, or a call that fits two of them equally well, leaving out
// optional parameters.
void Sema::checkVariants(ClassDecl *cls) {
  auto check = [&](llvm::ArrayRef<FunctionDecl *> fns) {
    for (size_t i = 0; i < fns.size(); ++i)
      for (size_t j = 0; j < i; ++j)
        if (fns[j]->name == fns[i]->name && fns[j]->isStatic == fns[i]->isStatic)
          checkVariantPair(fns[j], fns[i]);
  };
  check(cls->methods);
  check(cls->constructors);
}

void Sema::checkVariantPair(FunctionDecl *a, FunctionDecl *b) {
  if (!a->sym || !b->sym)
    return;
  const FuncSymbol *x = a->sym, *y = b->sym;
  if (llvm::is_contained(x->params, nullptr) || llvm::is_contained(y->params, nullptr))
    return;
  std::string what = a->isConstructor ? "the constructor" : "'" + a->name.str() + "'";
  if (x->params == y->params && x->minArgs == y->minArgs && x->hasRest == y->hasRest) {
    if (x->ret && y->ret && x->ret != y->ret)
      diags_.error(b->loc(), "variants of " + what +
                                 " cannot differ only in what they return: a call "
                                 "chooses a variant by its arguments");
    else
      diags_.error(b->loc(), "duplicate variant " + variantText(b) +
                                 ": variants must differ in their parameters");
    diags_.note(a->loc(), "the other variant is here");
    return;
  }
  // A count of arguments both take without their rest parameters, for
  // which the parameters are the same.
  size_t fixedX = x->params.size() - x->hasRest, fixedY = y->params.size() - y->hasRest;
  for (size_t n = std::max(x->minArgs, y->minArgs); n <= std::min(fixedX, fixedY); ++n) {
    bool same = true;
    for (size_t k = 0; k < n && same; ++k)
      same = x->params[k] == y->params[k];
    if (!same)
      continue;
    diags_.error(b->loc(), "variants " + variantText(a) + " and " + variantText(b) +
                               " fit a call with " + std::to_string(n) + " argument" +
                               (n == 1 ? "" : "s") +
                               " equally well; give them parameters of different types");
    diags_.note(a->loc(), "the other variant is here");
    break;
  }
}

// The types of the fields that come from what initializes them. A parameter
// property with a default value that is not a literal has the type of its
// parameter; any other field the type of its initializer, which `init`
// checks (see checkAssign).
void Sema::completeClass(ClassDecl *cls) {
  auto *type = const_cast<Type *>(cls->type);
  if (!type->isIncomplete() || cls->literal || !completing_.insert(cls).second)
    return;
  // The fields up the chain come first.
  if (cls->parent)
    completeClass(cls->parent);
  // An instance of a generic class: the fields of the generic class, with
  // the types of the instance; what it reports there is not reported again.
  bool muted = false;
  if (ClassDecl *generic = cls->generic) {
    completeClass(generic);
    for (size_t i = 0; i < cls->fields.size() && i < generic->fields.size(); ++i)
      if (!cls->fields[i].isStatic && !cls->fields[i].method && !cls->fields[i].resolved)
        cls->fields[i].resolved = generic->fields[i].resolved
                                      ? instanceType(cls, generic->fields[i].resolved)
                                      : Type::error();
    muted = diags_.setMuted(true);
  }
  for (ClassField &f : cls->fields) {
    if (!f.parameter || f.resolved)
      continue;
    for (FunctionDecl *ctor : cls->constructors) {
      if (f.resolved || !ctor->sym)
        continue;
      resolveParams(ctor->sym);
      for (const Param &param : ctor->params)
        if (param.property && param.name == f.name && param.declaredType)
          f.resolved = param.declaredType;
    }
    if (!f.resolved)
      f.resolved = Type::error();
  }
  bool pending = llvm::any_of(cls->fields, [](const ClassField &f) {
    return !f.isStatic && !f.method && !f.resolved;
  });
  if (pending && cls->init && !cls->init->checked) {
    // The initializers run in the file of the class, where the globals
    // declared so far are visible.
    BodyState saved = enterBody(cls->init);
    flow_ = Flow();
    checkFunctionBody(cls->init);
    leaveBody(saved);
  }
  std::vector<Field> fields;
  if (cls->parent)
    fields = cls->parent->type->fields();
  for (ClassField &f : cls->fields) {
    if (f.isStatic || f.method)
      continue;
    if (!f.resolved)
      f.resolved = Type::error(); // its initializer had an error
    if (!f.redeclared) {
      if (cls->parent && !methodsNamed(cls->parent, f.name, /*isStatic=*/false).empty())
        diags_.error(f.loc, "'" + f.name + "' is a method of '" + cls->parent->name +
                                "', so a class that extends it cannot have a field of that name");
      fields.push_back({f.name.str(), f.resolved, f.readonly});
      continue;
    }
    // Declared up the chain: the same field, which the class may narrow to a
    // type of the same references (`pet: Dog` for `pet: Animal`) or make
    // read-only. Code that writes another Animal there through the class up
    // the chain is found by the check of the whole program (sema_widen.cpp,
    // docs/objects.md, section 2).
    auto it = llvm::find_if(fields, [&](const Field &g) { return g.name == f.name; });
    if (it == fields.end() || f.resolved->isError() || it->type->isError())
      continue;
    const ClassDecl *owner = nullptr;
    findField(cls->parent, f.name, &owner);
    if (it->type != f.resolved && !sameReference(f.resolved, it->type)) {
      diags_.error(f.loc, "field '" + f.name + "' of '" + cls->name + "' is '" +
                              f.resolved->name() + "', and '" + owner->name + "' declares it as '" +
                              it->type->name() +
                              "'; a class can narrow a field of the class it extends only to "
                              "objects of a type down the chain of its type, such as 'Dog' for "
                              "'Animal', which lie in memory as they do");
      continue;
    }
    narrowedFields_ |= it->type != f.resolved || (f.readonly && !it->readonly);
    it->type = f.resolved;
    it->readonly = f.readonly;
  }
  type->defineFields(std::move(fields));
  completing_.erase(cls);
  if (cls->generic)
    diags_.setMuted(muted);
}

//===----------------------------------------------------------------------===//
// Bodies
//===----------------------------------------------------------------------===//

void Sema::checkClassStmt(ClassDecl *cls) {
  if (scopes_.size() != 1 || !functions_.empty() || !cls->type) {
    diags_.error(cls->loc(), "a class can be declared only at the top level of a file; here, "
                             "make it with a class expression, 'const " + cls->name +
                             " = class { ... }'");
    return;
  }
  // A class whose base is a value (`extends Mixin(Base)`) is made here,
  // where that value is.
  if (cls->baseExpr && !cls->resolved)
    makeLateClass(cls);
  // The static fields get their values when the declaration runs, in order;
  // the class exists by then (`static instance = new C()`).
  checkStaticFields(cls);
  ClassDecl *outer = std::exchange(currentClass_, cls);
  // Then the decorators, as TypeScript applies them right after the class:
  // of a generic class once, for the generic class itself (with the types
  // of the declaration, as TypeScript, which has one class), not for each
  // instance.
  if (!cls->generic && hasDecorators(cls))
    decorateClass(cls);
  currentClass_ = outer;
}

bool Sema::hasDecorators(const ClassDecl *cls) {
  auto decorated = [](const FunctionDecl *fn) {
    return !fn->decorators.empty() ||
           llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
  };
  return !cls->decorators.empty() || llvm::any_of(cls->methods, decorated) ||
         llvm::any_of(cls->constructors, decorated) ||
         llvm::any_of(cls->fields, [](const ClassField &f) { return !f.decorators.empty(); });
}

void Sema::checkClassBodies(ClassDecl *cls) {
  completeClass(cls);
  if (cls->init && !cls->init->checked) {
    flow_ = Flow();
    checkFunctionBody(cls->init);
  }
  // Without a constructor, the initializers must set every field.
  if (cls->constructors.empty()) {
    for (const ClassField &f : cls->fields) {
      if (f.isStatic || f.method || f.optional || f.init || f.definite || !f.resolved ||
          f.resolved->isError())
        continue;
      diags_.error(f.loc, "field '" + f.name + "' never gets a value: give it one here, e.g. '" +
                              f.name + ": " + f.resolved->name() + " = ...', set it in a "
                              "constructor, or make it optional: '" + f.name + "?: " +
                              f.resolved->name() + "'");
    }
  }
  for (FunctionDecl *ctor : cls->constructors)
    if (!ctor->checked) {
      flow_ = Flow();
      checkFunctionBody(ctor);
    }
  std::vector<FunctionDecl *> methods = cls->methods;
  for (const ClassField &f : cls->fields)
    if (f.method)
      methods.push_back(f.method);
  for (FunctionDecl *fn : methods) {
    if (isTemplate(fn->sym)) // checked in its copies
      continue;
    if (!fn->sym->ret)
      inferReturnType(fn->sym);
    if (fn->checked || fn->isAbstract)
      continue;
    flow_ = Flow();
    checkFunctionBody(fn);
  }
  checkAbstract(cls);
  checkImplements(cls);
  // An override returns what the method it overrides returns, or, for an
  // object, a type that is one (Dog for Animal): the same value in memory.
  for (FunctionDecl *fn : ownMethods(cls)) {
    if (!fn->overrides)
      continue;
    const Type *mine = returnType(fn->sym), *theirs = returnType(fn->overrides->sym);
    if (!mine || !theirs || mine == theirs || mine->isError() || theirs->isError())
      continue;
    if (mine->nonOptional()->isObject() && theirs->nonOptional()->isObject() &&
        isImplicitlyConvertible(mine, theirs))
      continue;
    diags_.error(fn->loc(), "'" + fn->name + "' returns '" + mine->name() + "' in '" + cls->name +
                                "', and '" + theirs->name() + "' in '" + fn->overrides->cls->name +
                                "', which it overrides; an override returns what the method it "
                                "overrides returns");
  }
}

VarSymbol *Sema::declareThis(FunctionDecl *fn) {
  VarSymbol *self = ctx_.newVar();
  self->name = "this";
  self->loc = fn->loc();
  self->type = fn->cls->type;
  self->isConst = true;
  self->owner = fn;
  self->declSeq = ++seq_;
  fn->thisVar = self;
  return self;
}

// `this.x` and `this.m()` in a constructor see the fields set so far.
bool Sema::isInitThis(const Expr *expr) const {
  while (auto *conv = dyn_cast<CastExpr>(expr))
    if (conv->isImplicit())
      expr = conv->operand;
    else
      break;
  auto *name = dyn_cast<NameExpr>(expr);
  return name && name->var && initThis_ && name->var == initThis_ && currentDecl() == initFn_;
}

bool Sema::thisReady(const Flow &flow) const {
  return !flow.beforeSuper && firstUnset(flow).empty();
}

std::string Sema::firstUnset(const Flow &flow) const {
  if (!initFn_)
    return "";
  for (const ClassField &f : initFn_->cls->fields)
    if (!f.isStatic && !f.method && !f.optional && !llvm::is_contained(flow.setFields, f.name))
      return f.name.str();
  return "";
}

void Sema::checkFieldsSet(llvm::SMLoc loc) {
  if (flow_.unreachable)
    return;
  std::string unset = firstUnset(flow_);
  if (unset.empty())
    return;
  diags_.error(loc, "the constructor of '" + initFn_->cls->name + "' does not set field '" + unset +
                        "' on every path; set it, give it a value where it is declared, or "
                        "make it optional: '" + unset + "?: T'");
}

Expr *Sema::checkThis(NameExpr *name) {
  name->type = Type::error();
  // An arrow sees the `this` of the function around it.
  FunctionDecl *method = nullptr;
  for (FunctionDecl *fn : llvm::reverse(functions_)) {
    if (fn->takesThis()) {
      method = fn;
      break;
    }
    if (fn->isArrow)
      continue;
    // `function (this: unknown, ...)`: its parameter.
    if (fn->sym && fn->sym->hasThis && !fn->params.empty() && fn->params[0].sym) {
      VarSymbol *var = fn->params[0].sym;
      name->var = var;
      noteUse(var, name->loc());
      name->type = var->type;
      if (const Type *narrowed = narrowedType(var))
        return convert(name, narrowed);
      return name;
    }
    if (fn->cls && fn->isStatic)
      diags_.error(name->loc(), "in a static method 'this' would be the class itself; use the "
                                "class's name: '" + fn->cls->name + "." +
                                    (thisMember_.empty() ? "x" : thisMember_) + "'");
    else
      diags_.error(name->loc(), "a 'function' has no 'this' of its own; use an arrow function, "
                                "which sees the 'this' of the method around it");
    return name;
  }
  if (!method) {
    if (staticInit_ && currentClass_)
      diags_.error(name->loc(), "in a static field 'this' would be the class itself; use the "
                                "class's name: '" + currentClass_->name + "." +
                                    (thisMember_.empty() ? "x" : thisMember_) + "'");
    else
      diags_.error(name->loc(), "'this' is used outside of a method; it exists in the methods "
                                "of classes and object literals");
    return name;
  }
  VarSymbol *var = method->thisVar;
  if (!var)
    return name; // a copy whose body is not being checked
  name->var = var;
  noteUse(var, name->loc());
  name->type = var->type;
  if (var != initThis_)
    return name;
  // The rules while the constructor sets the fields.
  if (currentDecl() != initFn_) {
    if (!closureThisReady_)
      diags_.error(name->loc(), "this function uses 'this', and it is created before every field "
                                "is set; create it after the constructor sets them");
  } else if (flow_.beforeSuper) {
    diags_.error(name->loc(), "'this' cannot be used before 'super(...)': the object exists once "
                              "the constructor of '" + initFn_->cls->parent->name + "' has run");
    name->type = Type::error();
  } else if (thisMember_.empty()) {
    std::string unset = firstUnset(flow_);
    if (!unset.empty())
      diags_.error(name->loc(), "'this' is used before every field is set: field '" + unset +
                                    "' is not set yet; until then 'this' can only set fields "
                                    "and read the ones set");
  }
  return name;
}

bool Sema::checkAccess(const ClassDecl *cls, Access access, llvm::StringRef name,
                       llvm::SMLoc loc) {
  // The instances of a generic class are that class.
  if (access == Access::Public || sameClass(currentClass_, cls))
    return true;
  // A protected member is for the class and those down its chain.
  if (access == Access::Protected)
    for (const ClassDecl *c = currentClass_; c; c = c->parent)
      if (sameClass(c, cls))
        return true;
  std::string message = "'" + name.str() + "' is " + accessWord(access) + " in class '" +
                        cls->name.str() + "'";
  if (access == Access::Protected)
    message += "; only '" + cls->name.str() + "' and the classes that extend it can use it";
  diags_.error(loc, message);
  return false;
}

//===----------------------------------------------------------------------===//
// Members
//===----------------------------------------------------------------------===//

Expr *Sema::checkClassMember(MemberExpr *member, const Type *expected) {
  ClassDecl *cls = classOf(member->object->type);
  completeClass(cls);
  member->type = Type::error();
  llvm::StringRef name = member->name;
  if (derivesFromError(cls) && name == "stack")
    return checkErrorStack(member);
  if (Expr *date = checkStatsDate(member, cls))
    return date;
  bool viaThis = isInitThis(member->object);
  const ClassDecl *owner = nullptr;
  const ClassField *field = findField(cls, name, &owner);
  if (field && !field->method) {
    checkAccess(owner, field->access, name, member->loc());
    // The fields up the chain are set by `super(...)`, before `this` exists.
    if (viaThis && owner == initFn_->cls && !llvm::is_contained(flow_.setFields, name)) {
      diags_.error(member->loc(), "field '" + name + "' is read before it is set");
      return member;
    }
    if (!field->resolved) {
      diags_.error(member->loc(), "the type of field '" + name +
                                      "' comes from its initializer, which runs later; declare "
                                      "it: '" + name + ": T = ...'");
      return member;
    }
    member->type = field->resolved;
    if (Expr *narrowed = narrowedField(member); narrowed != member)
      return narrowed;
    if (field->resolved->isOptional())
      if (std::optional<FieldPath> path = fieldPath(member); path && isFieldDefined(*path))
        return convert(member, field->resolved->inner());
    return member;
  }
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/false);
  // A property of a built-in class written in tz (sema_library.cpp).
  if (llvm::any_of(methods, [](const FunctionDecl *fn) {
        return fn->accessor != FunctionDecl::Accessor::None;
      }))
    if (Expr *read = checkAccessorRead(member, cls, expected))
      return read;
  if (methods.empty()) {
    if (!methodsNamed(cls, name, /*isStatic=*/true).empty() ||
        llvm::any_of(cls->fields,
                     [&](const ClassField &f) { return f.isStatic && f.name == name; }))
      diags_.error(member->loc(), "'" + name + "' is a static member of '" + cls->name +
                                      "': use it through the class, '" + cls->name + "." + name +
                                      "'");
    else
      diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                      cls->type->name() + "'");
    return member;
  }
  // A method as a value: bound to the object, so `const f = o.m; f()` works.
  checkAccess(methods[0]->cls, methods[0]->access, name, member->loc());
  // But not one of an emitter that the event map checks (sema_events.cpp).
  if (methods[0]->cls->library && isEmitterClass(cls) && !inLibrary(member->loc()) &&
      name != "getMaxListeners" && name != "eventNames") {
    reportEmitterMethodValue(member);
    return member;
  }
  if (viaThis && !flow_.beforeSuper && !thisReady(flow_))
    diags_.error(member->loc(), "'this." + name + "' is used before every field is set: field '" +
                                    firstUnset(flow_) + "' is not set yet");
  FuncSymbol *sym = variantForType(methods, expected, member);
  if (!sym)
    return member;
  if (sym->isGeneric()) {
    diags_.error(member->loc(), "'" + name + "' is a generic method, so it can only be called: each "
                                    "call runs a copy of it made for its types; wrap it in an arrow, e.g. "
                                    "'(x) => o." + name + "(x)'");
    return member;
  }
  // The method of the object's class, if classes down the chain override it.
  if (FunctionDecl *impl = soleImplementation(cls, sym->decl))
    member->method = impl->sym;
  else
    member->method = sym, member->virtualCall = true;
  member->type = functionType(sym);
  return member;
}

const Type *Sema::checkFieldTarget(MemberExpr *member, bool compound) {
  const Type *type = member->object->type;
  ClassDecl *cls = classOf(type);
  if (!cls)
    return checkObjectFieldTarget(member);
  completeClass(cls);
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const ClassField *field = findField(cls, name, &owner);
  if (derivesFromError(cls) && name == "stack") {
    diags_.error(member->loc(), "'stack' cannot be assigned: it is made from the places the error "
                                "passed on its way up");
    return Type::error();
  }
  if (cls == statsClass_ && checkStatsDate(member, cls)) {
    diags_.error(member->loc(), "'" + name + "' cannot be assigned: it is made from '" + name +
                                    "Ms' when it is read; keep the date in a variable of its own");
    return Type::error();
  }
  if (!field)
    if (const Type *accessor = checkAccessorTarget(member, cls, compound))
      return accessor;
  if (!field || field->method) {
    if (field || !methodsNamed(cls, name, /*isStatic=*/false).empty())
      diags_.error(member->loc(), "'" + name + "' is a method of '" + type->name() +
                                      "' and cannot be replaced: a method belongs to the class, "
                                      "not to the object; to change what it does, declare a "
                                      "field that holds a function: '" + name +
                                      ": () => void = () => { ... }'");
    else
      diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                      type->name() + "'");
    return Type::error();
  }
  checkAccess(owner, field->access, name, member->loc());
  bool viaThis = isInitThis(member->object);
  if (compound && viaThis && owner == initFn_->cls && !llvm::is_contained(flow_.setFields, name)) {
    diags_.error(member->loc(), "field '" + name + "' is read before it is set");
    return Type::error();
  }
  // A read-only field is set by the constructor of the class that declares
  // it (or declares it again).
  if (field->readonly && !(viaThis && owner == initFn_->cls))
    diags_.error(member->loc(), "cannot assign to '" + name + "' because it is a read-only field of '" +
                                    type->name() + "'; only its constructor can set it");
  if (!field->resolved) {
    // Its type comes from this initializer (see checkAssign).
    if (viaThis && initFn_ == cls->init && owner == cls && !compound) {
      member->type = nullptr;
      return nullptr;
    }
    diags_.error(member->loc(), "the type of field '" + name +
                                    "' comes from its initializer, which runs later; declare "
                                    "it: '" + name + ": T = ...'");
    return Type::error();
  }
  member->type = field->resolved;
  return field->resolved;
}

Expr *Sema::checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected) {
  llvm::StringRef name = member->name;
  member->type = Type::error();
  // Up the chain too: a class has the static members of the one it extends.
  ClassDecl *owner = nullptr;
  if (ClassField *found = staticField(cls, name, &owner)) {
    ClassField &f = *found;
    checkAccess(owner, f.access, name, member->loc());
    auto *read = ctx_.make<NameExpr>(member->loc(), f.var->name);
    read->var = f.var;
    if (!f.var->type) {
      if (!owner->declared && loading_ && functions_.empty())
        diags_.error(member->loc(), "'" + f.var->name + "' is used before class '" + owner->name +
                                        "' is declared, which sets it");
      else
        diags_.error(member->loc(), "the type of '" + f.var->name +
                                        "' comes from its value, which is set later; declare "
                                        "it: 'static " + name + ": T = ...'");
      read->type = Type::error();
      return read;
    }
    if (!owner->declared && loading_ && functions_.empty() &&
        declModules_.lookup(owner) == loading_)
      diags_.error(member->loc(), "'" + f.var->name + "' is used before class '" + owner->name +
                                      "' is declared, which sets it");
    else if (staticInit_ && currentClass_ == owner && !staticsSet_.contains(f.var))
      diags_.error(member->loc(), "'" + f.var->name + "' is read before it is set: the static "
                                      "fields are set in the order they are declared");
    read->type = f.var->type;
    if (read->type->isOptional() && isDefined(f.var))
      return convert(read, read->type->inner());
    return read;
  }
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/true);
  if (methods.empty() && (name == "name" || name == "prototype")) {
    // As of a class as a value: `C.name` is "C"; there is no prototype.
    auto *object = cast<NameExpr>(member->object);
    member->object = checkClassValue(object, cls);
    return checkClassRefMember(member);
  }
  if (methods.empty()) {
    if (findField(cls, name) || !methodsNamed(cls, name, /*isStatic=*/false).empty())
      diags_.error(member->loc(), "'" + name + "' is a member of the instances of '" + cls->name +
                                      "', not of the class: create one with 'new " + cls->name +
                                      "(...)'");
    else
      diags_.error(member->loc(), "class '" + cls->name + "' has no static member '" + name + "'");
    return member;
  }
  checkAccess(methods[0]->cls, methods[0]->access, name, member->loc());
  FuncSymbol *sym = variantForType(methods, expected, member);
  if (!sym)
    return member;
  // A static method as a value, as a top-level function is one.
  auto *value = ctx_.make<NameExpr>(member->loc(), name);
  value->func = sym;
  value->type = functionType(sym);
  return value;
}

const Type *Sema::checkStaticTarget(Expr *&target, ClassDecl *cls) {
  auto *member = cast<MemberExpr>(target);
  // The class's name is no value: the target is a static field, or wrong.
  member->object->type = member->type = Type::error();
  ClassDecl *owner = nullptr;
  if (ClassField *found = staticField(cls, member->name, &owner)) {
    ClassField &f = *found;
    checkAccess(owner, f.access, f.name, member->loc());
    auto *name = ctx_.make<NameExpr>(member->loc(), f.var->name);
    name->var = f.var;
    name->type = f.var->type ? f.var->type : Type::error();
    target = name;
    if (f.var->isConst)
      diags_.error(member->loc(), "cannot assign to '" + f.var->name +
                                      "' because it is a read-only static field");
    f.var->reassigned = true;
    if (!f.var->type) {
      diags_.error(member->loc(), "the type of '" + f.var->name +
                                      "' comes from its value, which is set later; declare it: "
                                      "'static " + f.name + ": T = ...'");
      return Type::error();
    }
    return f.var->type;
  }
  if (!methodsNamed(cls, member->name, /*isStatic=*/true).empty())
    diags_.error(member->loc(), "'" + member->name + "' is a static method of '" + cls->name +
                                    "' and cannot be replaced");
  else
    diags_.error(member->loc(), "class '" + cls->name + "' has no static field '" + member->name +
                                    "'");
  return Type::error();
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

Expr *Sema::checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  ClassDecl *cls = classOf(member->object->type);
  completeClass(cls);
  call->type = Type::error();
  member->type = Type::error();
  llvm::StringRef name = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (cls->builtin && derivesFromError(cls))
    if (Expr *checked = checkErrorMethod(call, member))
      return checked;
  if (Expr *checked = checkFsClassMethod(call, member, cls))
    return checked;
  if (Expr *checked = checkStreamCall(call, member, cls))
    return checked;
  if (isEmitterClass(cls))
    if (Expr *checked = checkEmitterCall(call, member, cls, /*super=*/false))
      return checked;
  if (ClassDecl *target = libraryClass("EventTarget"); target && cls->derivesFrom(target))
    if (Expr *checked = checkEventTargetCall(call, member, cls))
      return checked;
  const ClassField *field = findField(cls, name);
  if (field && !field->method) {
    // A function in a field.
    call->callee = checkClassMember(member, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction())
      return checkValueCall(call, type);
    if (type->isOptional() && type->inner()->isFunction())
      diags_.error(member->loc(), "the function may be undefined; check it first, or call it with "
                                  "'?.()', which gives undefined then");
    else if (!type->isError())
      diags_.error(member->loc(), "field '" + name + "' is not a function; it has type '" +
                                      type->name() + "'");
    checkArgsLoosely();
    return call;
  }
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/false);
  if (methods.empty()) {
    if (!methodsNamed(cls, name, /*isStatic=*/true).empty())
      diags_.error(member->loc(), "'" + name + "' is a static method of '" + cls->name +
                                      "': call it through the class, '" + cls->name + "." + name +
                                      "(...)'");
    else
      diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                      cls->type->name() + "'");
    checkArgsLoosely();
    return call;
  }
  checkAccess(methods[0]->cls, methods[0]->access, name, member->loc());
  if (isInitThis(member->object) && !flow_.beforeSuper && !thisReady(flow_))
    diags_.error(member->loc(), "'this." + name + "()' is called before every field is set: field '" +
                                    firstUnset(flow_) + "' is not set yet, and the method may read it");
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = chooseVariant(methods, call->args, call->spread, member->loc(), checked, &bound);
  if (!sym || rejectAccessorCall(call, member, sym))
    return call;
  resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty())
    diags_.error(member->loc(), "'" + name + "' is not a generic method; remove the type arguments");
  // A generic method: a call of the copy made for its types, which no class
  // overrides.
  if (sym->isGeneric() && checked)
    return callGenericVariant(call, sym, bound, member->loc(), /*method=*/true);
  if (sym->isGeneric()) {
    checkGenericCall(call, sym, member->loc(), expected, member->typeArgs);
    call->method = call->func;
    call->func = nullptr;
    if (call->method)
      member->type = functionType(call->method);
    return call;
  }
  // A direct call, unless classes down the chain override the method: then
  // the method table of the object's class says which one runs.
  if (FunctionDecl *impl = soleImplementation(cls, sym->decl))
    call->method = impl->sym;
  else
    call->method = sym, call->virtualCall = true;
  member->type = functionType(sym);
  call->type = returnType(sym);
  // The arguments chooseVariant checked are only converted.
  std::vector<bool> given(checked ? call->args.size() : 0, true);
  return checkArguments(call, sym->params, sym->minArgs, sym->hasRest, member->loc(), &given);
}

Expr *Sema::checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                 const Type *expected) {
  call->type = Type::error();
  // `EventEmitter.once(emitter, name)`: the functions of node:events are
  // properties of EventEmitter too (sema_events.cpp).
  static const llvm::StringLiteral eventsFunctions[] = {
      "once", "on", "listenerCount", "getEventListeners", "getMaxListeners", "setMaxListeners"};
  if (cls->library && cls->name == "EventEmitter" &&
      llvm::is_contained(eventsFunctions, member->name)) {
    if (Expr *events = checkEventsFunction(call, member->name, member->loc()))
      return events;
    return checkDirectCall(call, libFunctions_.lookup(member->name)->sym, member->loc(), expected);
  }
  // `Readable.from(iterable)` (sema_stream.cpp).
  if (cls->library && cls->name == "Readable" && member->name == "from")
    return checkStreamFrom(call, member);
  std::vector<FunctionDecl *> methods = methodsNamed(cls, member->name, /*isStatic=*/true);
  if (methods.empty()) {
    // A static field that holds a function, or an error.
    call->callee = checkStaticMember(member, cls, nullptr);
    const Type *type = call->callee->type;
    if (type->isFunction())
      return checkValueCall(call, type);
    if (!type->isError())
      diags_.error(member->loc(), "'" + cls->name + "." + member->name +
                                      "' is not a function; it has type '" + type->name() + "'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  checkAccess(methods[0]->cls, methods[0]->access, member->name, member->loc());
  bool checked = false;
  std::vector<const Type *> bound;
  FuncSymbol *sym = chooseVariant(methods, call->args, call->spread, member->loc(), checked, &bound);
  if (!sym)
    return call;
  resolveParams(sym);
  if (!sym->isGeneric() && !member->typeArgs.empty())
    diags_.error(member->loc(), "'" + member->name + "' is not a generic method; remove the type arguments");
  // A generic static method, as a generic function.
  if (sym->isGeneric() && checked)
    return callGenericVariant(call, sym, bound, member->loc(), /*method=*/false);
  if (sym->isGeneric())
    return checkGenericCall(call, sym, member->loc(), expected, member->typeArgs);
  call->func = sym;
  call->type = returnType(sym);
  // The arguments chooseVariant checked are only converted.
  std::vector<bool> given(checked ? call->args.size() : 0, true);
  return checkArguments(call, sym->params, sym->minArgs, sym->hasRest, member->loc(), &given);
}

Expr *Sema::checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected) {
  expr->type = Type::error();
  if (cls->isGeneric())
    return checkGenericNew(expr, cls, expected);
  // As node's `new AbortSignal()`, which throws "Illegal constructor".
  if (cls == libraryClass("AbortSignal") && !inLibrary(expr->loc())) {
    diags_.error(expr->loc(), "an AbortSignal is not made with 'new'; take the signal of a "
                              "controller, 'new AbortController().signal', or use "
                              "'AbortSignal.timeout(ms)', 'AbortSignal.abort()' or "
                              "'AbortSignal.any(signals)'");
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    return expr;
  }
  if (isRuntimeClass(cls)) {
    std::string hint = cls == systemErrorClass_ || cls == execSyncErrorClass_
                           ? "; throw an Error, or an object of a class of your own that extends "
                             "Error"
                           : "";
    diags_.error(expr->loc(),
                 "'new " + cls->name + "' is not possible: only the functions of " +
                     std::string(cls == execSyncErrorClass_ ? "node:child_process" : "node:fs") +
                     " make " + cls->name + hint);
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    return expr;
  }
  expr->cls = cls;
  if (!expr->typeArgs.empty())
    diags_.error(expr->loc(), "class '" + cls->name + "' is not generic; remove the type arguments");
  if (!cls->staticOwner()->declared && loading_ && functions_.empty() &&
      declModules_.lookup(cls) == loading_)
    diags_.error(expr->loc(), "class '" + cls->name +
                                  "' is used before its declaration, which JavaScript has not run "
                                  "yet here (a ReferenceError); move the class up");
  if (cls->isAbstract)
    diags_.error(expr->loc(), "class '" + cls->name + "' is abstract and cannot be created: create "
                              "an object of a class that extends it");
  completeClass(cls);
  auto *call = ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->ctor = call;
  call->type = Type::voidTy();
  // Without a constructor of its own, a class takes those of the nearest
  // class up its chain that has any, as in JavaScript.
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  if (ctors.empty()) {
    if (!call->args.empty()) {
      diags_.error(expr->loc(), "class '" + cls->name + "' has no constructor, so 'new " +
                                    cls->name + "()' takes no arguments");
      for (Expr *&arg : call->args)
        arg = checkExpr(arg);
      return expr;
    }
  } else {
    checkAccess(ctors[0]->cls, ctors[0]->access, "constructor", expr->loc());
    bool checked = false;
    FuncSymbol *sym = chooseVariant(ctors, call->args, {}, expr->loc(), checked);
    if (!sym)
      return expr;
    resolveParams(sym);
    call->func = sym;
    std::vector<bool> given(checked ? call->args.size() : 0, true);
    checkArguments(call, sym->params, sym->minArgs, sym->hasRest, expr->loc(), &given);
  }
  for (const ClassDecl *c = cls; c; c = c->parent)
    if (c->init || !c->constructors.empty()) {
      calledFunction(); // the code of the class runs
      break;
    }
  expr->type = cls->type;
  return expr;
}

// `x instanceof C` compares the number of x's class with the range of
// numbers of C and the classes down its chain. An object whose type is not
// up C's chain (or down it) is never a C.
Expr *Sema::checkInstanceOf(BinaryExpr *bin) {
  bin->lhs = checkExpr(bin->lhs);
  bin->type = Type::boolTy();
  auto *name = dyn_cast<NameExpr>(bin->rhs);
  ClassDecl *cls = name ? lookupValue(name->name).cls : nullptr;
  // `x instanceof Date`, `Map` or `Set`: of a union, the members that are
  // one; anywhere else the type says it already. The right side gets the
  // type of those members, whose numbers the check compares (see
  // CodeGen::emitInstanceOf).
  for (llvm::StringRef builtin : {"Date", "Map", "Set"}) {
    if (cls || !isGlobalName(bin->rhs, builtin))
      continue;
    auto is = [&](const Type *t) {
      return builtin == "Date" ? t->isDate() : builtin == "Map" ? t->isMap() : t->isSet();
    };
    const Type *type = bin->lhs->type;
    const Type *base = type->nonOptional();
    std::string what = "'" + builtin.str() + "'";
    // An unknown or an object may hold a Date, which the check shows; the
    // types in a Map or a Set no check can tell.
    if (base->isUnknown() || base->isAnyObject()) {
      if (builtin == "Date")
        bin->rhs->type = Type::date();
      else
        diags_.error(bin->loc(), "'instanceof " + builtin.str() +
                                     "' cannot tell the types of the keys and values, which "
                                     "tz needs to use a " + builtin.str() +
                                     "; keep it in a variable of its type, e.g. '" +
                                     (builtin == "Map" ? "Map<string, int>" : "Set<string>") +
                                     "'");
      return bin;
    }
    if (base->isUnion()) {
      std::vector<const Type *> matching;
      for (const Type *m : base->members())
        if (is(m))
          matching.push_back(m);
      if (matching.empty())
        diags_.error(bin->loc(), "a '" + base->name() + "' is never a " + what +
                                     ": none of its members is one, so this is always false");
      else if (matching.size() == base->members().size() && !type->isOptional())
        diags_.error(bin->loc(), "every member of '" + base->name() + "' is a " + what +
                                     ", so 'instanceof " + builtin.str() + "' is always true");
      else
        bin->rhs->type = Type::unionOf(matching);
      return bin;
    }
    if (is(type))
      diags_.error(bin->loc(), "the value is a '" + type->name() + "', so 'instanceof " +
                                   builtin.str() + "' is always true");
    else if (type->isOptional() && is(type->inner()))
      diags_.error(bin->loc(), "the value is a '" + type->inner()->name() +
                                   "' or undefined; test it with '!= undefined'");
    else if (!type->isError())
      diags_.error(bin->loc(), "a '" + type->name() + "' is never a " + what +
                                   ", so 'instanceof " + builtin.str() + "' is always false");
    return bin;
  }
  // `x instanceof Uint8Array` and the other types of binary data, as for
  // Date; but a Uint8Array may hold a Buffer, which only the check tells.
  if (const Type *bytes = !cls && name && !lookup(name->name).var && !lookup(name->name).func
                              ? bytesTypeNamed(name->name)
                              : nullptr)
    return checkBytesInstanceOf(bin, bytes);
  if (!cls) {
    // A class as a value: one known when compiling is that class; of one
    // known only when the program runs, its number is compared then
    // (docs/decorators.md, section 6), and the value is a T.
    bin->rhs = checkExpr(bin->rhs);
    const Type *rhs = bin->rhs->type;
    if (ClassDecl *known = const_cast<ClassDecl *>(rhs->classRef()); known && known->type)
      cls = known;
    else if (rhs->isConstructor()) {
      const Type *base = bin->lhs->type->nonOptional();
      if (!base->isError() && !base->isUnknown() && !isObjectReference(base))
        diags_.error(bin->lhs->loc(), "'instanceof' tests an object, found '" +
                                          bin->lhs->type->name() +
                                          "', which is never an instance of a class");
      return bin;
    } else {
      if (!rhs->isError())
        diags_.error(bin->rhs->loc(), "the right side of 'instanceof' must be a class");
      return bin;
    }
  }
  // A generic class: the instance the type of the value says, which is a
  // class of its own (of a union, the members that are one).
  if (cls->isGeneric()) {
    const Type *type = bin->lhs->type;
    bin->rhs->type = Type::error();
    if (type->isError())
      return bin;
    const Type *base = type->nonOptional();
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    std::vector<const Type *> matching;
    for (const Type *m : members)
      if (genericInstanceOf(m, cls))
        matching.push_back(m);
    if (matching.empty()) {
      // An unknown or an object may be any of them.
      bool related = base->isUnknown() || base->isAnyObject() ||
                     (base->isObject() && cls->type->isSubtypeOf(base));
      for (const Type *m : members)
        related |= m->isObject() && cls->type->isSubtypeOf(m);
      if (related) {
        std::string generic = cls->name.str();
        std::string hint;
        if (cls->library) // an emitter of the library: a class that extends it
          hint = "test for a class that extends it, e.g. 'x instanceof Bus' with 'class Bus "
                 "extends " + generic + " {}'";
        else
          hint = "keep the value in a variable of a union of the classes it may be, e.g. '" +
                 generic + "<int> | " +
                 (base->isUnknown() || base->isAnyObject() ? generic + "<string>"
                                                           : base->name().str()) +
                 "'";
        diags_.error(bin->loc(), "'instanceof " + generic + "' cannot tell which '" + generic +
                                     "' a '" + base->name() + "' would be: each list of type "
                                     "arguments of '" + generic + "' makes a class of its own; " +
                                     hint);
      } else {
        diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                     "', so this is always false");
      }
      return bin;
    }
    if (base->isUnion())
      bin->rhs->type = matching.size() == 1 ? matching[0] : Type::unionOf(matching);
    else
      bin->rhs->type = genericInstanceOf(base, cls)->type;
    return bin;
  }
  bin->rhs->type = cls->type;
  const Type *type = bin->lhs->type;
  if (type->isError())
    return bin;
  const Type *base = type->nonOptional();
  // An unknown or an object may be of any class (see addUnknownFacts).
  if (base->isUnknown() || base->isAnyObject())
    return bin;
  // Of a union: some member must be able to be one (docs/objects.md,
  // section 7); then the check is one of the class numbers.
  if (base->isUnion()) {
    bool may = llvm::any_of(base->members(), [&](const Type *m) {
      return m->isObject() && (cls->type->isSubtypeOf(m) || m->isSubtypeOf(cls->type));
    });
    if (!may)
      diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                   "': none of its members is or extends it, so this is always "
                                   "false");
    return bin;
  }
  if (!base->isObject()) {
    diags_.error(bin->lhs->loc(), "'instanceof' tests an object, found '" + type->name() +
                                      "', which is never an instance of a class");
    return bin;
  }
  if (!cls->type->isSubtypeOf(base) && !base->isSubtypeOf(cls->type)) {
    if (base->isClass())
      diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                   "': classes are told apart by their names, and neither extends "
                                   "the other, so this is always false");
    else if (!base->bases().empty() || base->isBase())
      diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name + "': '" +
                                   cls->name + "' does not implement it, so this is always false");
    else
      diags_.error(bin->loc(), "an object of type '" + base->name() + "' is never a '" + cls->name +
                                   "': only 'new " + cls->name +
                                   "(...)' creates one, so this is always false");
  }
  return bin;
}

Expr *Sema::checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes) {
  using Bytes = Type::Bytes;
  // What a value of type t is for sure, and what it may be.
  auto is = [&](const Type *t) {
    return t->isBytes() &&
           (t->bytes() == bytes->bytes() || (bytes->bytes() == Bytes::Uint8 && t->isBuffer()));
  };
  auto may = [&](const Type *t) { return bytes->isBuffer() && t->bytes() == Bytes::Uint8; };
  const Type *type = bin->lhs->type;
  if (type->isError())
    return bin;
  const Type *base = type->nonOptional();
  std::string what = "'" + bytes->name().str() + "'";
  // The right side gets the types the check compares the number of the
  // object with (see CodeGen::emitInstanceOf): the members that are one,
  // or Buffer.
  bin->rhs->type = bytes;
  // An unknown or an object may be one (see addUnknownFacts).
  if (base->isUnknown() || base->isAnyObject())
    return bin;
  if (base->isUnion()) {
    std::vector<const Type *> matching;
    bool maybe = false;
    for (const Type *m : base->members()) {
      if (is(m))
        matching.push_back(m);
      maybe |= may(m);
    }
    if (matching.empty() && !maybe)
      diags_.error(bin->loc(), "a '" + base->name() + "' is never a " + what +
                                   ": none of its members is one, so this is always false");
    else if (matching.size() == base->members().size() && !type->isOptional())
      diags_.error(bin->loc(), "every member of '" + base->name() + "' is a " + what +
                                   ", so 'instanceof " + bytes->name() + "' is always true");
    else if (!bytes->isBuffer())
      bin->rhs->type = Type::unionOf(matching);
    return bin;
  }
  if (is(type))
    diags_.error(bin->loc(), "the value is a '" + type->name() + "', so 'instanceof " +
                                 bytes->name() + "' is always true");
  else if (type->isOptional() && is(type->inner()))
    diags_.error(bin->loc(), "the value is a '" + type->inner()->name() +
                                 "' or undefined; test it with '!= undefined'");
  else if (!may(base))
    diags_.error(bin->loc(), "a '" + type->name() + "' is never a " + what +
                                 ", so 'instanceof " + bytes->name() + "' is always false");
  return bin;
}

//===----------------------------------------------------------------------===//
// Overloads
//===----------------------------------------------------------------------===//

FuncSymbol *Sema::chooseVariant(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                                const std::vector<bool> &spread, llvm::SMLoc loc, bool &checked,
                                std::vector<const Type *> *bound) {
  checked = false;
  auto checkLoosely = [&] {
    for (Expr *&arg : args)
      if (!arg->type)
        arg = checkExpr(arg);
  };
  if (variants.size() == 1)
    return variants[0]->sym;
  const FunctionDecl *first = variants[0];
  std::string what = first->isConstructor ? "the constructor of '" + first->cls->name.str() + "'"
                                          : "'" + first->name.str() + "'";
  for (FunctionDecl *v : variants)
    resolveParams(v->sym);
  if (llvm::is_contained(spread, true)) {
    diags_.error(loc, "a spread argument cannot choose among the variants of " + what +
                          "; pass the arguments one by one");
    checkLoosely();
    return nullptr;
  }
  size_t count = args.size();
  std::vector<FunctionDecl *> fit;
  for (FunctionDecl *v : variants) {
    const FuncSymbol *sym = v->sym;
    if (count >= sym->minArgs && (sym->hasRest || count <= sym->params.size()))
      fit.push_back(v);
  }
  if (fit.empty()) {
    diags_.error(loc, "no variant of " + what + " takes " + std::to_string(count) + " argument" +
                          (count == 1 ? "" : "s") + ": there are " + variantList(variants));
    checkLoosely();
    return nullptr;
  }
  if (fit.size() == 1)
    return fit[0]->sym;

  // The arguments, each with the type of its parameter if the variants agree
  // on it (and it is no type parameter of a generic variant).
  for (size_t i = 0; i < count; ++i) {
    const Type *expected = parameterAt(fit[0]->sym, i);
    for (FunctionDecl *v : fit)
      if (parameterAt(v->sym, i) != expected || (expected && typeMentions(expected, v->sym->typeParams)))
        expected = nullptr;
    // A literal with a fraction is a double, or a decimal where a variant
    // takes one there and none takes a double: `m.times(1.5)` with
    // `times(k: int)` and `times(k: decimal)`.
    if (!expected && isLiteralTree(args[i]) && hasFloatLiteral(args[i])) {
      bool decimal = false, dbl = false;
      for (FunctionDecl *v : fit) {
        const Type *param = parameterAt(v->sym, i)->nonOptional();
        decimal |= param->isDecimal();
        dbl |= param->isFloat();
      }
      if (decimal && !dbl)
        expected = Type::decimalTy();
    }
    args[i] = checkExpr(args[i], expected && !expected->isError() ? expected : nullptr);
  }
  checked = true;
  if (llvm::any_of(args, [](const Expr *arg) { return arg->type->isError(); }))
    return nullptr;

  struct Candidate {
    FunctionDecl *fn;
    std::vector<int> ranks;
    bool leavesOut; // takes defaults, optional parameters or a rest parameter
    std::vector<const Type *> typeArgs; // of a generic variant
  };
  std::vector<Candidate> applicable;
  std::string why;
  for (FunctionDecl *v : fit) {
    Candidate c{v, {}, v->sym->hasRest || count < v->sym->params.size(), {}};
    // A generic variant takes the types its arguments give its type
    // parameters.
    if (v->sym->isGeneric() && !inferFromArguments(v->sym, args, c.typeArgs)) {
      why += "; " + variantText(v) + " cannot take these arguments for its type parameters";
      continue;
    }
    std::string mismatch;
    for (size_t i = 0; i < count && mismatch.empty(); ++i) {
      const Type *param = parameterAt(v->sym, i);
      if (v->sym->isGeneric())
        param = substitute(param, v->sym->typeParams, c.typeArgs);
      int rank = conversionRank(args[i]->type, param);
      if (rank < 0)
        mismatch = variantText(v) + " takes '" + param->name().str() + "' as argument " +
                   std::to_string(i + 1) + ", which is '" + args[i]->type->name().str() + "'";
      c.ranks.push_back(rank);
    }
    if (mismatch.empty())
      applicable.push_back(std::move(c));
    else
      why += "; " + mismatch;
  }
  if (applicable.empty()) {
    diags_.error(loc, "no variant of " + what + " fits these arguments" + why);
    return nullptr;
  }
  auto better = [&](const Candidate &a, const Candidate &b) {
    bool strictly = false;
    for (size_t i = 0; i < count; ++i) {
      if (a.ranks[i] > b.ranks[i])
        return false;
      strictly |= a.ranks[i] < b.ranks[i];
    }
    bool generic = a.fn->sym->isGeneric(), otherGeneric = b.fn->sym->isGeneric();
    return strictly || (!a.leavesOut && b.leavesOut) || (!generic && otherGeneric);
  };
  for (const Candidate &c : applicable)
    if (llvm::all_of(applicable,
                     [&](const Candidate &other) { return &other == &c || better(c, other); })) {
      if (c.fn->sym->isGeneric()) {
        if (!bound) {
          diags_.error(loc, "the call chooses " + variantText(c.fn) + ", a generic variant, which " +
                                "cannot be chosen here");
          return nullptr;
        }
        *bound = c.typeArgs;
      }
      return c.fn->sym;
    }
  std::vector<FunctionDecl *> tied;
  for (const Candidate &c : applicable)
    tied.push_back(c.fn);
  diags_.error(loc, "the call fits several variants of " + what + " equally well: " +
                        variantList(tied) + "; convert an argument to choose one");
  return nullptr;
}

Expr *Sema::callGenericVariant(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                               llvm::SMLoc loc, bool method) {
  llvm::ArrayRef<const Type *> typeParams = sym->typeParams;
  std::vector<const Type *> params;
  llvm::SMLoc outerLoc = std::exchange(instanceLoc_, loc);
  for (const Type *p : sym->params)
    params.push_back(substitute(p, typeParams, bound));
  const Type *ret = returnType(sym);
  call->type = ret ? substitute(ret, typeParams, bound) : Type::error();
  instanceLoc_ = outerLoc;
  FuncSymbol *callee = sym;
  const ClassDecl *cls = sym->decl->cls;
  if (!trial_ && !sym->inferring && sym->ret && (!cls || sym->decl->isStatic || cls->isEmitted()))
    if (FuncSymbol *instance = instantiate(sym, bound, loc))
      callee = instance;
  (method ? call->method : call->func) = callee;
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && method)
    member->type = functionType(callee);
  // The arguments chooseVariant checked are only converted.
  std::vector<bool> given(call->args.size(), true);
  return checkArguments(call, params, sym->minArgs, sym->hasRest, loc, &given);
}

FuncSymbol *Sema::variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                                 const MemberExpr *member) {
  if (variants.size() == 1)
    return variants[0]->sym;
  std::vector<FunctionDecl *> matching;
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && wanted->isFunction()) {
    for (FunctionDecl *v : variants)
      if (functionType(v->sym) == wanted)
        matching.push_back(v);
  } else if (callback_) {
    // A callback, `[1, 2].map(v.add)`: the variant that takes what the
    // method passes.
    for (FunctionDecl *v : variants) {
      const FuncSymbol *sym = v->sym;
      bool fits = !sym->hasRest && sym->minArgs <= callback_->params.size() &&
                  sym->params.size() <= callback_->params.size();
      for (size_t i = 0; i < sym->params.size() && fits; ++i) {
        const Type *param = sym->params[i], *given = callback_->params[i];
        fits = param == given || (param->isOptional() && param->inner() == given);
      }
      if (fits)
        matching.push_back(v);
    }
  }
  if (matching.size() == 1)
    return matching[0]->sym;
  const FunctionDecl *last = variants.back();
  std::string type = "(";
  for (size_t i = 0; i < last->params.size() && i < last->sym->params.size(); ++i)
    type += (i == 0 ? "" : ", ") + last->params[i].label().str() + ": " +
            last->sym->params[i]->name().str();
  type += ") => " + returnType(last->sym)->name().str();
  std::string object = "o";
  if (auto *name = dyn_cast<NameExpr>(member->object))
    object = name->name.str();
  diags_.error(member->loc(), "'" + member->name + "' has several variants, " +
                                  variantList(variants) +
                                  ": say which one with the type of the place it goes, e.g. "
                                  "'const f: " + type + " = " + object + "." + member->name + "'");
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Object literals with methods
//===----------------------------------------------------------------------===//

Expr *Sema::checkMethodLiteral(ObjectLitExpr *lit, const Type *target) {
  lit->type = Type::error();
  if (target && target->isAnyObject())
    target = nullptr;
  if (lit->hasSpread()) {
    diags_.error(lit->loc(), "an object literal with methods cannot take the fields of another "
                             "object with '...'; write them out");
    for (ObjectLitExpr::Property &prop : lit->props)
      prop.value = checkExpr(prop.value);
    return lit;
  }
  auto *cls = ctx_.make<ClassDecl>(lit->loc(), "");
  cls->literal = lit;
  cls->id = ++classCount_;
  lit->cls = cls;
  Type *type = Type::classType("", cls);
  cls->type = type;
  cls->resolved = cls->declared = true;
  classes_.push_back(cls);

  // The fields have the types of their values, in the order written.
  bool ok = true;
  std::vector<Field> fields;
  for (ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method) {
      FunctionDecl *fn = cast<FunctionExpr>(prop.value)->fn;
      fn->name = prop.name;
      fn->cls = cls;
      if (!fn->typeParams.empty()) {
        diags_.error(fn->loc(), "a method of an object literal cannot be generic; declare a class, "
                                "whose methods can");
        fn->typeParams.clear();
      }
      cls->methods.push_back(fn);
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(prop.value); fe && fe->fn->name.empty())
      fe->fn->name = prop.name;
    std::string what = ("field '" + prop.name + "'").str();
    // The interface expected gives the type, as for a literal without methods.
    const Field *wanted = target ? target->field(prop.name) : nullptr;
    prop.value = checkExpr(prop.value, wanted ? wanted->type : nullptr);
    if (wanted)
      prop.value = coerce(prop.value, wanted->type, what);
    const Type *valueType =
        wanted && !prop.value->type->isError() ? wanted->type : prop.value->type;
    if (valueType->isVoid()) {
      diags_.error(prop.value->loc(), "an expression of type 'void' cannot be the value of " + what);
      ok = false;
    } else if (valueType->isUndefined()) {
      diags_.error(prop.value->loc(), "cannot infer the type of " + what + " from 'undefined'");
      ok = false;
    }
    ok &= !valueType->isError();
    ClassField field;
    field.name = prop.name;
    field.loc = prop.loc;
    field.resolved = valueType;
    cls->fields.push_back(field);
    fields.push_back({prop.name.str(), valueType, false});
  }
  if (!ok)
    return lit;
  type->defineFields(fields);
  std::string name = objectTypeSpelling(fields);
  std::string methods;
  for (FunctionDecl *fn : cls->methods)
    methods += (methods.empty() ? "" : "; ") + fn->name.str() + "()";
  name = fields.empty() ? "{ " + methods + " }" : name.substr(0, name.size() - 2) + "; " + methods + " }";
  type->rename(name);

  // The methods share one environment, the object: created with it, they
  // see what is visible where it is written.
  NestedGroup *group = ctx_.newGroup();
  group->owner = currentDecl();
  group->scopeDepth = scopes_.size();
  group->functionDepth = functions_.size();
  group->creationSeq = ++seq_;
  group->finished = true;
  group->members = cls->methods;
  cls->group = group;
  for (FunctionDecl *fn : cls->methods) {
    fn->group = group;
    fn->parent = currentDecl();
    fn->creationSeq = group->creationSeq;
    // A method of the interface expected gives the types of the parameters
    // and the result, as it gives them to an arrow.
    const Field *wanted = target ? target->field(fn->name) : nullptr;
    if (wanted && wanted->type->isFunction()) {
      declareTyped(fn, wanted->type);
      if (fn->returnType)
        fn->sym->ret = resolveReturnType(fn, fn->sym);
    } else {
      declareFunction(fn, nullptr, /*scoped=*/false);
    }
  }
  findCycles(cls->methods, [](FunctionDecl *, llvm::StringRef) -> FunctionDecl * { return nullptr; });
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->sym->ret)
      inferReturnType(fn->sym);
    if (fn->checked)
      continue;
    BodyState saved = enterBody(fn);
    checkFunctionBody(fn);
    leaveBody(saved);
  }
  lit->type = type;
  if (target && implementsLiteral(lit, target))
    addBase(type, target);
  return lit;
}

// Whether the literal has exactly the members of interface `target`: fields
// of the same types, and methods where it has functions it cannot replace.
// Reports the first difference.
bool Sema::implementsLiteral(ObjectLitExpr *lit, const Type *target) {
  const ClassDecl *cls = lit->cls;
  auto fail = [&](llvm::SMLoc loc, const std::string &why) {
    diags_.error(loc, "the object literal does not fit '" + target->name() + "': " + why);
    lit->type = Type::error();
    return false;
  };
  for (const ObjectLitExpr::Property &prop : lit->props) {
    const Field *member = target->field(prop.name);
    if (!member)
      return fail(prop.loc, "it has no field '" + prop.name.str() +
                                "'; an object must have exactly the fields of its type");
    if (!prop.method)
      continue;
    FunctionDecl *fn = cast<FunctionExpr>(prop.value)->fn;
    if (!member->type->isFunction())
      return fail(prop.loc, "'" + prop.name.str() + "' is a field of type '" +
                                member->type->name().str() + "' there, not a method");
    if (!member->readonly)
      return fail(prop.loc, "'" + prop.name.str() + "' is a field there that code may replace, "
                                "which a method cannot be; write it as a field: '" +
                                prop.name.str() + ": (...) => ...'");
    const Type *type = functionType(fn->sym);
    if (type != member->type && !type->isError())
      return fail(prop.loc, "'" + prop.name.str() + "' is '" + type->name().str() + "' here and '" +
                                member->type->name().str() + "' there");
  }
  for (const Field &member : target->fields())
    if (!cls->field(member.name) && !llvm::any_of(cls->methods, [&](const FunctionDecl *fn) {
          return fn->name == member.name;
        }))
      return fail(lit->loc(), "it has no '" + member.name + "'");
  if (!target->overloads().empty())
    return fail(lit->loc(), "its method '" + target->overloads()[0].name +
                                "' has several signatures, and only a class implements those, "
                                "each with a variant of its own");
  return true;
}

} // namespace tz
