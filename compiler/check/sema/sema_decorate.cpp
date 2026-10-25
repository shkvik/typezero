// Decorators (docs/decorators.md) and what they are made of: classes as
// values, functions with a `this` parameter, and lists of parameters
// (`A extends unknown[]`, docs/generics.md section 9).

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

//===----------------------------------------------------------------------===//
// Lists of parameters and `this`
//===----------------------------------------------------------------------===//

// `f.call(obj, a, b)` and `f.apply(obj, args)` of a function with a `this`
// parameter: a call of the function value with obj first. apply takes the
// arguments as the list of parameters the function ends with.
Expr *Sema::checkThisCall(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  llvm::StringRef name = member->name;
  if (name != "call" && name != "apply") {
    diags_.error(member->loc(), name == "bind"
                                    ? "'bind' is not supported: pass the 'this' with every call, "
                                      "'f.call(obj, ...)'"
                                    : "a function has no property '" + name.str() +
                                          "'; call it with '.call(obj, ...)' or '.apply(obj, args)'");
    checkArgsLoosely();
    return call;
  }
  if (call->args.empty()) {
    diags_.error(member->loc(), "'" + name + "' takes the 'this' of the call first, e.g. 'f." +
                                    name + "(this" + (name == "apply" ? ", args)'" : ", ...)'"));
    return call;
  }
  if (name == "apply") {
    if (call->args.size() != 2 || call->hasSpread()) {
      diags_.error(member->loc(), "'apply' takes two arguments: the 'this' of the call and the list "
                                  "of the arguments, e.g. 'f.apply(this, args)'");
      checkArgsLoosely();
      return call;
    }
    // As `f(obj, ...args)` would pass the list: an argument list, or a
    // tuple in a variable, element by element (see expandListSpreads).
    call->spread = {false, true};
    expandListSpreads(call->args, call->spread);
  }
  call->callee = member->object;
  call->type = type->ret();
  return checkArguments(call, type->params(), type->minArgs(), type->hasRest(), member->loc());
}

Expr *Sema::checkListMethod(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (member->name != "join") {
    diags_.error(member->loc(), "a list of parameters has no method '" + member->name +
                                    "' here; it has 'length' and 'join', and goes on with "
                                    "'...args'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  // Each copy checks that its elements can be joined (checkTupleJoin).
  if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 1))
    return call;
  if (!call->args.empty())
    call->args[0] = checkArg(call->args[0], Type::string());
  call->type = Type::string();
  return call;
}

// `t.join(sep)`: the elements of the tuple as String() makes them (an
// undefined one empty, as join does), with sep between them.
Expr *Sema::checkTupleJoin(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 1))
    return call;
  if (!call->args.empty())
    call->args[0] = checkArg(call->args[0], Type::string());
  const Type *tuple = member->object->type;
  for (size_t i = 0; i < tuple->fields().size(); ++i) {
    const Type *element = tuple->fields()[i].type->nonOptional();
    if (element->isNumeric() || element->isDecimal() || element->isBool() ||
        element->isString() || element->isUndefined())
      continue;
    diags_.error(member->loc(), "'join' turns each element into a string, and element " +
                                    std::to_string(i) + " of '" + tuple->name() + "' is '" +
                                    element->name() +
                                    "', which cannot be one; build the text from the elements");
    return call;
  }
  call->builtin = Builtin::TupleJoin;
  call->type = Type::string();
  return call;
}

//===----------------------------------------------------------------------===//
// Classes as values
//===----------------------------------------------------------------------===//

Expr *Sema::checkClassValue(NameExpr *name, ClassDecl *cls) {
  // Before its declaration has run, JavaScript has no class there yet (a
  // ReferenceError), as for `new C()`.
  if (!cls->declared && loading_ && functions_.empty() && declModules_.lookup(cls) == loading_)
    diags_.error(name->loc(), "class '" + cls->name +
                                  "' is used before its declaration, which JavaScript has not run "
                                  "yet here (a ReferenceError); move the class up");
  // The class a decorator gave in its place, from where it is declared on.
  cls = finalClass(cls);
  name->cls = cls;
  name->type = classValueType(cls);
  cls->usedAsValue = true;
  return name;
}

// The value of `object`, a class as a value, is not needed for `result`,
// which is about the class it is: of a name, nothing; of anything else, the
// expression is evaluated first.
static Expr *afterClassValue(AstContext &ctx, Expr *object, Expr *result) {
  if (isa<NameExpr>(object))
    return result;
  auto *seq = ctx.make<SequenceExpr>(result->loc(), std::vector<Expr *>{object, result});
  seq->type = result->type;
  return seq;
}

