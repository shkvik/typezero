// Writes through a wider type (docs/objects.md, section 2, step 5 of its
// plan). A value may go where its type is seen as one that allows more
// writes than its own: a `Dog[]` where an `Animal[]` is expected, an object
// whose class or interface narrows a field (`pet: Dog` for `pet: Animal`)
// where the wider one is expected, an object with a read-only field where
// the field may change. Nothing changes in memory, so such a write would
// break the value: a Cat in an array of Dog, read later as a Dog. The
// conversion is allowed if the program never makes such a write, which this
// file checks once the whole program is checked.
//
// The check follows where values go, flow-insensitively: into variables and
// parameters (calls of known functions and methods, `this` of methods, the
// parameters of callbacks), into fields by name, into the elements of arrays
// by their type, out of functions by what they return. A value is tracked
// with how deep inside the converted value it is: a field of it is one level
// down, and a new array of its elements (`[...dogs]`, `slice`) holds them a
// level down but is not the array itself, so writing to the copy is fine.
// A write is checked if the converted value reaches what it writes to at the
// level of some narrowed part: an element put into an array that is a Dog[]
// at heart, of a type other than Dog. A call of a function value may call any
// function the program uses as a value. What is thrown or rejected goes to
// where errors come back: the variables of `catch`, the callbacks of
// `catch` and of `then` for a rejection, `reason` of what `allSettled`
// gives, `errors` of an AggregateError. A value that reaches a place the
// check does not follow (`yield`, the values given to the resolvers of a
// Promise, a built-in that keeps it) is checked against every write of the
// program but those of the built-in modules written in tz, which write only
// to what they make. Types tell where a value cannot be: in a string or a
// number, in a variable of a type it cannot be seen as (nor hold it, nor be
// a part of it), behind a write through a type its part cannot be seen as.
// The error shows at the conversion and names the write.

#include "compiler/check/sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"

#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// How deep inside a converted value a narrowed part may be.
constexpr int kMaxDepth = 6;

// A write that the wider type allows and the value's own type does not:
// `depth` levels inside the value (0: the value itself), to the field `name`
// (or to an element of an array), of anything but a `narrow` (none: of
// anything, the field is read-only in the value's type). `owner` is the type
// of the value's part there.
struct Narrowing {
  int depth;
  bool element;
  std::string name;
  const Type *narrow;
  const Type *owner;
};

void collectNarrowings(const Type *from, const Type *to, int depth, std::vector<Narrowing> &out,
                       std::set<std::pair<const Type *, const Type *>> &seen) {
  from = from->nonOptional();
  to = to->nonOptional();
  if (from == to || depth > kMaxDepth || !seen.insert({from, to}).second)
    return;
  if (from->isArray() && to->isArray()) {
    if (from->inner() != to->inner())
      out.push_back({depth, true, "", from->inner(), from});
    collectNarrowings(from->inner(), to->inner(), depth + 1, out, seen);
    return;
  }
  // The objects of a union seen as a type they all are. Seen as one of its
  // members, the value is that member: what narrowed it found it is.
  if (from->isUnion()) {
    if (llvm::is_contained(from->members(), to))
      return;
    for (const Type *m : from->members())
      collectNarrowings(m, to, depth, out, seen);
    return;
  }
  if (!from->isObject() || !to->isObject() || from->isAnyObject() || to->isAnyObject())
    return;
  for (const Field &wide : to->fields()) {
    const Field *own = from->field(wide.name);
    if (!own)
      continue;
    if (own->readonly && !wide.readonly)
      out.push_back({depth, false, wide.name, nullptr, from});
    else if (own->type != wide.type)
      out.push_back({depth, false, wide.name, own->type, from});
    if (own->type != wide.type)
      collectNarrowings(own->type, wide.type, depth + 1, out, seen);
  }
}

// Where values flow: a variable, the fields of a name, the elements of
// arrays of a type, the keys and values of a kind of dictionary, what a
// function returns, what a call of a function value gives, a conversion, or
// a place the check does not follow.
enum class NodeKind { Var, Field, Elements, Values, Return, CallResult, Conversion, Unknown };

// A value that holds what `node` holds `off` levels deeper, from its own
// level `md` on: a variable is (var, 0, 0); a field of it (var, 1, 0); a new
// array of its elements (var, 0, 1), whose elements are its elements but
// which is not the array itself.
struct Ref {
  int node;
  int off;
  int md;
};
using Refs = std::vector<Ref>;

// What flows from a node into another: `to` at level j holds what `from`
// holds at level j + off, from j = md on.
struct Edge {
  int to;
  int off;
  int md;
};

// A write to a field or an element of what `target` holds.
struct Write {
  Refs target;
  bool element;
  std::string name;    // empty for an element; "*" for a field not known
  const Type *value;   // what is written, before it is converted; null for any
  const Type *through; // the type of the object written to
  llvm::SMLoc loc;
};

// A conversion that widens what code may write: from where it is, the
// writes the value's type does not allow.
struct Conversion {
  int node;
  const Type *from;
  const Type *to;
  llvm::SMLoc loc;
  std::vector<Narrowing> narrowings;
  // `super(...)`: the node of the `this` of the constructor of the class up
  // the chain, which sets the fields of `this` as the class's own
  // initializers do after it; -1 for another conversion.
  int initializes = -1;
  std::string role;     // how the value goes there, for the message
  llvm::StringRef name; // of the variable converted, if it is one
};

Refs shifted(const Refs &refs, int levels) {
  Refs out;
  for (const Ref &r : refs)
    out.push_back({r.node, r.off + levels, std::max(r.md - levels, 0)});
  return out;
}

// A new container of what `refs` hold: a copy of an array, `[...xs]`.
Refs copied(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs)
    out.push_back({r.node, r.off, std::max(r.md, 1)});
  return out;
}

// A new container that holds the values `refs` hold: `[x]`, `{ f: x }`.
Refs holding(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs)
    out.push_back({r.node, r.off - 1, r.md + 1});
  return out;
}

void append(Refs &to, const Refs &from) { to.insert(to.end(), from.begin(), from.end()); }