Expr *Sema::checkClassRefMember(MemberExpr *member) {
  const Type *type = member->object->type;
  const ClassDecl *cls = type->classRef();
  member->type = Type::error();
  if (member->name == "prototype") {
    diags_.error(member->loc(), "there are no prototypes in tz: the methods belong to the class "
                                "itself, and a decorator gets the class; use it as it is");
    return member;
  }
  if (!cls) {
    // A class known only when the program runs: its name is in its object.
    if (member->name == "name") {
      auto *call = ctx_.make<CallExpr>(member->loc(), member, std::vector<Expr *>{member->object});
      call->builtin = Builtin::ClassName;
      call->type = Type::string();
      return call;
    }
    diags_.error(member->loc(), "'" + type->name() + "' is some class known only when the program "
                                "runs, which has only its 'name'; make an object of it with 'new', "
                                "or use reflection (docs/decorators.md, section 6)");
    return member;
  }
  if (member->name == "name" && (!cls->type || methodsNamed(cls, "name", true).empty())) {
    // `cls.name`, as in JavaScript: the class's name.
    auto *text = ctx_.make<StringLitExpr>(member->loc(),
                                          std::u16string(cls->name.begin(), cls->name.end()));
    text->type = Type::string();
    return afterClassValue(ctx_, member->object, text);
  }
  if (!cls->type) {
    diags_.error(member->loc(), "'" + cls->name + "' as a value has only its 'name' in tz");
    return member;
  }
  // A static member of the class it is, known when compiling.
  Expr *object = member->object;
  return afterClassValue(ctx_, object,
                         checkStaticMember(member, const_cast<ClassDecl *>(cls), nullptr));
}

//===----------------------------------------------------------------------===//
// Applying decorators
//===----------------------------------------------------------------------===//

namespace {

// What a decorator gets where it stands, as TypeScript's __decorate passes it
// (docs/decorators.md, section 1): the class; the class and the name of a
// field; those and the descriptor of a method; or the class, the name of the
// method (undefined for a constructor) and the position of a parameter.
enum class Place { Class, Field, Method, Parameter };

size_t argumentsOf(Place place) {
  switch (place) {
  case Place::Class: return 1;
  case Place::Field: return 2;
  default: return 3;
  }
}

const char *placeText(Place place) {
  switch (place) {
  case Place::Class: return "a class";
  case Place::Field: return "a field";
  case Place::Method: return "a method";
  default: return "a parameter";
  }
}

const char *argumentsText(Place place) {
  switch (place) {
  case Place::Class: return "the class";
  case Place::Field: return "the class and the name of the field";
  case Place::Method: return "the class, the name of the method and its descriptor";
  default:
    return "the class, the name of the method (undefined for a constructor) and the position of "
           "the parameter";
  }
}

// `Log`, `Table`, `Reflect.metadata`: a decorator in messages, and what
// tells two apart.
std::string decoratorName(const Expr *expr) {
  if (auto *call = dyn_cast<CallExpr>(expr))
    return decoratorName(call->callee);
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->name.starts_with("%") ? "" : name->name.str();
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    std::string object = decoratorName(member->object);
    return object.empty() ? "" : object + "." + member->name.str();
  }
  return "";
}

// A decorator used as it is, which no call makes: `@Log`, `@rules.required`.
bool isPlainReference(const Expr *expr) {
  if (isa<NameExpr>(expr))
    return true;
  if (auto *member = dyn_cast<MemberExpr>(expr))
    return isPlainReference(member->object);
  return false;
}

bool decoratedParams(const FunctionDecl *fn) {
  return llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
}

} // namespace

Expr *Sema::classValueAt(ClassDecl *cls, llvm::SMLoc loc) {
  auto *name = ctx_.make<NameExpr>(loc, cls->name);
  name->cls = cls;
  name->type =
      Type::classRef(cls, cls->name.empty() && cls->type ? cls->type->name() : cls->name);
  cls->usedAsValue = true;
  return name;
}

const Type *Sema::methodDescriptorType(const Type *args, const Type *ret) {
  const Type *value =
      Type::function({Type::unknown(), args}, 1, /*hasRest=*/true, ret, /*hasThis=*/true);
  const Type *flag = Type::boolTy();
  const Type *type = Type::object({{"value", value, false},
                                   {"writable", flag, false},
                                   {"enumerable", flag, false},
                                   {"configurable", flag, false}},
                                  "MethodDescriptor<" + args->name().str() + ", " +
                                      ret->name().str() + ">");
  descriptorOrigins_.try_emplace(type, args, ret);
  return type;
}