// Whether values of `type` may hold references to what a conversion widened,
// or be one: not numbers, strings or booleans, nor the objects of the runtime
// that hold only those (bytes, dates, regular expressions, Intl and crypto
// objects), which no conversion widens.
bool holdsReferences(const Type *type) {
  if (!type)
    return true;
  type = type->nonOptional();
  return !(type->isNumeric() || type->isBool() || type->isString() || type->isLiteral() ||
           type->isDecimal() || type->isVoid() || type->isUndefined() || type->isNever() ||
           type->isBytes() || type->isDate() || type->isRegExp() || type->isIntl() ||
           type->isCrypto());
}

// The type of a value before the conversions Sema added to it.
const Type *ownType(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  return expr->type;
}

// The program as a graph of flows (see the top of the file).
class Flows {
public:
  Flows(const llvm::DenseSet<const CastExpr *> &widenings,
        llvm::function_ref<std::vector<Narrowing>(const Type *, const Type *)> narrowings,
        llvm::function_ref<bool(const Type *, const Type *)> convertible)
      : widenings_(widenings), narrowings_(narrowings), convertible_(convertible) {
    unknown_ = node(NodeKind::Unknown, nullptr, "");
    thrown_ = node(NodeKind::Unknown, nullptr, "thrown");
  }

  void walkProgram(SourceFile &file) {
    for (Stmt *item : file.items)
      stmt(item);
    for (FunctionDecl *fn : file.instances)
      function(fn);
    // A call of a function value may call any function the program uses as
    // a value that takes as many arguments, of types that what the value's
    // type passes may go to (a listener of Animal[] is no callback of a
    // stream's Uint8Array chunks).
    for (const ValueCall &call : valueCalls_)
      for (const FunctionDecl *fn : functionValues_) {
        size_t count = fn->params.size();
        bool rest = count > 0 && fn->params.back().rest;
        if (call.args.size() > count && !rest)
          continue;
        if (!mayCall(call.call->callee ? call.call->callee->type : nullptr, fn))
          continue;
        passArguments(call.call, fn, call.args);
        edges[size_t(returned(fn))].push_back({call.result, 0, 0});
      }
    // A Result made from what a function throws or a promise rejects holds
    // the error, which this check does not follow there.
    if (resultOfErrors_)
      escape({{thrown_, 0, 0}});
  }

  std::vector<std::vector<Edge>> edges;
  std::vector<Write> writes;
  std::vector<Conversion> conversions;
  int unknown_ = 0;
  // By node: the type of the variable or of what the function returns, which
  // the value a node holds is one of; null for other nodes.
  std::vector<const Type *> types;
  // What `throw` throws and what a promise is rejected with.
  int thrown_ = 0;

private:
  int node(NodeKind kind, const void *key, llvm::StringRef name) {
    auto [it, added] =
        ids_.try_emplace(std::make_tuple(int(kind), key, name.str()), int(edges.size()));
    if (added)
      edges.emplace_back();
    return it->second;
  }
  int var(const VarSymbol *v) { return holder(node(NodeKind::Var, v, ""), v->type); }
  int fieldNode(llvm::StringRef name) { return node(NodeKind::Field, nullptr, name); }
  // What containers of `type` hold: the elements of arrays of a type, the
  // keys and values of a kind of dictionary, what promises of a type give;
  // -1 for other types.
  int contents(const Type *type) {
    if (!type)
      return -1;
    type = type->nonOptional();
    if (type->isArray())
      return holder(node(NodeKind::Elements, type->inner(), ""), type->inner());
    if (type->isDict() || type->isPromise())
      return node(NodeKind::Values, type, "");
    return -1;
  }
  // Whether a value of the function type `type` may be `fn`: the types of
  // what it passes go to its parameters. Types with type parameters, and
  // unknown, may be anything.
  bool mayCall(const Type *type, const FunctionDecl *fn) const {
    type = type ? type->nonOptional() : nullptr;
    if (!type || !type->isFunction() || !fn->sym)
      return true;
    llvm::ArrayRef<const Type *> given = type->params();
    const std::vector<const Type *> &takes = fn->sym->params;
    for (size_t i = 0; i < given.size() && i < takes.size(); ++i) {
      const Type *q = given[i], *p = takes[i];
      if (!q || !p || mentionsParam(q) || mentionsParam(p) || q->isUnknown() || p->isUnknown() ||
          q->isError() || p->isError())
        continue;
      if ((type->hasRest() && i + 1 == given.size()) || (fn->sym->hasRest && i + 1 == takes.size()))
        continue;
      if (!convertible_(q, p))
        return false;
    }
    return true;
  }

  void functionValue(const FunctionDecl *fn) {
    if (fn && functionSet_.insert(fn).second)
      functionValues_.push_back(fn);
  }
  int returned(const FunctionDecl *fn) {
    return holder(node(NodeKind::Return, fn, ""), fn->sym ? fn->sym->ret : nullptr);
  }

  // A node of a type that holds no references (a string, a number) holds
  // nothing a conversion widened, whatever flows to it.
  int holder(int id, const Type *type) {
    if (size_t(id) >= plain_.size()) {
      plain_.resize(edges.size());
      types.resize(edges.size());
    }
    if (!holdsReferences(type))
      plain_[size_t(id)] = true;
    types[size_t(id)] = type;
    return id;
  }
  void flow(const Refs &refs, int to) {
    if (size_t(to) < plain_.size() && plain_[size_t(to)])
      return;
    for (const Ref &r : refs)
      edges[size_t(r.node)].push_back({to, r.off, r.md});
  }
  void escape(const Refs &refs) { flow(refs, unknown_); }

  // A conversion a call makes of `this`: an object of a class that narrows
  // fields, given to a method of a class up its chain.
  Refs receiverConversion(const Expr *at, const Type *from, const FunctionDecl *method,
                          const Refs &refs, const VarSymbol *initializes, std::string role) {
    if (!method || !method->cls || !method->cls->type || !from)
      return refs;
    const Type *to = method->cls->type;
    from = from->nonOptional();
    if (from == to || !from->isObject())
      return refs;
    std::vector<Narrowing> found = narrowings_(from, to);
    if (found.empty())
      return refs;
    int conv = node(NodeKind::Conversion, at, method->name);
    conversions.push_back({conv, from, to, at->loc(), std::move(found),
                           initializes ? var(initializes) : -1, std::move(role),
                           llvm::StringRef()});
    Refs out = refs;
    out.push_back({conv, 0, 0});
    return out;
  }