bool Sema::decoratorParams(Expr *callee, std::vector<const Type *> &params) {
  const FuncSymbol *sym = nullptr;
  if (auto *name = dyn_cast<NameExpr>(callee)) {
    ScopeEntry entry = lookup(name->name);
    if (entry.var) {
      const Type *type = entry.var->type;
      if (!type || !type->isFunction() || type->hasRest())
        return false;
      params.assign(type->params().begin(), type->params().end());
      return true;
    }
    sym = entry.func;
  } else if (auto *member = dyn_cast<MemberExpr>(callee)) {
    // A static method of a class.
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? lookup(object->name).cls : nullptr) {
      std::vector<FunctionDecl *> methods = methodsNamed(cls, member->name, /*isStatic=*/true);
      if (methods.size() == 1)
        sym = methods[0]->sym;
    }
  }
  if (!sym)
    return false;
  resolveParams(const_cast<FuncSymbol *>(sym));
  if (sym->hasRest || llvm::is_contained(sym->params, nullptr))
    return false;
  params = sym->params;
  return true;
}

void Sema::checkDecoratedNames(ClassDecl *cls) {
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
    for (const Decorator &d : decorators)
      m.decorators.push_back(decoratorName(d.expr));
    if (fn)
      for (const Param &p : fn->params)
        for (const Decorator &d : p.decorators)
          m.decorators.push_back(decoratorName(d.expr));
    if (!m.decorators.empty())
      members.push_back(std::move(m));
  };
  for (const ClassField &f : cls->fields)
    add(f.name, f.loc, f.isStatic, f.decorators, nullptr);
  for (const FunctionDecl *fn : cls->methods)
    add(fn->name, fn->loc(), fn->isStatic, fn->decorators, fn);
  for (size_t i = 0; i < members.size(); ++i)
    for (size_t j = 0; j < i; ++j) {
      const Member &a = members[j], &b = members[i];
      if (a.name != b.name)
        continue;
      std::string what = a.isStatic != b.isStatic
                             ? "both the static member '" + a.name.str() + "' and the member '" +
                                   a.name.str() + "'"
                             : "two variants of '" + a.name.str() + "'";
      std::string shared;
      for (const std::string &d : b.decorators)
        if (!d.empty() && llvm::is_contained(a.decorators, d))
          shared = d;
      std::string records = shared.empty() ? "the records 'design:*' the compiler writes"
                                           : "the records of '@" + shared + "'";
      diags_.error(b.loc, what + " of '" + cls->name.str() + "' are decorated, and " +
                              records + " for both would be one: metadata is kept on the class "
                              "itself, by the member's name (docs/decorators.md, section 3); "
                              "give one of them another name");
      diags_.note(a.loc, "the other one is here");
      break;
    }
  // The decorators of the parameters of a constructor write on the class
  // itself, and the compiler writes its 'design:paramtypes' there: of two
  // constructors, those would be one (docs/overloads.md, section 6).
  const FunctionDecl *first = nullptr;
  for (const FunctionDecl *ctor : cls->constructors) {
    if (!decoratedParams(ctor))
      continue;
    if (!first) {
      first = ctor;
      continue;
    }
    diags_.error(ctor->loc(), "two constructors of '" + cls->name.str() +
                                  "' have decorated parameters, and the record "
                                  "'design:paramtypes' the compiler writes for the class would be "
                                  "one: metadata is kept on the class itself (docs/decorators.md, "
                                  "section 3); decorate the parameters of one of them");
    diags_.note(first->loc(), "the other one is here");
    break;
  }
}

void Sema::decorateClass(ClassDecl *cls) {
  checkDecoratedNames(cls);
  struct Member {
    ClassField *field;
    FunctionDecl *method;
    bool isStatic;
    const char *at;
  };
  std::vector<Member> members;
  for (ClassField &f : cls->fields)
    if (!f.decorators.empty())
      members.push_back({&f, nullptr, f.isStatic, f.loc.getPointer()});
  for (FunctionDecl *fn : cls->methods)
    if (!fn->decorators.empty() || decoratedParams(fn))
      members.push_back({nullptr, fn, fn->isStatic, fn->loc().getPointer()});
  // As TypeScript applies them: the members of the instances, then the
  // static ones, each in the order written; then the class.
  llvm::stable_sort(members, [](const Member &a, const Member &b) {
    return std::make_pair(a.isStatic, a.at) < std::make_pair(b.isStatic, b.at);
  });
  for (Member &m : members) {
    std::vector<DecoratorUse> uses;
    for (const Decorator &d : m.field ? m.field->decorators : m.method->decorators)
      uses.push_back({&d});
    if (m.method)
      for (size_t i = 0; i < m.method->params.size(); ++i)
        for (const Decorator &d : m.method->params[i].decorators)
          uses.push_back({&d, int(i), m.method});
    decorate(cls, m.field, m.method, uses);
  }
  // The class gets the decorators of the parameters of its constructor.
  std::vector<DecoratorUse> uses;
  for (const Decorator &d : cls->decorators)
    uses.push_back({&d});
  for (const FunctionDecl *ctor : cls->constructors)
    for (size_t i = 0; i < ctor->params.size(); ++i)
      for (const Decorator &d : ctor->params[i].decorators)
        uses.push_back({&d, int(i), ctor});
  decorate(cls, nullptr, nullptr, uses);
}