  void function(FunctionDecl *fn) {
    if (!fn || !walked_.insert(fn).second)
      return;
    functions_.push_back(fn);
    for (Param &p : fn->params)
      if (p.defaultValue) {
        Refs value = expr(p.defaultValue);
        if (p.sym)
          flow(value, var(p.sym));
      }
    if (fn->body)
      stmt(fn->body);
    functions_.pop_back();
  }

  void classDecl(ClassDecl *cls) {
    if (!cls->isEmitted())
      return;
    for (FunctionDecl *fn : cls->constructors)
      function(fn);
    for (FunctionDecl *fn : cls->methods)
      if (fn->typeParams.empty())
        function(fn);
    for (FunctionDecl *fn : cls->methodInstances)
      function(fn);
    if (cls->init)
      function(cls->init);
    for (ClassField &f : cls->fields)
      if (f.init)
        flow(expr(f.init), f.var ? var(f.var) : fieldNode(f.name));
    for (Stmt *s : cls->decorations)
      stmt(s);
  }

  void stmt(Stmt *s) {
    if (!s)
      return;
    switch (s->kind()) {
    case Stmt::Kind::Block:
      for (Stmt *child : cast<BlockStmt>(s)->body)
        stmt(child);
      break;
    case Stmt::Kind::VarDecl:
      for (VarDeclarator &d : cast<VarDeclStmt>(s)->decls)
        if (d.init) {
          Refs value = expr(d.init);
          if (d.sym)
            flow(value, var(d.sym));
        }
      break;
    case Stmt::Kind::Function: {
      auto *fn = cast<FunctionDecl>(s);
      if (fn->typeParams.empty())
        function(fn);
      break;
    }
    case Stmt::Kind::Expr: expr(cast<ExprStmt>(s)->expr); break;
    case Stmt::Kind::If: {
      auto *i = cast<IfStmt>(s);
      expr(i->cond);
      stmt(i->thenStmt);
      stmt(i->elseStmt);
      break;
    }
    case Stmt::Kind::While:
      expr(cast<WhileStmt>(s)->cond);
      stmt(cast<WhileStmt>(s)->body);
      break;
    case Stmt::Kind::DoWhile:
      expr(cast<DoWhileStmt>(s)->cond);
      stmt(cast<DoWhileStmt>(s)->body);
      break;
    case Stmt::Kind::For: {
      auto *f = cast<ForStmt>(s);
      stmt(f->init);
      if (f->cond)
        expr(f->cond);
      if (f->step)
        expr(f->step);
      stmt(f->body);
      break;
    }
    case Stmt::Kind::ForOf: {
      auto *f = cast<ForOfStmt>(s);
      Refs iterable = expr(f->iterable);
      if (!f->keys && f->binding.sym) {
        Refs element = shifted(iterable, 1);
        if (int held = contents(f->iterable->type); held >= 0)
          element.push_back({held, 0, 0});
        flow(element, var(f->binding.sym));
      }
      stmt(f->body);
      break;
    }
    case Stmt::Kind::Switch: {
      auto *sw = cast<SwitchStmt>(s);
      expr(sw->discriminant);
      for (SwitchCase &c : sw->cases) {
        if (c.test)
          expr(c.test);
        for (Stmt *body : c.body)
          stmt(body);
      }
      break;
    }
    case Stmt::Kind::Labeled: stmt(cast<LabeledStmt>(s)->body); break;
    case Stmt::Kind::Return:
      if (Expr *value = cast<ReturnStmt>(s)->value) {
        Refs refs = expr(value);
        if (!functions_.empty())
          flow(refs, returned(functions_.back()));
        else
          escape(refs);
      }
      break;
    case Stmt::Kind::Throw: flow(expr(cast<ThrowStmt>(s)->value), thrown_); break;
    case Stmt::Kind::Try: {
      auto *t = cast<TryStmt>(s);
      stmt(t->block);
      if (t->handler && t->param.sym)
        flow({{thrown_, 0, 0}}, var(t->param.sym));
      stmt(t->handler);
      stmt(t->finalizer);
      break;
    }
    case Stmt::Kind::Class: classDecl(cast<ClassDecl>(s)); break;
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
    case Stmt::Kind::Empty:
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
    case Stmt::Kind::Debugger: break;
    }
  }

  // The `this` of the function being walked, for `super`.
  const VarSymbol *currentThis() const {
    for (auto it = functions_.rbegin(); it != functions_.rend(); ++it)
      if ((*it)->thisVar)
        return (*it)->thisVar;
    return nullptr;
  }

  Refs name(const NameExpr *n) {
    if (n->var)
      return {{var(n->var), 0, 0}};
    if ((n->name == "this" || n->name == "super") && currentThis())
      return {{var(currentThis()), 0, 0}};
    return {};
  }