void Sema::decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                    llvm::ArrayRef<DecoratorUse> uses) {
  if (uses.empty())
    return;
  std::vector<Stmt *> &out = cls->decorations;
  Place own = field ? Place::Field : method ? Place::Method : Place::Class;
  llvm::StringRef memberName = field ? field->name : method ? method->name : llvm::StringRef();
  llvm::SMLoc at = uses[0].decorator->loc;
  if (field && field->accessor) {
    diags_.error(at, "a decorator cannot decorate an 'accessor' field: in tz it is an ordinary "
                     "field, and reading or writing it never runs code");
    return;
  }
  if (method && method->isAbstract) {
    diags_.error(at, "a decorator cannot decorate an abstract method, which has no code; decorate "
                     "the methods that implement it");
    return;
  }
  if (memberName.starts_with("#")) {
    diags_.error(at, "a decorator cannot decorate a '#private' member, whose name is no key of "
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
    return member && member->name == "metadata" && isGlobalName(member->object, "Reflect");
  };
  std::vector<Expr *> callees;
  for (const DecoratorUse &use : uses) {
    Expr *expr = use.decorator->expr;
    if (isMetadata(expr)) {
      callees.push_back(nullptr);
      continue;
    }
    if (isPlainReference(expr)) {
      callees.push_back(expr);
      continue;
    }
    llvm::StringRef hidden = ctx_.hiddenName();
    auto *decl = ctx_.make<VarDeclStmt>(use.decorator->loc, /*isConst=*/true);
    VarDeclarator d;
    d.name = hidden;
    d.loc = use.decorator->loc;
    d.init = expr;
    decl->decls.push_back(std::move(d));
    checkStmt(decl);
    out.push_back(decl);
    callees.push_back(ctx_.make<NameExpr>(use.decorator->loc, hidden));
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
      diags_.error(loc, label + " cannot decorate " + placeText(place) + ": it takes " +
                            std::to_string(count) + " arguments, and a decorator of " +
                            placeText(place) + " gets " + std::to_string(given) + ", " +
                            argumentsText(place));
      ok = false;
    } else if (third && place == Place::Parameter && third->isObject() && third->field("value")) {
      diags_.error(loc, label + " takes the descriptor of a method: it decorates methods, not "
                                "parameters");
      ok = false;
    } else if (third && place == Place::Method && third->isNumeric()) {
      diags_.error(loc, label + " takes the position of a parameter: it decorates parameters, not "
                                "methods");
      ok = false;
    }
    takes.push_back(std::min(count, given));
    descriptor |= place == Place::Method && count >= 3;
  }
  if (!ok)
    return;
  // A method of a generic class is a method of each of its instances, with
  // their own types: one descriptor cannot stand for them all.
  if (descriptor && cls->isGeneric()) {
    diags_.error(at, "a decorator that takes the descriptor of a method of a generic class is not "
                     "supported yet: each instance of '" +
                         cls->name + "' has a method of its own; decorate a method of a class "
                                     "that is not generic, or take only the class and the name");
    return;
  }

  // The descriptor: `value` is what the method runs, its own code until a
  // decorator replaces it (see codegen_decorate.cpp).
  VarSymbol *descVar = nullptr;
  const Type *descType = nullptr;
  if (descriptor) {
    FuncSymbol *sym = method->sym;
    resolveParams(sym);
    const Type *ret = returnType(sym);
    if (!ret || ret->isError() || llvm::is_contained(sym->params, nullptr))
      return;
    descType = methodDescriptorType(Type::tuple(sym->params), ret);
    VarSymbol *impl = ctx_.newVar();
    impl->name = ctx_.keepName("%" + cls->name.str() + "." + method->name.str() + "." +
                               std::to_string(method->variant));
    impl->loc = method->loc();
    impl->type = descType->field("value")->type;
    impl->isGlobal = true;
    impl->usedInFunction = true;
    impl->reassigned = true;
    impl->declSeq = ++seq_;
    declare(impl->name, impl->loc, {impl});
    method->impl = impl;
    auto *lit = ctx_.make<ObjectLitExpr>(at);
    auto prop = [&](llvm::StringRef name, Expr *value) {
      ObjectLitExpr::Property p;
      p.name = name;
      p.loc = at;
      p.value = value;
      lit->props.push_back(p);
    };
    prop("value", ctx_.make<NameExpr>(at, impl->name));
    prop("writable", ctx_.make<BoolLitExpr>(at, true));
    prop("enumerable", ctx_.make<BoolLitExpr>(at, false));
    prop("configurable", ctx_.make<BoolLitExpr>(at, true));
    Expr *init = coerce(checkExpr(lit, descType), descType);
    descVar = declareVar(ctx_.hiddenName(), at, descType, /*isConst=*/false);
    descVar->reassigned = true;
    auto *decl = ctx_.make<VarDeclStmt>(at, /*isConst=*/false);
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
    for (const FunctionDecl *c : cls->constructors)
      if (decoratedParams(c) || cls->constructors.size() == 1)
        ctor = ctor ? nullptr : c;
  }
  writeDesignRecords(cls, field, method, ctor, at);

  // Applied from the bottom up, the parameters' first. A class decorator
  // gets the class the one below it gave, if any.
  ClassDecl *current = cls;
  for (size_t i = uses.size(); i-- > 0;) {
    const DecoratorUse &use = uses[i];
    llvm::SMLoc loc = use.decorator->loc;
    Place place = use.param >= 0 ? Place::Parameter : own;
    if (!callees[i]) {
      // `@Reflect.metadata(key, value)`: Reflect.defineMetadata(key, value,
      // cls, name) right here.
      auto *metadata = cast<CallExpr>(use.decorator->expr);
      if (metadata->args.size() != 2 || metadata->hasSpread()) {
        diags_.error(loc, "'Reflect.metadata' takes the key and the value of the record, e.g. "
                          "'@Reflect.metadata(Role, \"admin\")'");
        continue;
      }
      auto *callee = ctx_.make<MemberExpr>(loc, ctx_.make<NameExpr>(loc, "Reflect"), "defineMetadata");
      std::vector<Expr *> args{metadata->args[0], metadata->args[1], classValueAt(cls, loc)};
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (place != Place::Class && !(use.param >= 0 && use.owner->isConstructor))
        args.push_back(ctx_.make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
      out.push_back(ctx_.make<ExprStmt>(loc, checkExpr(ctx_.make<CallExpr>(loc, callee, args))));
      continue;
    }
    std::vector<Expr *> args{classValueAt(place == Place::Class ? current : cls, loc)};
    if (place != Place::Class) {
      llvm::StringRef name = use.param >= 0 ? use.owner->name : memberName;
      if (use.param >= 0 && use.owner->isConstructor)
        args.push_back(ctx_.make<UndefinedExpr>(loc));
      else
        args.push_back(ctx_.make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
    }
    if (place == Place::Parameter)
      args.push_back(ctx_.make<IntLitExpr>(loc, uint64_t(use.param)));
    else if (place == Place::Method && descVar)
      args.push_back(ctx_.make<NameExpr>(loc, descVar->name));
    args.resize(std::min(args.size(), takes[i]));
    auto *call = ctx_.make<CallExpr>(loc, callees[i], std::move(args));
    Expr *applied = checkExpr(call);
    const Type *result = applied->type;
    if (descVar && place == Place::Method && result == descType) {
      // `return descriptor`: what it gives back is the descriptor from now on.
      auto *target = ctx_.make<NameExpr>(loc, descVar->name);
      target->var = descVar;
      target->type = descType;
      auto *assign = ctx_.make<AssignExpr>(loc, std::nullopt, target, applied);
      assign->type = descType;
      applied = assign;
    } else if (place == Place::Class && result->classRef() &&
               result->classRef()->derivesFrom(current)) {
      // A class that extends the one it got, or that one: the name means it
      // from here on (docs/decorators.md, section 5).
      if (ClassDecl *given = const_cast<ClassDecl *>(result->classRef()); given != current) {
        replaceClass(current, given, loc);
        current = given;
      }
    } else if (!result->isVoid() && !result->isUndefined() && !result->isError()) {
      std::string name = decoratorName(use.decorator->expr);
      std::string label = name.empty() ? "the decorator" : "'" + name + "'";
      if (place == Place::Class && result->isConstructor())
        diags_.error(loc, label + " returns '" + result->name().str() +
                              "', some class known only when the program runs, and the class it "
                              "gives in place of '" + current->name.str() +
                              "' must be known when compiling: make the decorator generic, "
                              "'<T extends Constructor>(cls: T)', and return 'cls' or a class "
                              "that extends it");
      else if (place == Place::Class)
        diags_.error(loc, label + " returns '" + result->name().str() +
                              "': a class decorator gives back nothing, the class it got, or a "
                              "class that extends it, which then replaces it");
      else if (place == Place::Method && descVar)
        diags_.error(loc, label + " returns '" + result->name().str() +
                              "': a decorator of a method returns nothing, or the descriptor "
                              "it got");
      else
        diags_.error(loc, label + " returns '" + result->name().str() +
                              "', which a decorator of " + placeText(place) +
                              " cannot give back; return nothing");
    }
    out.push_back(ctx_.make<ExprStmt>(loc, applied));
  }
  if (descVar) {
    // What the calls of the method run from now on.
    auto *target = ctx_.make<NameExpr>(at, method->impl->name);
    auto *value = ctx_.make<MemberExpr>(at, ctx_.make<NameExpr>(at, descVar->name), "value");
    auto *assign = ctx_.make<AssignExpr>(at, std::nullopt, target, value);
    out.push_back(ctx_.make<ExprStmt>(at, checkExpr(assign)));
  }
}

//===----------------------------------------------------------------------===//
// Metadata
//===----------------------------------------------------------------------===//

namespace {

// The type a record `design:*` holds, for a key written as that literal;
// null for any other key.
const Type *designType(const Expr *key) {
  auto *lit = dyn_cast<StringLitExpr>(key);
  if (!lit)
    return nullptr;
  // Classes, which `new` and Reflect.construct can make objects of (a DI
  // container makes those of the parameters of a constructor).
  const Type *any = Type::constructorOf(Type::anyObject());
  if (lit->value == u"design:type" || lit->value == u"design:returntype")
    return Type::optional(any);
  if (lit->value == u"design:paramtypes")
    return Type::optional(Type::array(any));
  return nullptr;
}

bool isDesignKey(const Expr *key) {
  auto *lit = dyn_cast<StringLitExpr>(key);
  return lit && lit->value.rfind(u"design:", 0) == 0;
}

} // namespace

ClassDecl *Sema::builtinConstructor(llvm::StringRef name) {
  static const llvm::StringLiteral names[] = {
      "Number",      "String",       "Boolean",     "Object",       "Array",
      "Function",    "Date",         "RegExp",      "Map",          "Set",
      "Promise",     "Uint8Array",   "Int8Array",   "Uint8ClampedArray", "Int16Array",
      "Uint16Array", "Int32Array",   "Uint32Array", "Float16Array", "Float32Array",
      "Float64Array", "Buffer",      "ArrayBuffer", "DataView",     "TextEncoder",
      "TextDecoder"};
  auto found = llvm::find(names, name);
  if (found == std::end(names))
    return nullptr;
  ClassDecl *&cls = builtinConstructors_[name];
  if (!cls) {
    cls = ctx_.make<ClassDecl>(llvm::SMLoc(), *found);
    cls->builtin = true;
    cls->resolved = cls->declared = true;
  }
  return cls;
}

const void *Sema::metadataKey(const Expr *key, std::string &label) {
  auto *name = dyn_cast<NameExpr>(key);
  if (!name)
    return nullptr;
  ScopeEntry entry = lookup(name->name);
  label = name->name.str();
  if (entry.var)
    return entry.var->isConst && entry.var->type && !entry.var->type->isString() ? entry.var
                                                                                    : nullptr;
  if (entry.func)
    return entry.func;
  return entry.cls;
}

void Sema::findMetadataWriters(SourceFile &program) {
  for (Module *module : program.modules)
    for (Stmt *item : module->file.items) {
      std::vector<FunctionDecl *> fns;
      if (auto *fn = dyn_cast<FunctionDecl>(item))
        fns.push_back(fn);
      else if (auto *cls = dyn_cast<ClassDecl>(item))
        fns = cls->methods;
      for (FunctionDecl *fn : fns)
        visitExprs(fn, [&](const Expr *e) {
          auto *call = dyn_cast<CallExpr>(e);
          auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
          auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
          if (!object || object->name != "Reflect" || member->name != "defineMetadata" ||
              call->args.empty())
            return;
          if (auto *key = dyn_cast<NameExpr>(call->args[0])) {
            std::vector<FunctionDecl *> &writers = metaWriters_[key->name];
            if (!llvm::is_contained(writers, fn))
              writers.push_back(fn);
          }
        });
    }
}

void Sema::checkBodyEarly(FunctionDecl *fn) {
  if (!fn->sym || fn->checked || fn->sym->isGeneric() || llvm::is_contained(functions_, fn))
    return;
  if (!fn->sym->ret) { // checking it finds what it returns
    inferReturnType(fn->sym);
    return;
  }
  if (fn->cls) // checked with its class
    return;
  BodyState saved = enterBody(fn);
  if (!topLevelDone_)
    early_ = fn->sym; // top-level code reads it: globals declared later are not there yet
  flow_ = Flow();
  checkFunctionBody(fn);
  leaveBody(saved);
}

const Type *Sema::metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                               const Type *expected) {
  if (auto found = metaTypes_.find(key); found != metaTypes_.end())
    return found->second.type;
  if (auto writers = metaWriters_.find(label); writers != metaWriters_.end())
    for (FunctionDecl *fn : std::vector<FunctionDecl *>(writers->second)) {
      checkBodyEarly(fn);
      if (auto found = metaTypes_.find(key); found != metaTypes_.end())
        return found->second.type;
    }
  // `const names: string[] = Reflect.getMetadata(Column, cls) ?? []` in the
  // function that writes them next.
  if (expected && !expected->nonOptional()->isUnknown() && !mentionsParam(expected)) {
    metaTypes_[key] = {expected->nonOptional(), loc};
    return expected->nonOptional();
  }
  diags_.error(loc, "the records of '" + label + "' have the type of the first one written, "
                    "'Reflect.defineMetadata(" + label + ", value, target)', and none is written "
                    "before this; write one first, give the type where this one goes (e.g. "
                    "'const x: string[] = ...'), or use a string key, whose records are 'unknown'");
  return Type::error();
}

Expr *Sema::checkReflectCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  struct Function {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned min, max;
  };
  static const Function functions[] = {
      {"defineMetadata", Builtin::MetaDefine, 3, 4}, {"getMetadata", Builtin::MetaGet, 2, 3},
      {"getOwnMetadata", Builtin::MetaGetOwn, 2, 3}, {"hasMetadata", Builtin::MetaHas, 2, 3},
      {"hasOwnMetadata", Builtin::MetaHasOwn, 2, 3}, {"deleteMetadata", Builtin::MetaDelete, 2, 3}};
  llvm::StringRef fn = member->name;
  // Reflection (sema_reflect.cpp).
  if (fn == "get" || fn == "apply" || fn == "construct")
    return checkReflection(call, member, expected);
  const Function *function =
      llvm::find_if(functions, [&](const Function &f) { return f.name == fn; });
  if (function == std::end(functions)) {
    if (fn == "metadata")
      diags_.error(member->loc(), "'Reflect.metadata(key, value)' makes a decorator; write it as "
                                  "one, before a class or a member: '@Reflect.metadata(key, value)'");
    else if (fn == "getMetadataKeys" || fn == "getOwnMetadataKeys")
      diags_.error(member->loc(), "'Reflect." + fn + "' is not supported: the keys of records are "
                                  "decorators, classes and strings, which no one type holds; ask "
                                  "for each key with 'Reflect.hasMetadata(key, target)'");
    else
      diags_.error(member->loc(), "'Reflect." + fn + "' is not supported; Reflect has get, apply, "
                                  "construct, defineMetadata, getMetadata, getOwnMetadata, "
                                  "hasMetadata, hasOwnMetadata and deleteMetadata");
    checkArgsLoosely();
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, member->loc(), function->min, function->max))
    return call;
  bool define = function->builtin == Builtin::MetaDefine;

  // The key: an object the compiler knows, or a string.
  Expr *&keyArg = call->args[0];
  llvm::SMLoc keyLoc = keyArg->loc();
  std::string label;
  const void *key = metadataKey(keyArg, label);
  const Type *design = designType(keyArg);
  if (key) {
    call->metaKey = key;
    keyArg = ctx_.make<StringLitExpr>(keyLoc, u"");
    keyArg->type = Type::string();
  } else {
    keyArg = checkExpr(keyArg);
    if (!keyArg->type->isString() && !keyArg->type->isError()) {
      diags_.error(keyLoc, "a key of metadata is a decorator, a class or a constant, whose records "
                           "have the type of what is written, or a string, whose records are "
                           "'unknown'; found '" +
                               keyArg->type->name() + "'");
      checkArgsLoosely();
      return call;
    }
    if (define && isDesignKey(keyArg))
      diags_.error(keyLoc, "the records 'design:*' are what the compiler writes for decorated "
                           "members; write a key of your own");
  }

  // The value.
  if (define) {
    Expr *&value = call->args[1];
    const MetaKeyType *known = nullptr;
    if (key)
      if (auto found = metaTypes_.find(key); found != metaTypes_.end())
        known = &found->second;
    value = checkExpr(value, known ? known->type : nullptr);
    const Type *type = value->type;
    if (type->isVoid()) {
      diags_.error(value->loc(), "an expression of type 'void' has no value to record");
    } else if (known && !type->isError()) {
      unsigned before = diags_.errorCount();
      value = coerce(value, known->type, "the records of '" + label + "'");
      if (diags_.errorCount() > before)
        diags_.note(known->loc, "the records of '" + label + "' are '" + known->type->name() +
                                    "' since the first one, here");
    } else if (key && !type->isError() && !mentionsParam(type)) {
      if (type->isUndefined())
        diags_.error(value->loc(), "cannot tell the type of the records of '" + label +
                                       "' from 'undefined'; write a value of the type first");
      else
        metaTypes_[key] = {type, value->loc()};
    }
    if (!value->type->isVoid())
      value = convert(value, Type::unknown());
  }

  // The target, an object, and the property, a string or nothing.
  size_t at = define ? 2 : 1;
  Expr *&target = call->args[at];
  target = checkExpr(target);
  const Type *targetType = target->type;
  if (!targetType->isError() && !isObjectReference(targetType) && !targetType->isAnyObject())
    diags_.error(target->loc(), "the target of metadata is an object, such as a class, found '" +
                                    targetType->name() + "'");
  else
    target = convert(target, Type::anyObject());
  if (call->args.size() == at + 1)
    call->args.push_back(ctx_.make<UndefinedExpr>(call->loc()));
  call->args[at + 1] = checkArg(call->args[at + 1], Type::optional(Type::string()));

  call->builtin = function->builtin;
  switch (function->builtin) {
  case Builtin::MetaDefine:
    call->type = Type::voidTy();
    break;
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn:
    call->type = key      ? Type::optional(metadataType(key, label, keyLoc, expected))
                 : design ? design
                          : Type::unknown();
    break;
  default:
    call->type = Type::boolTy();
    break;
  }
  return call;
}

Expr *Sema::designClass(const Type *type, llvm::SMLoc loc) {
  return classValueAt(designConstructor(type), loc);
}

ClassDecl *Sema::designConstructor(const Type *type) {
  type = type->nonOptional();
  // A literal type is a value of its base; a union is of the constructor all
  // its members have, or else Object, as TypeScript writes them.
  if (type->isLiteral())
    type = type->literalBase();
  if (type->isUnion()) {
    ClassDecl *common = designConstructor(type->members().front());
    for (const Type *m : type->members())
      if (designConstructor(m) != common)
        return builtinConstructor("Object");
    return common;
  }
  if (type->isBytes())
    return builtinConstructor(type->name());
  llvm::StringRef name = "Object";
  if (type->isPromise())
    name = "Promise";
  else if (type->isNumeric())
    name = "Number";
  else if (type->isString())
    name = "String";
  else if (type->isBool())
    name = "Boolean";
  else if (type->isArray() || type->isTuple() || type->isMatch())
    name = "Array";
  else if (type->isFunction() || type->isClassRef())
    name = "Function";
  else if (type->isDate())
    name = "Date";
  else if (type->isRegExp())
    name = "RegExp";
  else if (type->isMap())
    name = "Map";
  else if (type->isSet())
    name = "Set";
  else if (ClassDecl *cls = classOf(type); cls && !cls->literal)
    return cls;
  return builtinConstructor(name);
}