  void recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                   const Type *through, llvm::SMLoc loc) {
    writes.push_back(
        {target, element, field.str(), value ? ownType(value) : nullptr, through, loc});
  }

  // The target of an assignment or `++`: the write, and what flows there.
  void assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound) {
    if (auto *n = dyn_cast<NameExpr>(target)) {
      if (n->var)
        flow(value, var(n->var));
      return;
    }
    if (auto *member = dyn_cast<MemberExpr>(target)) {
      Refs object = expr(member->object);
      recordWrite(object, false, member->name, compound ? nullptr : valueExpr, member->object->type,
                  target->loc());
      flow(value, fieldNode(member->name));
      return;
    }
    if (auto *index = dyn_cast<IndexExpr>(target)) {
      Refs object = expr(index->object);
      expr(index->index);
      const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
      if (objectType && objectType->isArray()) {
        recordWrite(object, true, "", compound ? nullptr : valueExpr, objectType, target->loc());
        flow(value, contents(objectType));
      } else if (objectType && objectType->isDict()) {
        flow(value, contents(objectType));
      } else {
        // A field by its key: known in a copy, or any.
        std::string field = !index->field.empty() ? index->field.str() : "*";
        if (auto *key = dyn_cast<IntLitExpr>(index->index);
            key && objectType && objectType->isTuple())
          field = std::to_string(key->value);
        recordWrite(object, false, field, compound ? nullptr : valueExpr, objectType,
                    target->loc());
        flow(value, field == "*" ? unknown_ : fieldNode(field));
      }
      return;
    }
    expr(target);
    escape(value);
  }

  // The parameters a call gives `args` to.
  void passArguments(const CallExpr *call, const FunctionDecl *target,
                     const std::vector<Refs> &args) {
    if (!target) {
      for (const Refs &a : args)
        escape(a);
      return;
    }
    size_t count = target->params.size();
    bool rest = count > 0 && target->params.back().rest;
    for (size_t i = 0; i < args.size(); ++i) {
      bool spread = call->spreads(i);
      if (count == 0) {
        escape(args[i]);
        continue;
      }
      size_t k = std::min(i, count - 1);
      const VarSymbol *param = target->params[k].sym;
      if (!param)
        continue;
      // Into the array of a rest parameter, an element; spread there, the
      // elements of the array spread.
      if (rest && k == count - 1)
        flow(spread ? copied(args[i]) : holding(args[i]), var(param));
      else if (spread)
        for (size_t j = k; j < count; ++j) {
          if (const VarSymbol *p = target->params[j].sym)
            flow(shifted(args[i], 1), var(p));
        }
      else if (i < count)
        flow(args[i], var(param));
    }
  }

  // What a call of the method gives `this`: the method, and the ones that
  // override it, which a call through the method table may run.
  void passThis(const Refs &receiver, const FunctionDecl *method) {
    if (!method)
      return;
    std::vector<const FunctionDecl *> todo{method};
    std::set<const FunctionDecl *> done;
    while (!todo.empty()) {
      const FunctionDecl *fn = todo.back();
      todo.pop_back();
      if (!done.insert(fn).second)
        continue;
      if (fn->thisVar)
        flow(receiver, var(fn->thisVar));
      for (const FunctionDecl *o : overridersOf(fn))
        todo.push_back(o);
    }
  }

  std::vector<const FunctionDecl *> overridersOf(const FunctionDecl *fn) {
    std::vector<const FunctionDecl *> out;
    if (fn->cls)
      for (const ClassDecl *child : fn->cls->children)
        for (std::vector<const ClassDecl *> todo{child}; !todo.empty();) {
          const ClassDecl *c = todo.back();
          todo.pop_back();
          for (const FunctionDecl *m : c->methods)
            if (m->overrides == fn)
              out.push_back(m);
          todo.insert(todo.end(), c->children.begin(), c->children.end());
        }
    return out;
  }

  Refs call(CallExpr *c) {
    // The functions the executor of a Promise gets are the runtime's: what
    // the first gets is not followed, what the second gets is a rejection.
    if (c->builtin == Builtin::PromiseNew)
      for (const Expr *arg : c->args)
        if (auto *f = dyn_cast<FunctionExpr>(ownExpr(arg)))
          for (size_t i = 0; i < f->fn->params.size(); ++i)
            if (const VarSymbol *p = f->fn->params[i].sym)
              (i == 1 ? runtimeRejects_ : runtimeFunctions_).insert(p);
    auto *member = llvm::dyn_cast_or_null<MemberExpr>(c->callee);
    Refs receiver;
    const Type *receiverType = nullptr;
    bool superCtor = false;
    auto *callee = llvm::dyn_cast_or_null<NameExpr>(c->callee);
    if (member) {
      receiver = expr(member->object);
      receiverType = member->object->type;
    } else if (callee && callee->name == "super") {
      receiver = name(callee);
      receiverType = currentThis() ? currentThis()->type : nullptr;
      superCtor = true;
    } else if (!callee || !(callee->func || (callee->var && callee->var->lambda))) {
      expr(c->callee); // a function value
    }
    std::vector<Refs> args;
    for (Expr *arg : c->args)
      args.push_back(expr(arg));

    if (c->builtin != Builtin::None)
      return builtin(c, member, receiver, args);

    const FunctionDecl *target = c->func && c->func->decl       ? c->func->decl
                                 : c->method && c->method->decl ? c->method->decl
                                                                : nullptr;
    if (!target)
      if (callee && callee->var && callee->var->lambda)
        target = callee->var->lambda;
    if (!target) {
      // A function value: see walkProgram. The resolvers of a Promise are
      // the runtime's.
      int result = node(NodeKind::CallResult, c, "");
      valueCalls_.push_back({c, args, result});
      if ((callee && callee->var && runtimeRejects_.count(callee->var)) ||
          (member && member->name == "reject")) {
        for (const Refs &a : args)
          flow(a, thrown_);
      } else if ((callee && callee->var && runtimeFunctions_.count(callee->var)) ||
                 (member && member->name == "resolve"))
        for (size_t i = 0; i < args.size(); ++i) {
          escape(args[i]);
          if (const Type *value = c->args[i]->type; value && !value->isVoid())
            flow(args[i], contents(Type::promise(value)));
        }
      return {{result, 0, 0}};
    }
    passArguments(c, target, args);
    if (target->takesThis() && (member || superCtor)) {
      const VarSymbol *initializes = nullptr;
      std::string role =
          "as 'this' of '" + (target->cls ? target->cls->name.str() + "." : std::string()) +
          (target->isConstructor ? std::string("constructor") : target->name.str()) + "'";
      if (superCtor && target->isConstructor)
        initializes = target->thisVar;
      Refs self = receiverConversion(c, receiverType, target, receiver, initializes, role);
      passThis(self, target);
    }
    return {{returned(target), 0, 0}};
  }

  // A function given as an argument: its parameters get `values`, as the
  // callee may call it with them; what it returns flows back.
  void callback(const Expr *arg, const Refs &values) {
    const FunctionDecl *fn = nullptr;
    if (auto *f = dyn_cast<FunctionExpr>(ownExpr(arg)))
      fn = f->fn;
    else if (auto *n = dyn_cast<NameExpr>(ownExpr(arg)))
      fn = n->func && n->func->decl ? n->func->decl : n->var ? n->var->lambda : nullptr;
    if (!fn)
      return;
    for (const Param &p : fn->params)
      if (p.sym)
        flow(values, var(p.sym));
  }

  static const Expr *ownExpr(const Expr *e) {
    while (auto *conv = dyn_cast<CastExpr>(e))
      e = conv->operand;
    return e;
  }

  Refs builtin(CallExpr *c, MemberExpr *member, const Refs &receiver,
               const std::vector<Refs> &args) {
    const Type *receiverType = member ? member->object->type : nullptr;
    int held = contents(receiverType);
    Refs parts = shifted(receiver, 1);
    if (held >= 0)
      parts.push_back({held, 0, 0});
    auto insert = [&](size_t from) {
      for (size_t i = from; i < args.size(); ++i) {
        // `a.push(...b)`: the elements of b.
        Refs value = c->spreads(i) ? shifted(args[i], 1) : args[i];
        recordWrite(receiver, true, "", c->spreads(i) ? nullptr : c->args[i],
                    receiverType ? receiverType->nonOptional() : nullptr, c->loc());
        if (held >= 0)
          flow(value, held);
      }
    };
    // The callbacks get the elements, the container, and what else the call
    // passes; what they return goes to the result of map and reduce.
    Refs given = parts;
    append(given, receiver);
    for (const Refs &a : args)
      append(given, a);
    Refs results;
    for (const Expr *arg : c->args) {
      callback(arg, given);
      if (auto *f = dyn_cast<FunctionExpr>(ownExpr(arg)))
        results.push_back({returned(f->fn), 0, 0});
    }
    // Where rejections come back: the callback of `catch`, the second one of
    // `then`, `reason` of what `allSettled` gives, the `errors` of the
    // AggregateError of `any`.
    switch (c->builtin) {
    // The intrinsics of the built-in modules written in tz: `__tz_as<C>(x)`
    // and `__tz_chunk<T>(u)` give x itself seen as another type,
    // `__tz_listener<F>(f)` a function that calls f; the others give
    // numbers, strings and the runtime's objects, and keep nothing.
    case Builtin::LibAs:
    case Builtin::LibChunk:
    case Builtin::LibListener: return args.empty() ? Refs() : args[0];
    case Builtin::LibIsChunk:
    case Builtin::LibChunkKind:
    case Builtin::LibTypeName:
    case Builtin::LibOverrides:
    case Builtin::LibFsOpen:
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
    case Builtin::LibFsClose:
    case Builtin::LibStdinChunk:
    case Builtin::LibStdWrite:
    case Builtin::LibOrder:
    case Builtin::LibNativeSignal: return {};
    case Builtin::LibAhead: return args.empty() ? Refs() : args[0];
    case Builtin::PromiseReject:
      for (const Refs &a : args)
        flow(a, thrown_);
      return {};
    case Builtin::PromiseCatch:
      if (!c->args.empty())
        callback(c->args[0], {{thrown_, 0, 0}});
      break;
    case Builtin::PromiseThen:
      if (c->args.size() > 1)
        callback(c->args[1], {{thrown_, 0, 0}});
      break;
    case Builtin::PromiseAllSettled: flow({{thrown_, 0, 0}}, fieldNode("reason")); break;
    case Builtin::PromiseAny: flow(holding({{thrown_, 0, 0}}), fieldNode("errors")); break;
    case Builtin::ResultFromThrowable:
    case Builtin::ResultAsyncFromPromise:
    case Builtin::ResultAsyncFromThrowable: resultOfErrors_ = true; break;
    default: break;
    }
    // `new Map(entries)`, `new Set(values)`: into the dictionary, its keys
    // and values by its type, two levels inside the entries (one of a Set's
    // values).
    if (c->builtin == Builtin::DictNew && args.size() == 1)
      if (int into = contents(c->type); into >= 0) {
        flow(shifted(args[0], c->type->nonOptional()->isSet() ? 1 : 2), into);
        return {};
      }
    switch (c->builtin) {
    case Builtin::ConsoleLog:
    case Builtin::ConsoleError:
    case Builtin::StringOf:
    case Builtin::JsonStringify:
    case Builtin::ArrJoin:
    case Builtin::ArrIndexOf:
    case Builtin::ArrLastIndexOf:
    case Builtin::ArrIncludes:
    case Builtin::ArrForEach:
    case Builtin::ArrSome:
    case Builtin::ArrEvery:
    case Builtin::ArrFindIndex:
    case Builtin::ArrFindLastIndex:
    case Builtin::ArrToString:
    case Builtin::ObjectKeys:
    case Builtin::MapHas:
    case Builtin::MapDelete:
    case Builtin::MapClear:
    case Builtin::MapSize: return {};
    case Builtin::ArrPush:
    case Builtin::ArrUnshift: insert(0); return {};
    case Builtin::ArrFill: insert(0); return receiver;
    case Builtin::ArrSplice: insert(2); return copied(receiver);
    case Builtin::ArrReverse:
    case Builtin::ArrSort:
    case Builtin::ArrSortWith: return receiver;
    case Builtin::ArrPop:
    case Builtin::ArrShift:
    case Builtin::ArrAt:
    case Builtin::ArrFind:
    case Builtin::ArrFindLast:
    case Builtin::MapGet: return parts;
    case Builtin::ArrSlice:
    case Builtin::ArrFilter:
    case Builtin::ObjectValues:
    case Builtin::ObjectEntries:
    case Builtin::MapValues:
    case Builtin::MapEntries:
    case Builtin::MapKeys:
    case Builtin::SetValues:
    case Builtin::SetEntries:
    case Builtin::ArrValues:
    case Builtin::ArrEntries: return copied(receiver);
    case Builtin::ArrConcat: {
      Refs out = copied(receiver);
      for (size_t i = 0; i < args.size(); ++i) {
        bool array = c->args[i]->type && c->args[i]->type->nonOptional()->isArray();
        append(out, array ? copied(args[i]) : holding(args[i]));
      }
      return out;
    }
    case Builtin::ArrMap:
    case Builtin::ArrFlatMap: {
      Refs out = copied(receiver);
      append(out, holding(results));
      return out;
    }
    case Builtin::ArrReduce:
    case Builtin::ArrReduceRight: {
      Refs out = parts;
      append(out, results);
      for (const Refs &a : args)
        append(out, a);
      return out;
    }
    case Builtin::MapSet:
    case Builtin::SetAdd:
    case Builtin::RecordSet:
    case Builtin::RecordNullishSet:
      // Into the dictionary: its values (and a Set's keys) by their type.
      if (held >= 0) {
        for (const Refs &a : args)
          flow(a, held);
      } else {
        for (const Refs &a : args)
          escape(a);
      }
      return receiver;
    default: {
      // Any other: what it gives may be its receiver, a part of it, or an
      // argument; what it keeps is not followed.
      Refs out = receiver;
      append(out, parts);
      for (const Refs &a : args) {
        append(out, a);
        append(out, shifted(a, 1));
      }
      append(out, results);
      for (size_t i = 0; i < args.size(); ++i)
        if (holdsReferences(c->args[i]->type))
          escape(args[i]);
      return out;
    }
    }
  }

  // What the value of `e` may hold or be: nothing for a string, a number,
  // or another type that holds no references, whatever it was made of.
  Refs expr(Expr *e) {
    Refs refs = exprRefs(e);
    if (e && e->type && !holdsReferences(e->type))
      return {};
    return refs;
  }

  Refs exprRefs(Expr *e) {
    if (!e)
      return {};
    switch (e->kind()) {
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StringLit:
    case Expr::Kind::Undefined:
    case Expr::Kind::RegexLit: return {};
    case Expr::Kind::Template:
      for (Expr *part : cast<TemplateExpr>(e)->exprs)
        expr(part);
      return {};
    case Expr::Kind::ArrayLit: {
      Refs out;
      for (ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements) {
        Refs value = expr(el.expr);
        append(out, el.spread ? copied(value) : holding(value));
      }
      return out;
    }
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(e);
      Refs out;
      for (ObjectLitExpr::Property &p : lit->props) {
        if (p.key)
          expr(p.key);
        Refs value = expr(p.value);
        if (!p.spread && !p.name.empty())
          flow(value, fieldNode(p.name));
        append(out, p.spread ? copied(value) : holding(value));
      }
      // The methods of the literal get it as `this`.
      if (lit->cls)
        for (FunctionDecl *fn : lit->cls->methods) {
          function(fn);
          if (fn->thisVar)
            flow(out, var(fn->thisVar));
        }
      return out;
    }
    case Expr::Kind::Name: {
      auto *n = cast<NameExpr>(e);
      if (n->func)
        functionValue(n->func->decl);
      return name(n);
    }
    case Expr::Kind::Member: {
      auto *member = cast<MemberExpr>(e);
      Refs object = expr(member->object);
      // A method as a value is bound to the object: its `this`.
      if (member->method) {
        passThis(object, member->method->decl);
        functionValue(member->method->decl);
        return {};
      }
      Refs out = shifted(object, 1);
      out.push_back({fieldNode(member->name), 0, 0});
      return out;
    }
    case Expr::Kind::Index: {
      auto *index = cast<IndexExpr>(e);
      Refs object = expr(index->object);
      expr(index->index);
      Refs out = shifted(object, 1);
      const Type *objectType = index->object->type;
      if (int held = contents(objectType); held >= 0)
        out.push_back({held, 0, 0});
      else if (!index->field.empty())
        out.push_back({fieldNode(index->field), 0, 0});
      else if (objectType && objectType->nonOptional()->isTuple())
        if (auto *key = dyn_cast<IntLitExpr>(index->index))
          out.push_back({fieldNode(std::to_string(key->value)), 0, 0});
      return out;
    }
    case Expr::Kind::Unary: {
      auto *u = cast<UnaryExpr>(e);
      Refs operand = expr(u->operand);
      return u->op == UnaryOp::NonNull ? operand : Refs();
    }
    case Expr::Kind::Binary: {
      auto *b = cast<BinaryExpr>(e);
      Refs l = expr(b->lhs), r = expr(b->rhs);
      if (b->op != BinaryOp::LogAnd && b->op != BinaryOp::LogOr && b->op != BinaryOp::Nullish)
        return {};
      append(l, r);
      return l;
    }
    case Expr::Kind::Assign: {
      auto *a = cast<AssignExpr>(e);
      Refs value = expr(a->value);
      assignTo(a->target, value, a->value, a->op.has_value());
      return value;
    }
    case Expr::Kind::Update: assignTo(cast<UpdateExpr>(e)->target, {}, nullptr, true); return {};
    case Expr::Kind::Call: return call(cast<CallExpr>(e));
    case Expr::Kind::New: {
      auto *n = cast<NewExpr>(e);
      CallExpr *ctor = n->ctor;
      Refs out;
      std::vector<Refs> args;
      for (Expr *arg : ctor ? ctor->args : n->args) {
        args.push_back(expr(arg));
        append(out, holding(args.back()));
      }
      const FunctionDecl *target = !ctor                                ? nullptr
                                   : ctor->method && ctor->method->decl ? ctor->method->decl
                                   : ctor->func                         ? ctor->func->decl
                                                                        : nullptr;
      if (!target)
        return out;
      passArguments(ctor, target, args);
      // The object is the `this` of its constructor. One of a class up the
      // chain (the class has none of its own) sets the fields that the
      // class's own initializers set after it.
      if (target->thisVar) {
        out.push_back({var(target->thisVar), 0, 0});
        if (n->cls && target->cls && target->cls != n->cls)
          passThis(receiverConversion(n, n->cls->type, target, {}, target->thisVar,
                                      "as 'this' of the constructor of '" +
                                          target->cls->name.str() + "'"),
                   target);
      }
      return out;
    }
    case Expr::Kind::Conditional: {
      auto *c = cast<ConditionalExpr>(e);
      expr(c->cond);
      Refs out = expr(c->thenExpr);
      append(out, expr(c->elseExpr));
      return out;
    }
    case Expr::Kind::Cast: {
      auto *conv = cast<CastExpr>(e);
      Refs out = expr(conv->operand);
      if (widenings_.contains(conv)) {
        int at = node(NodeKind::Conversion, conv, "");
        auto *n = dyn_cast<NameExpr>(ownExpr(conv->operand));
        conversions.push_back({at, conv->operand->type, conv->type, conv->loc(),
                               narrowings_(conv->operand->type, conv->type), -1, "",
                               n ? n->name : llvm::StringRef()});
        out.push_back({at, 0, 0});
      }
      return out;
    }
    case Expr::Kind::Sequence: {
      Refs last;
      for (Expr *part : cast<SequenceExpr>(e)->exprs)
        last = expr(part);
      return last;
    }
    case Expr::Kind::OptionalChain: return expr(cast<OptionalChainExpr>(e)->chain);
    case Expr::Kind::Function:
      function(cast<FunctionExpr>(e)->fn);
      functionValue(cast<FunctionExpr>(e)->fn);
      return {};
    case Expr::Kind::Yield: escape(expr(cast<YieldExpr>(e)->value)); return {};
    case Expr::Kind::Await: {
      // What the promise gives: what went to promises of its type.
      Expr *value = cast<AwaitExpr>(e)->value;
      Refs out = expr(value);
      if (int held = contents(value->type); held >= 0)
        out.push_back({held, 0, 0});
      return out;
    }
    }
    return {};
  }

  const llvm::DenseSet<const CastExpr *> &widenings_;
  llvm::function_ref<std::vector<Narrowing>(const Type *, const Type *)> narrowings_;
  llvm::function_ref<bool(const Type *, const Type *)> convertible_;
  std::map<std::tuple<int, const void *, std::string>, int> ids_;
  std::set<const FunctionDecl *> walked_;
  std::vector<const FunctionDecl *> functions_;
  // The functions the program uses as values, and the calls of values.
  struct ValueCall {
    const CallExpr *call;
    std::vector<Refs> args;
    int result;
  };
  std::vector<const FunctionDecl *> functionValues_;
  std::set<const FunctionDecl *> functionSet_;
  std::vector<ValueCall> valueCalls_;
  std::set<const VarSymbol *> runtimeFunctions_;
  std::set<const VarSymbol *> runtimeRejects_;
  std::vector<bool> plain_; // by node: of a type that holds no references
  // Whether the program makes a Result of what is thrown or rejected.
  bool resultOfErrors_ = false;
};

} // namespace

bool Sema::widensWrites(const Type *from, const Type *to) {
  const Type *a = from->nonOptional(), *b = to->nonOptional();
  if (a == b)
    return false;
  bool arrays = a->isArray() && b->isArray();
  bool objects = a->isObject() && b->isObject() && !a->isAnyObject() && !b->isAnyObject();
  if (!arrays && !objects && !(a->isUnion() && b->isObject()))
    return false;
  auto key = std::make_pair(a, b);
  if (auto found = widens_.find(key); found != widens_.end())
    return found->second;
  std::vector<Narrowing> out;
  std::set<std::pair<const Type *, const Type *>> seen;
  collectNarrowings(a, b, 0, out, seen);
  return widens_[key] = !out.empty();
}

void Sema::checkWidenings(SourceFile &file) {
  if (widenings_.empty() && !narrowedFields_)
    return;
  auto narrowings = [](const Type *from, const Type *to) {
    std::vector<Narrowing> out;
    std::set<std::pair<const Type *, const Type *>> seen;
    collectNarrowings(from, to, 0, out, seen);
    return out;
  };
  auto convertible = [](const Type *from, const Type *to) {
    return isImplicitlyConvertible(from, to);
  };
  Flows flows(widenings_, narrowings, convertible);
  flows.walkProgram(file);

  // The writes by the nodes they write to.
  std::vector<std::vector<std::pair<size_t, size_t>>> writesAt(flows.edges.size());
  for (size_t w = 0; w < flows.writes.size(); ++w)
    for (size_t r = 0; r < flows.writes[w].target.size(); ++r)
      writesAt[size_t(flows.writes[w].target[r].node)].push_back({w, r});

  // Whether the write breaks a value whose part at `depth` is narrowed as `n`
  // says (at any depth with `depth` negative): a write to a read-only field,
  // or of a value that is not of the narrow type.
  auto breaks = [](const Write &w, const Narrowing &n, int depth) {
    if ((depth >= 0 && n.depth != depth) || w.element != n.element)
      return false;
    if (!w.element && w.name != "*" && w.name != n.name)
      return false;
    return !n.narrow || !w.value || !isImplicitlyConvertible(w.value, n.narrow);
  };
  // Whether a write may change the part `n` is about: it writes through a
  // type the part may be seen as (an Animal[] or an unknown[] of a Dog[],
  // not a string[]).
  auto mayBe = [](const Write &w, const Narrowing &n) {
    const Type *through = w.through ? w.through->nonOptional() : nullptr;
    if (!through || !n.owner || mentionsParam(through) || through->isUnknown() ||
        through->isAnyObject())
      return true;
    if (n.element)
      return !through->isArray() || !n.narrow || mentionsParam(through->inner()) ||
             isImplicitlyConvertible(n.narrow, through->inner());
    return isImplicitlyConvertible(n.owner, through);
  };
  // Whether a place of `type` may hold a value of `value`'s type `depth`
  // levels inside what it holds (0: the value itself). Arrays, tuples, unions
  // and dictionaries say what they hold; an object may be of a class further
  // down its chain, with fields of its own.
  std::function<bool(const Type *, const Type *, int)> mayHold = [&](const Type *type,
                                                                     const Type *value,
                                                                     int depth) -> bool {
    if (!type)
      return true;
    type = type->nonOptional();
    if (type->isUnknown() || type->isAnyObject() || type->isError() || mentionsParam(type))
      return true;
    if (depth == 0)
      return isImplicitlyConvertible(value, type);
    if (!holdsReferences(type))
      return false;
    if (type->isArray())
      return mayHold(type->inner(), value, depth - 1);
    if (type->isDict())
      return mayHold(type->keyType(), value, depth - 1) ||
             mayHold(type->valueType(), value, depth - 1);
    if (type->isUnion())
      return llvm::any_of(type->members(),
                          [&](const Type *m) { return mayHold(m, value, depth); });
    if (type->isTuple())
      return llvm::any_of(type->fields(),
                          [&](const Field &f) { return mayHold(f.type, value, depth - 1); });
    return true;
  };
  // Whether a value of `type` may be a part of a value of `whole`'s type
  // `depth` levels inside it: an element of an array, a field of an object
  // of a class nothing extends (which has no fields but its own).
  std::function<bool(const Type *, const Type *, int)> mayBePart = [&](const Type *type,
                                                                       const Type *whole,
                                                                       int depth) -> bool {
    if (!type || !whole)
      return true;
    type = type->nonOptional();
    whole = whole->nonOptional();
    if (depth == 0)
      return mayHold(type, whole, 0) || mayHold(whole, type, 0);
    if (whole->isUnknown() || whole->isAnyObject() || whole->isError() || mentionsParam(whole))
      return true;
    if (!holdsReferences(whole))
      return false;
    if (whole->isArray())
      return mayBePart(type, whole->inner(), depth - 1);
    if (whole->isDict())
      return mayBePart(type, whole->keyType(), depth - 1) ||
             mayBePart(type, whole->valueType(), depth - 1);
    if (whole->isUnion())
      return llvm::any_of(whole->members(),
                          [&](const Type *m) { return mayBePart(type, m, depth); });
    const ClassDecl *cls = whole->classDecl();
    if (whole->isTuple() || (cls && cls->children.empty()))
      return llvm::any_of(whole->fields(),
                          [&](const Field &f) { return mayBePart(type, f.type, depth - 1); });
    return true;
  };
  std::set<const char *> reported;
  for (const Conversion &conv : flows.conversions) {
    if (conv.narrowings.empty() || !reported.insert(conv.loc.getPointer()).second)
      continue;
    // Where the value goes, and how deep inside it each place is.
    std::set<std::tuple<int, int, int>> seen;
    std::vector<std::tuple<int, int, int>> todo{{conv.node, 0, 0}};
    const Write *found = nullptr;
    const Narrowing *broken = nullptr;
    bool unknown = false;
    while (!todo.empty() && !found) {
      auto [node, off, md] = todo.back();
      todo.pop_back();
      if (off > kMaxDepth || off < -16 || md > 16 || !seen.insert({node, off, md}).second)
        continue;
      // A variable that holds the converted value, holds it inside or is a
      // part of it is of a type that may: a listener of Event is no place of
      // a Dog[], nor a pair of strings a part of one.
      if (size_t(node) < flows.types.size())
        if (const Type *type = flows.types[size_t(node)];
            type && (off <= 0 ? !mayHold(type, conv.from, -off) && !mayHold(type, conv.to, -off)
                              : !mayBePart(type, conv.from, off)))
          continue;
      // A place the check does not follow: any write may meet it (below).
      if (node == flows.unknown_) {
        unknown = true;
        continue;
      }
      for (auto [w, r] : writesAt[size_t(node)]) {
        const Write &write = flows.writes[w];
        const Ref &target = write.target[size_t(r)];
        if (target.md != 0 || target.off < md)
          continue;
        int depth = target.off + off;
        // The constructor of the class up the chain setting the fields of
        // `this`: the class's own initializers set them after it.
        if (node == conv.initializes && depth == 0)
          continue;
        for (const Narrowing &n : conv.narrowings)
          if (breaks(write, n, depth) && mayBe(write, n)) {
            found = &write;
            broken = &n;
            break;
          }
        if (found)
          break;
      }
      for (const Edge &e : flows.edges[size_t(node)])
        todo.push_back({e.to, off + e.off, std::max({e.md, md - e.off, 0})});
    }
    // What reached a place the check does not follow may meet any write of
    // the program; the built-in modules (library/) write only to what they
    // make, and what reaches them they follow.
    if (!found && unknown)
      for (const Write &write : flows.writes) {
        if (inLibrary(write.loc))
          continue;
        for (const Narrowing &n : conv.narrowings)
          if (breaks(write, n, -1) && mayBe(write, n)) {
            found = &write;
            broken = &n;
            break;
          }
        if (found)
          break;
      }
    if (found)
      reportWidening(conv.from, conv.to, conv.loc, conv.role, conv.name, found->loc, found->value,
                     found->through, broken->element, broken->name, broken->narrow, broken->owner,
                     broken->depth);
  }
}