void Sema::writeDesignRecord(ClassDecl *cls, const char16_t *key, Expr *value,
                             llvm::StringRef member, llvm::SMLoc loc) {
  auto *callee = ctx_.make<NameExpr>(loc, "Reflect");
  callee->type = Type::error(); // the call is a built-in, which needs none
  auto *keyLit = ctx_.make<StringLitExpr>(loc, key);
  keyLit->type = Type::string();
  Expr *property;
  if (member.empty()) {
    property = ctx_.make<UndefinedExpr>(loc);
    property->type = Type::undefinedTy();
  } else {
    property = ctx_.make<StringLitExpr>(loc, std::u16string(member.begin(), member.end()));
    property->type = Type::string();
  }
  if (value->type->isClassRef())
    value = convert(value, Type::constructorOf(Type::anyObject()));
  std::vector<Expr *> args{keyLit, convert(value, Type::unknown()),
                           convert(classValueAt(cls, loc), Type::anyObject()),
                           convert(property, Type::optional(Type::string()))};
  auto *call = ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->builtin = Builtin::MetaDefine;
  call->designRecord = true;
  call->type = Type::voidTy();
  cls->decorations.push_back(ctx_.make<ExprStmt>(loc, call));
}

// As TypeScript's emitDecoratorMetadata writes them, here always: in the
// order TypeScript applies them, design:returntype first.
void Sema::writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                              const FunctionDecl *ctor, llvm::SMLoc loc) {
  auto paramTypes = [&](const FunctionDecl *fn) {
    auto *list = ctx_.make<ArrayLitExpr>(loc);
    resolveParams(fn->sym);
    const Type *any = Type::constructorOf(Type::anyObject());
    for (const Type *param : fn->sym->params)
      if (param)
        list->elements.push_back({convert(designClass(param, loc), any), false});
    list->type = Type::array(any);
    return list;
  };
  if (field) {
    completeClass(cls); // the types that come from the initializers
    if (field->resolved && !field->resolved->isError())
      writeDesignRecord(cls, u"design:type", designClass(field->resolved, loc), field->name, loc);
    return;
  }
  if (method) {
    const Type *ret = returnType(method->sym);
    if (ret && !ret->isVoid() && !ret->isError())
      writeDesignRecord(cls, u"design:returntype", designClass(ret, loc), method->name, loc);
    writeDesignRecord(cls, u"design:paramtypes", paramTypes(method), method->name, loc);
    writeDesignRecord(cls, u"design:type", classValueAt(builtinConstructor("Function"), loc),
                      method->name, loc);
    return;
  }
  if (ctor)
    writeDesignRecord(cls, u"design:paramtypes", paramTypes(ctor), {}, loc);
}

} // namespace tz