bool Sema::sameReference(const Type *from, const Type *to) {
  if (from == to)
    return true;
  // An optional object or array is the reference or none.
  if (to->isOptional()) {
    const Type *inner = to->inner();
    return (inner->isObject() || inner->isArray()) && sameReference(from->nonOptional(), inner);
  }
  if (from->isOptional())
    return false;
  if (from->isArray() && to->isArray())
    return sameReference(from->inner(), to->inner());
  // Objects of a union, seen as a type each of them is.
  if (from->isUnion())
    return !from->isTagged() && llvm::all_of(from->members(), [&](const Type *m) {
      return m->isObject() && sameReference(m, to);
    });
  if (!from->isObject() || !to->isObject() || from->isAnyObject() || to->isAnyObject())
    return false;
  if (from->isSubtypeOf(to))
    return true;
  // The same fields, some of them read-only in one and not in the other.
  if (from->isClass() || to->isClass() || from->isTuple() != to->isTuple() ||
      from->fields().size() != to->fields().size() || !from->overloads().empty() ||
      !to->overloads().empty() || from->isBytes() || to->isBytes() || from->isRuntimeObject() ||
      to->isRuntimeObject())
    return false;
  return llvm::all_of(from->fields(), [&](const Field &f) {
    const Field *t = to->field(f.name);
    return t && t->type == f.type;
  });
}

void Sema::reportWidening(const Type *from, const Type *to, llvm::SMLoc at, llvm::StringRef role,
                          llvm::StringRef name, llvm::SMLoc write, const Type *value,
                          const Type *through, bool element, llvm::StringRef field,
                          const Type *narrow, const Type *owner, int depth) {
  // "line 12", or "line 12 of shapes.tz" in another file.
  llvm::SourceMgr &sources = diags_.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(write);
  std::string line =
      id ? "line " + std::to_string(sources.getLineAndColumn(write, id).first) : "a line";
  if (id && id != sources.FindBufferContainingLoc(at))
    line += " of " +
            llvm::sys::path::filename(sources.getMemoryBuffer(id)->getBufferIdentifier()).str();
  std::string message = "'" + from->name().str() + "' cannot go where '" + to->name().str() +
                        "' is expected here" + (role.empty() ? std::string() : ", " + role.str()) +
                        ": " + line;
  auto article = [](const Type *t) {
    char c = t->name().empty() ? 'x' : char(std::tolower(static_cast<unsigned char>(t->name()[0])));
    return std::string(llvm::StringRef("aeiou").contains(c) ? "an '" : "a '") + t->name().str() +
           "'";
  };
  std::string what = value ? article(value) : std::string("a value");
  if (element) {
    // An array of the narrow elements, the value itself or one in it.
    message += " puts " + what + " into " + (depth == 0 ? std::string("it") : "an array in it") +
               ", and an array of '" + narrow->name().str() + "' cannot hold one";
    if (depth == 0)
      message += "; pass a copy, '[..." + (name.empty() ? std::string("xs") : name.str()) + "]'";
  } else {
    std::string slot = owner->isTuple() ? "element " + field.str() : "field '" + field.str() + "'";
    std::string place = depth == 0 ? "its " + slot : slot + " of " + article(owner) + " in it";
    if (!narrow)
      message += " writes to " + place + ", which is read-only in '" + owner->name().str() + "'";
    else
      message += " writes " + what + " to " + place + ", which is " + article(narrow) + " in '" +
                 owner->name().str() + "'";
  }
  diags_.error(at, message);
  diags_.note(write, "the write, through '" + (through ? through : to)->name() + "'");
}

} // namespace tz
