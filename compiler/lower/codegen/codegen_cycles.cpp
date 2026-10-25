// Cycles of references without a collector (docs/objects.md, section 11):
// which types may form cycles, and the code that keeps their groups
// (runtime/core/group/group.c).
//
// The analysis builds, over the whole program, the graph of what may hold
// what: an object type holds the types of its fields, an array its elements,
// a dictionary its keys and values, the environment of a closure what it
// captures and a box the value of its variable. A reference of a type may
// point at an object of that type or of any declared to be it (subtypes,
// implementers); a function value at the environment of any closure, or at
// the object of a method taken as a value. A cycle of objects needs a cycle
// in this graph with an edge that may be set after its holder was made: a
// field that never changes after the object is made points only at objects
// made before it (a list built with `new Node(value, next)` has no cycle).
// The types on such a cycle are cyclic: their objects are members of
// groups. Every other type keeps plain counting and pays nothing.
//
// Code then counts references that may point at members through their
// groups (refMayBeMember, the sign of the count tells a member at run time),
// and a store into a slot of an object that may be a member joins the groups
// (groupStore), releases what the slot held as the member did (releaseIn),
// and a value that moves out of one takes its count back (takeFrom).

#include "compiler/lower/codegen/codegen.h"

#include "llvm/IR/MDBuilder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The type a generic copy sees a value of `type` as (Sema::representation).
const Type *representationOf(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return Type::anyObject();
  case Type::Kind::Function:
    return Type::function({}, 0, false, Type::voidTy());
  case Type::Kind::Array:
    return Type::array(representationOf(type->inner()));
  case Type::Kind::Optional:
    return Type::optional(representationOf(type->inner()));
  default:
    return type;
  }
}

std::string utf8Of(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

// The nodes of the graph: types whose objects hold references (objects,
// arrays, dictionaries), environments, boxes, and three that stand for what
// a value may refer to: any function's environment, any object (in generic
// code) and what an unknown may hold.
struct CycleGraph {
  enum Kind { TypeNode, EnvNode, BoxNode, FnNode, AnyObjectNode, UnknownNode };
  struct Node {
    Kind kind;
    const void *key;
    std::vector<std::pair<unsigned, bool>> edges; // target, may be set after it is made
  };
  std::vector<Node> nodes;
  std::map<std::pair<int, const void *>, unsigned> index;

  unsigned node(Kind kind, const void *key) {
    auto [it, inserted] = index.try_emplace({int(kind), key}, unsigned(nodes.size()));
    if (inserted)
      nodes.push_back({kind, key, {}});
    return it->second;
  }
  // Both ends are found first: making a node may move the others.
  void edge(unsigned from, unsigned to, bool late) { nodes[from].edges.push_back({to, late}); }
};

// Walks the code of the program and gathers what the analysis needs.
class CycleScanner {
public:
  // The types the program has values of, with the types they are made of.
  std::vector<const Type *> types;
  llvm::DenseSet<const Type *> seenTypes;
  // The fields that may be assigned after their object is made, by name.
  std::set<std::string> mutableFields;
  // The closures with an environment on the heap (by the key of their
  // environment: their group, or themselves) and what each captures.
  std::map<const void *, std::vector<const VarSymbol *>> envs;
  // The variables that live in boxes.
  llvm::DenseSet<const VarSymbol *> boxed;
  // The function values the program makes, by their types: the closures
  // (by the keys of their environments), and the static types of objects
  // whose methods are taken as values (the object is the environment). A
  // value of a type converted to another is one of that too.
  std::map<const Type *, std::set<const void *>> closuresOf;
  std::map<const Type *, std::set<const Type *>> boundOf;
  std::set<std::pair<const Type *, const Type *>> fnCasts; // from, to
  // The types of the callbacks given to promises and ResultAsync, which a
  // reaction holds (CodeGen::attachReaction).
  std::set<const Type *> reactionCallbacks;
  // The arrays and dictionaries that code adds to after they are made (by
  // static type): only their elements may close a cycle.
  llvm::DenseSet<const Type *> mutated;
  // The types of values that become unknowns (the objects among them are
  // held as they are).
  llvm::DenseSet<const Type *> toUnknown;

  void addType(const Type *type) {
    if (!type || !seenTypes.insert(type).second)
      return;
    types.push_back(type);
    switch (type->kind()) {
    case Type::Kind::Optional:
    case Type::Kind::Array:
    case Type::Kind::Promise:
    case Type::Kind::Iterator:
      addType(type->inner());
      break;
    case Type::Kind::Function:
      for (const Type *p : type->params())
        addType(p);
      addType(type->ret());
      break;
    case Type::Kind::Union:
      for (const Type *m : type->members())
        addType(m);
      break;
    case Type::Kind::Result:
      addType(type->okType());
      addType(type->errType());
      break;
    case Type::Kind::Dict:
      addType(type->keyType());
      addType(type->valueType());
      break;
    case Type::Kind::Object:
      for (const Field &f : type->fields())
        addType(f.type);
      for (const Type *b : type->bases())
        addType(b);
      if (const ClassDecl *cls = type->classDecl(); cls && cls->group)
        for (const VarSymbol *var : cls->group->captures)
          addVar(var);
      break;
    default:
      break;
    }
  }

  void addVar(const VarSymbol *var) {
    if (!var)
      return;
    addType(var->type);
    if (var->boxed())
      boxed.insert(var);
  }

  // A function and what it declares, once.
  void function(const FunctionDecl *fn) {
    if (!fn || !scanned_.insert(fn).second)
      return;
    if (fn->sym) {
      for (const Type *p : fn->sym->params)
        addType(p);
      addType(fn->sym->ret);
    }
    for (const Param &param : fn->params) {
      addVar(param.sym);
      expr(param.defaultValue);
    }
    addVar(fn->thisVar);
    for (const VarSymbol *var : fn->captures)
      addVar(var);
    const FunctionDecl *saved = current_;
    current_ = fn;
    stmt(fn->body);
    current_ = saved;
  }

  // A class's static fields and decorations, where it is declared.
  void classDecl(const ClassDecl *cls) {
    addType(cls->type);
    for (const ClassField &f : cls->fields) {
      addType(f.resolved);
      if (f.isStatic && f.var) {
        addVar(f.var);
        expr(f.init);
      }
    }
    for (const Stmt *s : cls->decorations)
      stmt(s);
  }

  void stmt(const Stmt *stmt) {
    if (!stmt)
      return;
    switch (stmt->kind()) {
    case Stmt::Kind::Block: {
      auto *block = cast<BlockStmt>(stmt);
      if (NestedGroup *group = block->group) {
        envs[group].assign(group->captures.begin(), group->captures.end());
        for (const VarSymbol *var : group->captures)
          addVar(var);
        for (const FunctionDecl *member : group->members) {
          if (member->sym)
            closuresOf[functionType(member)].insert(group);
          function(member);
        }
      }
      for (const Stmt *s : block->body)
        this->stmt(s);
      break;
    }
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
        addVar(d.sym);
        expr(d.init);
      }
      break;
    case Stmt::Kind::Function:
      function(cast<FunctionDecl>(stmt));
      break;
    case Stmt::Kind::Expr:
      expr(cast<ExprStmt>(stmt)->expr);
      break;
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      expr(s->cond);
      this->stmt(s->thenStmt);
      this->stmt(s->elseStmt);
      break;
    }
    case Stmt::Kind::While:
      expr(cast<WhileStmt>(stmt)->cond);
      this->stmt(cast<WhileStmt>(stmt)->body);
      break;
    case Stmt::Kind::DoWhile:
      this->stmt(cast<DoWhileStmt>(stmt)->body);
      expr(cast<DoWhileStmt>(stmt)->cond);
      break;
    case Stmt::Kind::For: {
      auto *s = cast<ForStmt>(stmt);
      this->stmt(s->init);
      expr(s->cond);
      expr(s->step);
      this->stmt(s->body);
      break;
    }
    case Stmt::Kind::ForOf: {
      auto *s = cast<ForOfStmt>(stmt);
      addVar(s->binding.sym);
      expr(s->iterable);
      this->stmt(s->body);
      break;
    }
    case Stmt::Kind::Switch: {
      auto *s = cast<SwitchStmt>(stmt);
      expr(s->discriminant);
      for (const SwitchCase &c : s->cases) {
        expr(c.test);
        for (const Stmt *body : c.body)
          this->stmt(body);
      }
      break;
    }
    case Stmt::Kind::Labeled:
      this->stmt(cast<LabeledStmt>(stmt)->body);
      break;
    case Stmt::Kind::Return:
      expr(cast<ReturnStmt>(stmt)->value);
      break;
    case Stmt::Kind::Throw:
      expr(cast<ThrowStmt>(stmt)->value);
      break;
    case Stmt::Kind::Try: {
      auto *s = cast<TryStmt>(stmt);
      this->stmt(s->block);
      addVar(s->param.sym);
      this->stmt(s->handler);
      this->stmt(s->finalizer);
      break;
    }
    case Stmt::Kind::Class:
      classDecl(cast<ClassDecl>(stmt));
      break;
    default:
      break;
    }
  }

  void expr(const Expr *expr) {
    if (!expr)
      return;
    addType(expr->type);
    switch (expr->kind()) {
    case Expr::Kind::Template:
      for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
        this->expr(e);
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
        this->expr(e.expr);
      break;
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(expr);
      if (lit->cls) {
        addType(lit->cls->type);
        for (const FunctionDecl *fn : lit->cls->methods)
          function(fn);
      }
      for (const ObjectLitExpr::Property &p : lit->props) {
        this->expr(p.key);
        if (!p.method)
          this->expr(p.value);
        // A method of an interface, bound to the object a spread copies it
        // from (see emitSpreadLiteral).
        if (p.spread && p.value->type && p.value->type->isObject())
          for (const Field &f : p.value->type->fields())
            if (f.type->isFunction())
              boundOf[f.type].insert(p.value->type);
      }
      break;
    }
    case Expr::Kind::Name:
      if (auto *name = cast<NameExpr>(expr); name->var)
        addVar(name->var);
      break;
    case Expr::Kind::Member: {
      auto *member = cast<MemberExpr>(expr);
      this->expr(member->object);
      // A method as a value is bound to its object; so is a member of an
      // interface that holds a function, which a method of a class that
      // implements it may be.
      const Type *object = member->object->type ? member->object->type->nonOptional() : nullptr;
      if (member->type && member->type->nonOptional()->isFunction() && object &&
          !callee_.contains(member) &&
          (member->method || (object->isObject() && !object->isClass() && object->isBase())))
        boundOf[member->type->nonOptional()].insert(member->object->type);
      break;
    }
    case Expr::Kind::Index:
      this->expr(cast<IndexExpr>(expr)->object);
      this->expr(cast<IndexExpr>(expr)->index);
      break;
    case Expr::Kind::Unary:
      this->expr(cast<UnaryExpr>(expr)->operand);
      break;
    case Expr::Kind::Binary: {
      auto *bin = cast<BinaryExpr>(expr);
      this->expr(bin->lhs);
      if (bin->op != BinaryOp::InstanceOf)
        this->expr(bin->rhs);
      break;
    }
    case Expr::Kind::Assign: {
      auto *assign = cast<AssignExpr>(expr);
      assigned(assign);
      if (auto *index = dyn_cast<IndexExpr>(assign->target); index && index->object->type)
        mutated.insert(index->object->type->nonOptional());
      target(assign->target);
      this->expr(assign->value);
      break;
    }
    case Expr::Kind::Update:
      target(cast<UpdateExpr>(expr)->target);
      break;
    case Expr::Kind::Call: {
      auto *call = cast<CallExpr>(expr);
      callee_.insert(call->callee);
      this->expr(call->callee);
      for (const Expr *arg : call->args)
        this->expr(arg);
      if (isPromiseBuiltin(call->builtin) || (call->builtin >= Builtin::ResultAsyncFromPromise &&
                                              call->builtin <= Builtin::ResultAsyncTeeDone))
        for (const Expr *arg : call->args)
          if (arg->type && arg->type->nonOptional()->isFunction())
            reactionCallbacks.insert(arg->type->nonOptional());
      switch (call->builtin) {
      case Builtin::ArrPush:
      case Builtin::ArrUnshift:
      case Builtin::ArrSplice:
      case Builtin::ArrFill:
      case Builtin::MapSet:
      case Builtin::SetAdd:
      case Builtin::RecordSet:
      case Builtin::RecordNullishSet:
        if (auto *member = dyn_cast<MemberExpr>(call->callee); member && member->object->type)
          mutated.insert(member->object->type->nonOptional());
        break;
      default:
        break;
      }
      break;
    }
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(expr);
      for (const Expr *arg : create->ctor ? create->ctor->args : create->args)
        this->expr(arg);
      break;
    }
    case Expr::Kind::Conditional: {
      auto *cond = cast<ConditionalExpr>(expr);
      this->expr(cond->cond);
      this->expr(cond->thenExpr);
      this->expr(cond->elseExpr);
      break;
    }
    case Expr::Kind::Cast: {
      auto *conv = cast<CastExpr>(expr);
      this->expr(conv->operand);
      if (conv->type && conv->operand->type &&
          (conv->type->nonOptional()->isUnknown() || conv->type->nonOptional()->isAnyObject()))
        toUnknown.insert(conv->operand->type);
      if (conv->type && conv->operand->type && conv->type->nonOptional()->isFunction() &&
          conv->operand->type->nonOptional()->isFunction())
        fnCasts.insert({conv->operand->type->nonOptional(), conv->type->nonOptional()});
      break;
    }
    case Expr::Kind::Sequence:
      for (const Expr *e : cast<SequenceExpr>(expr)->exprs)
        this->expr(e);
      break;
    case Expr::Kind::OptionalChain:
      this->expr(cast<OptionalChainExpr>(expr)->chain);
      break;
    case Expr::Kind::Function: {
      const FunctionDecl *fn = cast<FunctionExpr>(expr)->fn;
      if (!fn->nonEscaping) {
        const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
        const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
        envs[key].assign(captures.begin(), captures.end());
        if (expr->type)
          closuresOf[expr->type->nonOptional()].insert(key);
        if (fn->sym)
          closuresOf[functionType(fn)].insert(key);
      }
      function(fn);
      break;
    }
    case Expr::Kind::Yield:
      this->expr(cast<YieldExpr>(expr)->value);
      break;
    case Expr::Kind::Await:
      this->expr(cast<AwaitExpr>(expr)->value);
      break;
    default:
      break;
    }
  }

private:
  static const Type *functionType(const FunctionDecl *fn) {
    const FuncSymbol *sym = fn->sym;
    return Type::function(sym->params, sym->minArgs, sym->hasRest, sym->ret, sym->hasThis);
  }

  void target(const Expr *target) {
    if (auto *index = dyn_cast<IndexExpr>(target)) {
      expr(index->object);
      expr(index->index);
    } else if (auto *member = dyn_cast<MemberExpr>(target)) {
      expr(member->object);
    } else {
      expr(target);
    }
  }

  // An assignment to a field: one that the construction of the object makes
  // does not close a cycle (see constructionWrite).
  void assigned(const AssignExpr *assign) {
    const Expr *target = assign->target;
    std::vector<std::string> names;
    if (auto *member = dyn_cast<MemberExpr>(target)) {
      if (constructionWrite(assign))
        return;
      names.push_back(member->name.str());
    } else if (auto *index = dyn_cast<IndexExpr>(target)) {
      if (!index->field.empty())
        names.push_back(index->field.str());
      if (index->keyed && index->index->type) {
        const Type *key = index->index->type;
        for (const Type *n : key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key))
          if (n->isStringLiteral()) {
            const std::u16string &text = n->literalText();
            std::string name;
            llvm::convertUTF16ToUTF8String(
                llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()),
                                            text.size()),
                name);
            names.push_back(std::move(name));
          }
      }
    }
    for (std::string &name : names)
      mutableFields.insert(std::move(name));
  }

  // `this.f = v` in the construction of an object (a constructor or the
  // initializers of the fields), before `this` went anywhere, where v does
  // not hold it: f then points at objects made before this one.
  bool constructionWrite(const AssignExpr *assign) {
    const FunctionDecl *fn = current_;
    if (!fn || !fn->cls || !fn->thisVar || (!fn->isConstructor && fn != fn->cls->init))
      return false;
    auto [it, inserted] = safeWrites_.try_emplace(fn);
    if (inserted) {
      const ClassDecl *cls = fn->cls;
      bool initEscapes = cls->init && cls->init->thisVar && thisEscapes(cls->init->body, cls->init->thisVar);
      // Before a constructor's body: the initializers, which run first in a
      // class that extends none; in one that does, `super()` runs the class
      // extended and then them.
      bool escaped = fn == cls->init ? cls->parent && constructionEscapes(cls->parent)
                                     : !cls->parent && initEscapes;
      bool afterSuper = (cls->parent && constructionEscapes(cls->parent)) || initEscapes;
      ordered(fn->body, fn->thisVar, escaped, afterSuper, it->second);
    }
    return it->second.contains(assign);
  }

  // Walks `stmt` in the order it runs, noting the writes to fields of `self`
  // made while `escaped` is still false; loops, `switch` and `try` count as
  // a whole.
  static void ordered(const Stmt *stmt, const VarSymbol *self, bool &escaped, bool afterSuper,
                      llvm::DenseSet<const AssignExpr *> &safe) {
    if (!stmt)
      return;
    auto expr = [&](const Expr *e) { orderedExpr(e, self, escaped, afterSuper, safe); };
    switch (stmt->kind()) {
    case Stmt::Kind::Block:
      for (const Stmt *s : cast<BlockStmt>(stmt)->body)
        ordered(s, self, escaped, afterSuper, safe);
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
        expr(d.init);
      break;
    case Stmt::Kind::Expr:
      expr(cast<ExprStmt>(stmt)->expr);
      break;
    case Stmt::Kind::Return:
      expr(cast<ReturnStmt>(stmt)->value);
      break;
    case Stmt::Kind::Throw:
      expr(cast<ThrowStmt>(stmt)->value);
      break;
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      expr(s->cond);
      bool before = escaped;
      ordered(s->thenStmt, self, escaped, afterSuper, safe);
      bool then = escaped;
      escaped = before;
      ordered(s->elseStmt, self, escaped, afterSuper, safe);
      escaped = escaped || then;
      break;
    }
    case Stmt::Kind::Labeled:
      ordered(cast<LabeledStmt>(stmt)->body, self, escaped, afterSuper, safe);
      break;
    case Stmt::Kind::Function: {
      // A nested function that captures `self` lets it go.
      auto *fn = cast<FunctionDecl>(stmt);
      escaped = escaped || llvm::is_contained(fn->captures, self);
      break;
    }
    default:
      // A loop may run its body again after `self` went in it, and so may a
      // `switch` fall through or a `try` jump: their writes count as late if
      // `self` goes anywhere in them.
      escaped = escaped || thisEscapes(stmt, self);
      walkStmt(stmt, [&](const Expr *e) {
        bool late = true;
        orderedExpr(e, self, late, afterSuper, safe);
      });
      break;
    }
  }

  static void orderedExpr(const Expr *e, const VarSymbol *self, bool &escaped, bool afterSuper,
                          llvm::DenseSet<const AssignExpr *> &safe) {
    if (!e)
      return;
    if (auto *assign = dyn_cast<AssignExpr>(e)) {
      auto *member = dyn_cast<MemberExpr>(assign->target);
      auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
      if (object && object->var == self && !assign->op) {
        orderedExpr(assign->value, self, escaped, afterSuper, safe);
        if (!escaped && !mentions(assign->value, self))
          safe.insert(assign);
        return;
      }
    }
    if (auto *call = dyn_cast<CallExpr>(e); call && call->superCall && !isa<MemberExpr>(call->callee)) {
      for (const Expr *arg : call->args)
        escaped = escaped || exprEscapes(arg, self);
      escaped = escaped || afterSuper;
      return;
    }
    escaped = escaped || exprEscapes(e, self);
  }

  static bool exprEscapes(const Expr *expr, const VarSymbol *self) {
    ExprStmt stmt(llvm::SMLoc(), const_cast<Expr *>(expr));
    return thisEscapes(&stmt, self);
  }

  // Whether the construction of an object of `cls` lets `this` go anywhere
  // before it ends: anything but writing and reading its fields, there or
  // in the classes up the chain.
  bool constructionEscapes(const ClassDecl *cls) {
    auto [it, inserted] = escapes_.try_emplace(cls, true);
    if (!inserted)
      return it->second;
    bool escapes = cls->parent && constructionEscapes(cls->parent);
    for (const FunctionDecl *ctor : cls->constructors)
      escapes = escapes || (ctor->thisVar && thisEscapes(ctor->body, ctor->thisVar));
    if (cls->init && cls->init->thisVar)
      escapes = escapes || thisEscapes(cls->init->body, cls->init->thisVar);
    escapes_[cls] = escapes;
    return escapes;
  }

public:
  // Whether `self` is used in `stmt` for anything but a field: as an
  // argument, a value, a method's object, or captured by a closure.
  static bool thisEscapes(const Stmt *stmt, const VarSymbol *self) {
    bool escapes = false;
    std::function<void(const Expr *)> visit = [&](const Expr *e) {
      if (!e || escapes)
        return;
      // A method gets the object: called on it, or on `super`.
      if (auto *call = dyn_cast<CallExpr>(e)) {
        auto *member = dyn_cast<MemberExpr>(call->callee);
        auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
        if ((call->superCall && member) ||
            (object && object->var == self && (call->method || !call->unionMethods.empty()))) {
          escapes = true;
          return;
        }
      }
      if (auto *member = dyn_cast<MemberExpr>(e)) {
        auto *object = dyn_cast<NameExpr>(member->object);
        if (object && object->var == self && !member->method)
          return; // a field of it
        visit(member->object);
        return;
      }
      if (auto *name = dyn_cast<NameExpr>(e)) {
        escapes = name->var == self;
        return;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(e)) {
        escapes = llvm::is_contained(fe->fn->captures, self) || mentionsIn(fe->fn->body, self);
        return;
      }
      forEachChild(e, visit);
    };
    walkStmt(stmt, visit);
    return escapes;
  }

private:
  static bool mentions(const Expr *expr, const VarSymbol *self) {
    bool found = false;
    std::function<void(const Expr *)> visit = [&](const Expr *e) {
      if (!e || found)
        return;
      if (auto *name = dyn_cast<NameExpr>(e)) {
        found = name->var == self;
        return;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(e)) {
        found = llvm::is_contained(fe->fn->captures, self) || mentionsIn(fe->fn->body, self);
        return;
      }
      forEachChild(e, visit);
    };
    visit(expr);
    return found;
  }

  static bool mentionsIn(const Stmt *stmt, const VarSymbol *self) {
    bool found = false;
    walkStmt(stmt, [&](const Expr *e) { found = found || mentions(e, self); });
    return found;
  }

  // The expressions directly in `e`.
  static void forEachChild(const Expr *e, const std::function<void(const Expr *)> &visit) {
    switch (e->kind()) {
    case Expr::Kind::Template:
      for (const Expr *x : cast<TemplateExpr>(e)->exprs)
        visit(x);
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &x : cast<ArrayLitExpr>(e)->elements)
        visit(x.expr);
      break;
    case Expr::Kind::ObjectLit:
      for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
        visit(p.key);
        visit(p.value);
      }
      break;
    case Expr::Kind::Member:
      visit(cast<MemberExpr>(e)->object);
      break;
    case Expr::Kind::Index:
      visit(cast<IndexExpr>(e)->object);
      visit(cast<IndexExpr>(e)->index);
      break;
    case Expr::Kind::Unary:
      visit(cast<UnaryExpr>(e)->operand);
      break;
    case Expr::Kind::Binary:
      visit(cast<BinaryExpr>(e)->lhs);
      visit(cast<BinaryExpr>(e)->rhs);
      break;
    case Expr::Kind::Assign:
      visit(cast<AssignExpr>(e)->target);
      visit(cast<AssignExpr>(e)->value);
      break;
    case Expr::Kind::Update:
      visit(cast<UpdateExpr>(e)->target);
      break;
    case Expr::Kind::Call:
      visit(cast<CallExpr>(e)->callee);
      for (const Expr *x : cast<CallExpr>(e)->args)
        visit(x);
      break;
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      for (const Expr *x : create->ctor ? create->ctor->args : create->args)
        visit(x);
      break;
    }
    case Expr::Kind::Conditional:
      visit(cast<ConditionalExpr>(e)->cond);
      visit(cast<ConditionalExpr>(e)->thenExpr);
      visit(cast<ConditionalExpr>(e)->elseExpr);
      break;
    case Expr::Kind::Cast:
      visit(cast<CastExpr>(e)->operand);
      break;
    case Expr::Kind::Sequence:
      for (const Expr *x : cast<SequenceExpr>(e)->exprs)
        visit(x);
      break;
    case Expr::Kind::OptionalChain:
      visit(cast<OptionalChainExpr>(e)->chain);
      break;
    case Expr::Kind::Yield:
      visit(cast<YieldExpr>(e)->value);
      break;
    case Expr::Kind::Await:
      visit(cast<AwaitExpr>(e)->value);
      break;
    default:
      break;
    }
  }

  // The expressions of `stmt` and of the statements in it (not of the
  // functions it declares).
  static void walkStmt(const Stmt *stmt, const std::function<void(const Expr *)> &visit) {
    if (!stmt)
      return;
    switch (stmt->kind()) {
    case Stmt::Kind::Block:
      for (const Stmt *s : cast<BlockStmt>(stmt)->body)
        walkStmt(s, visit);
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
        visit(d.init);
      break;
    case Stmt::Kind::Expr:
      visit(cast<ExprStmt>(stmt)->expr);
      break;
    case Stmt::Kind::If:
      visit(cast<IfStmt>(stmt)->cond);
      walkStmt(cast<IfStmt>(stmt)->thenStmt, visit);
      walkStmt(cast<IfStmt>(stmt)->elseStmt, visit);
      break;
    case Stmt::Kind::While:
      visit(cast<WhileStmt>(stmt)->cond);
      walkStmt(cast<WhileStmt>(stmt)->body, visit);
      break;
    case Stmt::Kind::DoWhile:
      walkStmt(cast<DoWhileStmt>(stmt)->body, visit);
      visit(cast<DoWhileStmt>(stmt)->cond);
      break;
    case Stmt::Kind::For:
      walkStmt(cast<ForStmt>(stmt)->init, visit);
      visit(cast<ForStmt>(stmt)->cond);
      visit(cast<ForStmt>(stmt)->step);
      walkStmt(cast<ForStmt>(stmt)->body, visit);
      break;
    case Stmt::Kind::ForOf:
      visit(cast<ForOfStmt>(stmt)->iterable);
      walkStmt(cast<ForOfStmt>(stmt)->body, visit);
      break;
    case Stmt::Kind::Switch:
      visit(cast<SwitchStmt>(stmt)->discriminant);
      for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
        visit(c.test);
        for (const Stmt *s : c.body)
          walkStmt(s, visit);
      }
      break;
    case Stmt::Kind::Labeled:
      walkStmt(cast<LabeledStmt>(stmt)->body, visit);
      break;
    case Stmt::Kind::Return:
      visit(cast<ReturnStmt>(stmt)->value);
      break;
    case Stmt::Kind::Throw:
      visit(cast<ThrowStmt>(stmt)->value);
      break;
    case Stmt::Kind::Try:
      walkStmt(cast<TryStmt>(stmt)->block, visit);
      walkStmt(cast<TryStmt>(stmt)->handler, visit);
      walkStmt(cast<TryStmt>(stmt)->finalizer, visit);
      break;
    case Stmt::Kind::Function: {
      // A nested function that captures `this` lets it go.
      auto *fn = cast<FunctionDecl>(stmt);
      for (const VarSymbol *var : fn->captures) {
        NameExpr fake(llvm::SMLoc(), var->name);
        fake.var = const_cast<VarSymbol *>(var);
        visit(&fake);
      }
      break;
    }
    default:
      break;
    }
  }

  llvm::DenseSet<const FunctionDecl *> scanned_;
  llvm::DenseSet<const Expr *> callee_;
  llvm::DenseMap<const ClassDecl *, bool> escapes_;
  std::map<const FunctionDecl *, llvm::DenseSet<const AssignExpr *>> safeWrites_;
  const FunctionDecl *current_ = nullptr;
};


// Looks through what does not change which object a value is: implicit
// conversions, `x!` and `(x)`.
const Expr *bare(const Expr *e) {
  for (;;) {
    if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit()) {
      e = conv->operand;
      continue;
    }
    if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::NonNull) {
      e = unary->operand;
      continue;
    }
    return e;
  }
}

// Layer 2 of docs/objects.md, section 11: an object that another one makes
// for itself and never gives away (a dialog's button) lives no longer than
// its owner; a function the owner stores in it that captures the owner
// (`this.button.onClick = () => this.close()`) needs no count of it, and the
// cycle owner → button → function → owner is no cycle of counts.
//
// The proof, over the whole program, for a field f of class C and a field g
// of class D: every write to a field f is `this.f = new D(...)` in the
// construction of a C (with arguments that do not hold `this`); a field f
// is read only to reach a field or a method of what it holds, never as a
// value; D's construction and methods let `this` go nowhere; and a field g is
// only ever called, never read as a value. Then the D lives only in its C's
// field, the function only in the D's field, and the function's reference
// to the C is not counted.
class OwnerScanner {
public:
  struct BackRef {
    const ClassDecl *owner;    // C
    llvm::StringRef field;     // f
    llvm::StringRef callback;  // g
    const FunctionDecl *fn;    // the function stored in g
    const VarSymbol *self;     // C's `this`, which it captures
  };

  void function(const FunctionDecl *fn) {
    if (!fn || !scanned_.insert(fn).second)
      return;
    const FunctionDecl *saved = current_;
    current_ = fn;
    for (const Param &param : fn->params)
      expr(param.defaultValue, Use::Value);
    stmt(fn->body);
    current_ = saved;
  }

  void topLevel(const Stmt *s) {
    current_ = nullptr;
    stmt(s);
  }

  // The proofs that hold, given what escapes(cls) says of classes.
  std::vector<BackRef> proven(const std::function<bool(const ClassDecl *)> &escapes) const {
    std::vector<BackRef> out;
    for (const BackRef &b : candidates_) {
      if (valueReads_.contains(b.field) || nonCallReads_.contains(b.callback) ||
          bad_.contains(b.field))
        continue;
      // Every write of f makes a new D in the construction of a C.
      auto writes = writes_.find(b.field);
      if (writes == writes_.end())
        continue;
      const ClassDecl *made = nullptr;
      bool fresh = true;
      for (const Write &w : writes->second) {
        if (!w.made || !w.cls || !w.cls->derivesFrom(b.owner) || (made && w.made != made))
          fresh = false;
        made = w.made;
      }
      if (!fresh || !made || made->builtin || !made->type->field(b.callback))
        continue;
      // D's construction and methods, up its chain, keep `this` to
      // themselves.
      bool keeps = true;
      for (const ClassDecl *c = made; c && keeps; c = c->parent)
        keeps = !escapes(c);
      if (keeps)
        out.push_back(b);
    }
    return out;
  }

  // The class each owned field makes, by name, for the checks of debug mode.
  const ClassDecl *madeBy(llvm::StringRef field) const {
    auto writes = writes_.find(field);
    return writes == writes_.end() || writes->second.empty() ? nullptr
                                                             : writes->second.front().made;
  }

private:
  enum class Use { Value, Receiver, Callee };
  struct Write {
    const ClassDecl *cls; // whose construction makes it; null elsewhere
    const ClassDecl *made;
  };

  void stmt(const Stmt *stmt) {
    if (!stmt)
      return;
    switch (stmt->kind()) {
    case Stmt::Kind::Block:
      if (NestedGroup *group = cast<BlockStmt>(stmt)->group)
        for (const FunctionDecl *member : group->members)
          function(member);
      for (const Stmt *s : cast<BlockStmt>(stmt)->body)
        this->stmt(s);
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
        expr(d.init, Use::Value);
      break;
    case Stmt::Kind::Function:
      function(cast<FunctionDecl>(stmt));
      break;
    case Stmt::Kind::Expr:
      expr(cast<ExprStmt>(stmt)->expr, Use::Value);
      break;
    case Stmt::Kind::If:
      expr(cast<IfStmt>(stmt)->cond, Use::Value);
      this->stmt(cast<IfStmt>(stmt)->thenStmt);
      this->stmt(cast<IfStmt>(stmt)->elseStmt);
      break;
    case Stmt::Kind::While:
      expr(cast<WhileStmt>(stmt)->cond, Use::Value);
      this->stmt(cast<WhileStmt>(stmt)->body);
      break;
    case Stmt::Kind::DoWhile:
      this->stmt(cast<DoWhileStmt>(stmt)->body);
      expr(cast<DoWhileStmt>(stmt)->cond, Use::Value);
      break;
    case Stmt::Kind::For:
      this->stmt(cast<ForStmt>(stmt)->init);
      expr(cast<ForStmt>(stmt)->cond, Use::Value);
      expr(cast<ForStmt>(stmt)->step, Use::Value);
      this->stmt(cast<ForStmt>(stmt)->body);
      break;
    case Stmt::Kind::ForOf:
      expr(cast<ForOfStmt>(stmt)->iterable, Use::Value);
      this->stmt(cast<ForOfStmt>(stmt)->body);
      break;
    case Stmt::Kind::Switch:
      expr(cast<SwitchStmt>(stmt)->discriminant, Use::Value);
      for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
        expr(c.test, Use::Value);
        for (const Stmt *s : c.body)
          this->stmt(s);
      }
      break;
    case Stmt::Kind::Labeled:
      this->stmt(cast<LabeledStmt>(stmt)->body);
      break;
    case Stmt::Kind::Return:
      expr(cast<ReturnStmt>(stmt)->value, Use::Value);
      break;
    case Stmt::Kind::Throw:
      expr(cast<ThrowStmt>(stmt)->value, Use::Value);
      break;
    case Stmt::Kind::Try:
      this->stmt(cast<TryStmt>(stmt)->block);
      this->stmt(cast<TryStmt>(stmt)->handler);
      this->stmt(cast<TryStmt>(stmt)->finalizer);
      break;
    case Stmt::Kind::Class: {
      auto *cls = cast<ClassDecl>(stmt);
      for (const ClassField &f : cls->fields)
        if (f.isStatic)
          expr(f.init, Use::Value);
      for (const Stmt *s : cls->decorations)
        this->stmt(s);
      break;
    }
    default:
      break;
    }
  }

  void expr(const Expr *e, Use use) {
    if (!e)
      return;
    switch (e->kind()) {
    case Expr::Kind::Cast:
      expr(cast<CastExpr>(e)->operand, cast<CastExpr>(e)->isImplicit() ? use : Use::Value);
      return;
    case Expr::Kind::Unary: {
      auto *unary = cast<UnaryExpr>(e);
      expr(unary->operand, unary->op == UnaryOp::NonNull ? use : Use::Value);
      return;
    }
    case Expr::Kind::OptionalChain:
      expr(cast<OptionalChainExpr>(e)->chain, use);
      return;
    case Expr::Kind::Member: {
      auto *member = cast<MemberExpr>(e);
      if (use != Use::Callee)
        nonCallReads_.insert(member->name);
      if (use == Use::Value)
        valueReads_.insert(member->name);
      // A method as a value holds its object.
      bool binds = member->method || (member->type && member->type->nonOptional()->isFunction() &&
                                      use == Use::Value);
      expr(member->object, binds ? Use::Value : Use::Receiver);
      return;
    }
    case Expr::Kind::Index: {
      auto *index = cast<IndexExpr>(e);
      // `o[k]` of a key of literals reads a field by its name.
      if (!index->field.empty())
        readAll(index->field);
      if (index->keyed && index->index->type) {
        const Type *key = index->index->type;
        for (const Type *n : key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key))
          if (n->isStringLiteral())
            readAll(utf8Of(n->literalText()));
      }
      expr(index->object, index->field.empty() && !index->keyed ? Use::Receiver : Use::Value);
      expr(index->index, Use::Value);
      return;
    }
    case Expr::Kind::Call: {
      auto *call = cast<CallExpr>(e);
      auto *member = dyn_cast<MemberExpr>(bare(call->callee));
      if (member && (call->method || call->builtin != Builtin::None || call->superCall ||
                     !call->unionMethods.empty())) {
        // A method's name: its object is the receiver.
        expr(member->object, Use::Receiver);
      } else {
        expr(call->callee, Use::Callee);
      }
      for (const Expr *arg : call->args)
        expr(arg, Use::Value);
      // What gives the values of every field.
      if ((call->builtin == Builtin::ObjectValues || call->builtin == Builtin::ObjectEntries) &&
          !call->args.empty())
        readFields(call->args[0]->type);
      return;
    }
    case Expr::Kind::Assign: {
      auto *assign = cast<AssignExpr>(e);
      expr(assign->value, Use::Value);
      if (auto *member = dyn_cast<MemberExpr>(assign->target)) {
        wrote(member, assign);
        expr(member->object, Use::Receiver);
        // A compound assignment reads the field too.
        if (assign->op)
          nonCallReads_.insert(member->name), valueReads_.insert(member->name);
      } else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
        if (!index->field.empty())
          bad_.insert(index->field);
        expr(index->object, Use::Receiver);
        expr(index->index, Use::Value);
      } else {
        expr(assign->target, Use::Value);
      }
      return;
    }
    case Expr::Kind::Update: {
      auto *update = cast<UpdateExpr>(e);
      if (auto *member = dyn_cast<MemberExpr>(update->target))
        expr(member->object, Use::Receiver);
      else
        expr(update->target, Use::Value);
      return;
    }
    case Expr::Kind::Function:
      function(cast<FunctionExpr>(e)->fn);
      return;
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(e);
      for (const ObjectLitExpr::Property &p : lit->props) {
        expr(p.key, Use::Value);
        if (!p.method)
          expr(p.value, Use::Value);
        // A spread copies the fields.
        if (p.spread && p.value)
          readFields(p.value->type);
      }
      if (lit->cls)
        for (const FunctionDecl *fn : lit->cls->methods)
          function(fn);
      return;
    }
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      for (const Expr *arg : create->ctor ? create->ctor->args : create->args)
        expr(arg, Use::Value);
      return;
    }
    default:
      break;
    }
    // The rest: what is in them is used as a value.
    visitChildren(e, [&](const Expr *child) { expr(child, Use::Value); });
  }

  void readAll(llvm::StringRef name) {
    valueReads_.insert(name);
    nonCallReads_.insert(name);
  }

  void readFields(const Type *type) {
    if (!type)
      return;
    type = type->nonOptional();
    if (type->isUnion()) {
      for (const Type *m : type->members())
        readFields(m);
      return;
    }
    if (type->isObject())
      for (const Field &f : type->fields())
        readAll(f.name);
  }

  // `x.f = v`: of the construction of a C when x is its `this` and v makes
  // a new object of a class; and `this.f.g = () => ... this ...`.
  void wrote(const MemberExpr *target, const AssignExpr *assign) {
    const FunctionDecl *fn = current_;
    auto *object = dyn_cast<NameExpr>(bare(target->object));
    bool self = fn && fn->thisVar && object && object->var == fn->thisVar;
    bool construction = self && fn->cls && (fn->isConstructor || fn == fn->cls->init);
    const ClassDecl *made = nullptr;
    if (auto *create = dyn_cast<NewExpr>(bare(assign->value));
        create && create->cls && construction) {
      bool holdsSelf = false;
      for (const Expr *arg : create->ctor ? create->ctor->args : create->args)
        holdsSelf = holdsSelf || mentions(arg, fn->thisVar);
      if (!holdsSelf)
        made = create->cls;
    }
    if (assign->op)
      made = nullptr;
    writes_[target->name].push_back({construction ? fn->cls : nullptr, made});
    // `this.f.g = function` of a C, the function capturing its `this`.
    auto *owned = dyn_cast<MemberExpr>(bare(target->object));
    auto *fe = dyn_cast<FunctionExpr>(bare(assign->value));
    // A generator or an async function goes on after its call returns, which
    // its owner may not see.
    if (!owned || !fe || fe->fn->nonEscaping || fe->fn->isAsync || fe->fn->isGenerator || !fn ||
        !fn->thisVar || !fn->cls)
      return;
    auto *holder = dyn_cast<NameExpr>(bare(owned->object));
    if (holder && holder->var == fn->thisVar && llvm::is_contained(fe->fn->captures, fn->thisVar))
      candidates_.push_back({fn->cls, owned->name, target->name, fe->fn, fn->thisVar});
  }

  static bool mentions(const Expr *e, const VarSymbol *self) {
    bool found = false;
    std::function<void(const Expr *)> visit = [&](const Expr *x) {
      if (!x || found)
        return;
      if (auto *name = dyn_cast<NameExpr>(x)) {
        found = name->var == self;
        return;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(x)) {
        found = llvm::is_contained(fe->fn->captures, self);
        return;
      }
      visitChildren(x, visit);
    };
    visit(e);
    return found;
  }

  static void visitChildren(const Expr *e, const std::function<void(const Expr *)> &visit) {
    switch (e->kind()) {
    case Expr::Kind::Template:
      for (const Expr *x : cast<TemplateExpr>(e)->exprs)
        visit(x);
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &x : cast<ArrayLitExpr>(e)->elements)
        visit(x.expr);
      break;
    case Expr::Kind::ObjectLit:
      for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
        visit(p.key);
        visit(p.value);
      }
      break;
    case Expr::Kind::Member:
      visit(cast<MemberExpr>(e)->object);
      break;
    case Expr::Kind::Index:
      visit(cast<IndexExpr>(e)->object);
      visit(cast<IndexExpr>(e)->index);
      break;
    case Expr::Kind::Unary:
      visit(cast<UnaryExpr>(e)->operand);
      break;
    case Expr::Kind::Binary:
      visit(cast<BinaryExpr>(e)->lhs);
      visit(cast<BinaryExpr>(e)->rhs);
      break;
    case Expr::Kind::Assign:
      visit(cast<AssignExpr>(e)->target);
      visit(cast<AssignExpr>(e)->value);
      break;
    case Expr::Kind::Update:
      visit(cast<UpdateExpr>(e)->target);
      break;
    case Expr::Kind::Call:
      visit(cast<CallExpr>(e)->callee);
      for (const Expr *x : cast<CallExpr>(e)->args)
        visit(x);
      break;
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      for (const Expr *x : create->ctor ? create->ctor->args : create->args)
        visit(x);
      break;
    }
    case Expr::Kind::Conditional:
      visit(cast<ConditionalExpr>(e)->cond);
      visit(cast<ConditionalExpr>(e)->thenExpr);
      visit(cast<ConditionalExpr>(e)->elseExpr);
      break;
    case Expr::Kind::Cast:
      visit(cast<CastExpr>(e)->operand);
      break;
    case Expr::Kind::Sequence:
      for (const Expr *x : cast<SequenceExpr>(e)->exprs)
        visit(x);
      break;
    case Expr::Kind::OptionalChain:
      visit(cast<OptionalChainExpr>(e)->chain);
      break;
    case Expr::Kind::Yield:
      visit(cast<YieldExpr>(e)->value);
      break;
    case Expr::Kind::Await:
      visit(cast<AwaitExpr>(e)->value);
      break;
    default:
      break;
    }
  }

  llvm::DenseSet<const FunctionDecl *> scanned_;
  const FunctionDecl *current_ = nullptr;
  llvm::StringSet<> valueReads_, nonCallReads_, bad_;
  llvm::StringMap<std::vector<Write>> writes_;
  std::vector<BackRef> candidates_;
};

// What a call through the method table of `fn` may run: fn and the methods
// that override it down the chain (see codegen_throw.cpp).
void forEachOverride(const FunctionDecl *fn, llvm::function_ref<void(const FunctionDecl *)> visit) {
  if (!fn->isAbstract)
    visit(fn);
  if (!fn->cls || fn->slot < 0)
    return;
  std::function<void(const ClassDecl *)> down = [&](const ClassDecl *cls) {
    for (const ClassDecl *child : cls->children) {
      if (size_t(fn->slot) < child->vtable.size())
        if (const FunctionDecl *impl = child->vtable[size_t(fn->slot)]; impl && impl != fn)
          visit(impl);
      down(child);
    }
  };
  down(fn->cls);
}

// Layer 1 of docs/objects.md, section 11: the objects that are never stored
// anywhere a reference can be kept from (a field, an element, an entry, an
// environment, a box), only in variables and arguments, cannot be on a
// cycle. They are counted as any other object, and a reference to a member
// stored in them does not join groups: the arrays a search keeps its work
// in do not stay in the group of the graph it searches.
//
// The analysis follows values through variables, arguments, `this` and
// results, over the whole program: a node of its graph is a place a value
// may be (a variable, the result of a function, an allocation), and an edge
// says that if what is at its start is stored, so is what is at its end.
// Whatever reaches a place that stores is stored; an allocation nothing
// stored reaches is not.
class StoreScanner {
public:
  // `site` tells the allocations that matter: those of types that may form
  // cycles.
  explicit StoreScanner(std::function<bool(const Expr *)> site) : isSite_(std::move(site)) {
    node(Key{Kind::Stored, nullptr}); // node 0
  }

  void function(const FunctionDecl *fn) {
    if (!fn || !scanned_.insert(fn).second)
      return;
    const FunctionDecl *saved = current_;
    current_ = fn;
    for (const Param &param : fn->params)
      if (param.defaultValue)
        flowInto(var(param.sym), expr(param.defaultValue));
    // A rest parameter is an array the arguments go into.
    if (fn->sym && fn->sym->hasRest && !fn->params.empty())
      flowInto(var(fn->params.back().sym), {});
    stmt(fn->body);
    current_ = saved;
  }

  // Top-level code: no function, no result.
  void topLevel(const Stmt *s) {
    current_ = nullptr;
    stmt(s);
  }

  // A function used as a value: whoever calls it may keep what it returns.
  void escapes(const FunctionDecl *fn) {
    if (fn)
      edge(stored(), result(fn));
  }

  // The allocations nothing stored reaches.
  llvm::DenseSet<const Expr *> unstored() {
    std::vector<bool> reached(edges_.size(), false);
    std::vector<unsigned> work{0};
    reached[0] = true;
    while (!work.empty()) {
      unsigned n = work.back();
      work.pop_back();
      for (unsigned m : edges_[n])
        if (!reached[m]) {
          reached[m] = true;
          work.push_back(m);
        }
    }
    llvm::DenseSet<const Expr *> result;
    for (const auto &[key, n] : index_)
      if (key.kind == Kind::Site && !reached[n])
        result.insert(static_cast<const Expr *>(key.ptr));
    return result;
  }

private:
  enum class Kind { Stored, Var, Result, Site };
  struct Key {
    Kind kind;
    const void *ptr;
    bool operator<(const Key &o) const { return kind != o.kind ? kind < o.kind : ptr < o.ptr; }
  };
  using Sources = std::vector<unsigned>;

  unsigned node(Key key) {
    auto [it, inserted] = index_.try_emplace(key, unsigned(edges_.size()));
    if (inserted)
      edges_.emplace_back();
    return it->second;
  }
  unsigned stored() { return 0; }
  unsigned var(const VarSymbol *v) {
    unsigned n = node({Kind::Var, v});
    // A variable that lives in a box holds its value on the heap.
    if (v && v->boxed())
      edge(stored(), n);
    return n;
  }
  unsigned result(const FunctionDecl *fn) { return node({Kind::Result, fn}); }
  void edge(unsigned from, unsigned to) {
    if (from != to)
      edges_[from].push_back(to);
  }
  void flowInto(unsigned place, const Sources &values) {
    for (unsigned v : values)
      edge(place, v);
  }
  void store(const Sources &values) { flowInto(stored(), values); }

  void stmt(const Stmt *stmt) {
    if (!stmt)
      return;
    switch (stmt->kind()) {
    case Stmt::Kind::Block: {
      auto *block = cast<BlockStmt>(stmt);
      // The functions of a block share an environment, which holds what
      // they capture.
      if (NestedGroup *group = block->group)
        for (const VarSymbol *v : group->captures)
          edge(stored(), var(v));
      for (const Stmt *s : block->body)
        this->stmt(s);
      break;
    }
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
        if (d.sym)
          flowInto(var(d.sym), expr(d.init));
        else
          expr(d.init);
      break;
    case Stmt::Kind::Function: {
      auto *fn = cast<FunctionDecl>(stmt);
      function(fn);
      break;
    }
    case Stmt::Kind::Expr:
      expr(cast<ExprStmt>(stmt)->expr);
      break;
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      expr(s->cond);
      this->stmt(s->thenStmt);
      this->stmt(s->elseStmt);
      break;
    }
    case Stmt::Kind::While:
      expr(cast<WhileStmt>(stmt)->cond);
      this->stmt(cast<WhileStmt>(stmt)->body);
      break;
    case Stmt::Kind::DoWhile:
      this->stmt(cast<DoWhileStmt>(stmt)->body);
      expr(cast<DoWhileStmt>(stmt)->cond);
      break;
    case Stmt::Kind::For: {
      auto *s = cast<ForStmt>(stmt);
      this->stmt(s->init);
      expr(s->cond);
      expr(s->step);
      this->stmt(s->body);
      break;
    }
    case Stmt::Kind::ForOf: {
      auto *s = cast<ForOfStmt>(stmt);
      // The loop reads the elements; an iterator it walks may hold it.
      Sources iterable = expr(s->iterable);
      if (s->iterable->type && s->iterable->type->isIterator())
        store(iterable);
      if (s->binding.sym)
        var(s->binding.sym);
      this->stmt(s->body);
      break;
    }
    case Stmt::Kind::Switch: {
      auto *s = cast<SwitchStmt>(stmt);
      expr(s->discriminant);
      for (const SwitchCase &c : s->cases) {
        expr(c.test);
        for (const Stmt *body : c.body)
          this->stmt(body);
      }
      break;
    }
    case Stmt::Kind::Labeled:
      this->stmt(cast<LabeledStmt>(stmt)->body);
      break;
    case Stmt::Kind::Return: {
      Sources value = expr(cast<ReturnStmt>(stmt)->value);
      // A generator's or an async function's result is held by its object.
      if (!current_ || current_->isGenerator || current_->isAsync)
        store(value);
      else
        flowInto(result(current_), value);
      break;
    }
    case Stmt::Kind::Throw:
      store(expr(cast<ThrowStmt>(stmt)->value));
      break;
    case Stmt::Kind::Try: {
      auto *s = cast<TryStmt>(stmt);
      this->stmt(s->block);
      if (s->param.sym)
        var(s->param.sym);
      this->stmt(s->handler);
      this->stmt(s->finalizer);
      break;
    }
    case Stmt::Kind::Class: {
      auto *cls = cast<ClassDecl>(stmt);
      for (const ClassField &f : cls->fields)
        if (f.isStatic && f.var)
          flowInto(var(f.var), expr(f.init));
      for (const Stmt *s : cls->decorations)
        this->stmt(s);
      break;
    }
    default:
      break;
    }
  }

  // The places the value of `e` may come from.
  Sources expr(const Expr *e) {
    if (!e)
      return {};
    Sources out;
    if (isSite_(e))
      out.push_back(node({Kind::Site, e}));
    switch (e->kind()) {
    case Expr::Kind::Name: {
      auto *name = cast<NameExpr>(e);
      if (name->var) {
        out.push_back(var(name->var));
        // The variable of a function, as a value.
        if (name->var->function && !callee_.contains(e))
          escapes(name->var->function);
        if (name->var->lambda && !callee_.contains(e))
          escapes(name->var->lambda);
      }
      if (name->func && !callee_.contains(e))
        escapes(name->func->decl);
      break;
    }
    case Expr::Kind::Template:
      for (const Expr *x : cast<TemplateExpr>(e)->exprs)
        expr(x);
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &x : cast<ArrayLitExpr>(e)->elements) {
        Sources element = expr(x.expr);
        // A spread copies the elements, not the array.
        if (!x.spread || (x.expr->type && x.expr->type->isIterator()))
          store(element);
      }
      break;
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(e);
      for (const ObjectLitExpr::Property &p : lit->props) {
        expr(p.key);
        if (p.method) {
          if (auto *fe = dyn_cast<FunctionExpr>(p.value))
            function(fe->fn);
          continue;
        }
        Sources value = expr(p.value);
        // A spread copies the fields; a method of an interface it copies is
        // bound to the object (see emitSpreadLiteral).
        bool binds = p.spread && p.value->type && p.value->type->isObject() &&
                     llvm::any_of(p.value->type->fields(),
                                  [](const Field &f) { return f.type->isFunction(); });
        if (!p.spread || binds)
          store(value);
      }
      // A literal's methods get the object as `this`, and it holds what
      // they capture.
      if (lit->cls) {
        for (const FunctionDecl *fn : lit->cls->methods) {
          function(fn);
          if (fn->thisVar)
            flowInto(var(fn->thisVar), out);
        }
        if (lit->cls->group)
          for (const VarSymbol *v : lit->cls->group->captures)
            edge(stored(), var(v));
      }
      break;
    }
    case Expr::Kind::Member: {
      auto *member = cast<MemberExpr>(e);
      Sources object = expr(member->object);
      // A method as a value holds its object.
      if (member->method && !callee_.contains(e)) {
        store(object);
        escapes(member->method->decl);
      }
      if (member->type && member->type->nonOptional()->isFunction() && !callee_.contains(e))
        store(object);
      out.clear(); // a field read: anything
      if (isSite_(e))
        out.push_back(node({Kind::Site, e}));
      break;
    }
    case Expr::Kind::Index:
      expr(cast<IndexExpr>(e)->object);
      expr(cast<IndexExpr>(e)->index);
      break;
    case Expr::Kind::Unary: {
      auto *unary = cast<UnaryExpr>(e);
      Sources operand = expr(unary->operand);
      if (unary->op == UnaryOp::NonNull)
        out.insert(out.end(), operand.begin(), operand.end());
      break;
    }
    case Expr::Kind::Binary: {
      auto *bin = cast<BinaryExpr>(e);
      Sources lhs = expr(bin->lhs);
      Sources rhs = bin->op == BinaryOp::InstanceOf ? Sources{} : expr(bin->rhs);
      if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr ||
          bin->op == BinaryOp::Nullish) {
        out.insert(out.end(), lhs.begin(), lhs.end());
        out.insert(out.end(), rhs.begin(), rhs.end());
      }
      break;
    }
    case Expr::Kind::Assign: {
      auto *assign = cast<AssignExpr>(e);
      Sources value = expr(assign->value);
      if (auto *name = dyn_cast<NameExpr>(assign->target); name && name->var) {
        flowInto(var(name->var), value);
        out.push_back(var(name->var));
      } else {
        // A field, an element or an entry: stored.
        if (auto *member = dyn_cast<MemberExpr>(assign->target))
          expr(member->object);
        else if (auto *index = dyn_cast<IndexExpr>(assign->target)) {
          expr(index->object);
          expr(index->index);
        }
        store(value);
        out.insert(out.end(), value.begin(), value.end());
      }
      break;
    }
    case Expr::Kind::Update:
      expr(cast<UpdateExpr>(e)->target);
      break;
    case Expr::Kind::Call: {
      Sources result = call(cast<CallExpr>(e));
      out.insert(out.end(), result.begin(), result.end());
      break;
    }
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      std::vector<Sources> args;
      for (const Expr *arg : create->ctor ? create->ctor->args : create->args)
        args.push_back(expr(arg));
      if (const ClassDecl *cls = create->cls) {
        // The constructor that runs gets the arguments, and it, the
        // initializers and those up the chain get the object as `this`.
        const FunctionDecl *ctor =
            create->ctor && create->ctor->func ? create->ctor->func->decl : nullptr;
        bindArgs(ctor, args);
        for (const ClassDecl *c = cls; c; c = c->parent) {
          if (c->init && c->init->thisVar)
            flowInto(var(c->init->thisVar), out);
          for (const FunctionDecl *k : c->constructors)
            if (k->thisVar)
              flowInto(var(k->thisVar), out);
        }
      } else {
        for (const Sources &a : args)
          store(a);
      }
      break;
    }
    case Expr::Kind::Conditional: {
      auto *cond = cast<ConditionalExpr>(e);
      expr(cond->cond);
      Sources a = expr(cond->thenExpr), b = expr(cond->elseExpr);
      out.insert(out.end(), a.begin(), a.end());
      out.insert(out.end(), b.begin(), b.end());
      break;
    }
    case Expr::Kind::Cast: {
      auto *conv = cast<CastExpr>(e);
      Sources operand = expr(conv->operand);
      // An object as an unknown, or as the object of generic code: kept as
      // it may be.
      if (conv->type && (conv->type->nonOptional()->isUnknown() ||
                         conv->type->nonOptional()->isAnyObject()))
        store(operand);
      else
        out.insert(out.end(), operand.begin(), operand.end());
      break;
    }
    case Expr::Kind::Sequence: {
      Sources last;
      for (const Expr *x : cast<SequenceExpr>(e)->exprs)
        last = expr(x);
      out.insert(out.end(), last.begin(), last.end());
      break;
    }
    case Expr::Kind::OptionalChain: {
      Sources chain = expr(cast<OptionalChainExpr>(e)->chain);
      out.insert(out.end(), chain.begin(), chain.end());
      break;
    }
    case Expr::Kind::Function: {
      const FunctionDecl *fn = cast<FunctionExpr>(e)->fn;
      // An environment on the heap holds what it captures.
      if (!fn->nonEscaping) {
        const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
        for (const VarSymbol *v : captures)
          edge(stored(), var(v));
        if (!callee_.contains(e))
          escapes(fn);
      }
      function(fn);
      break;
    }
    case Expr::Kind::Yield:
      store(expr(cast<YieldExpr>(e)->value));
      break;
    case Expr::Kind::Await: {
      Sources value = expr(cast<AwaitExpr>(e)->value);
      store(value);
      break;
    }
    default:
      break;
    }
    return out;
  }

  // The arguments of a call of `fn` (null: of code not known) go to its
  // parameters.
  void bindArgs(const FunctionDecl *fn, const std::vector<Sources> &args) {
    if (!fn || fn->impl) {
      for (const Sources &a : args)
        store(a);
      return;
    }
    for (size_t i = 0; i < args.size(); ++i) {
      bool rest = fn->sym && fn->sym->hasRest && i + 1 >= fn->params.size();
      if (i < fn->params.size() && !rest && fn->params[i].sym)
        flowInto(var(fn->params[i].sym), args[i]);
      else
        store(args[i]);
    }
  }

  Sources call(const CallExpr *c) {
    callee_.insert(c->callee);
    auto *member = dyn_cast<MemberExpr>(c->callee);
    Sources receiver;
    if (member)
      receiver = expr(member->object);
    else if (c->builtin == Builtin::None && !c->func)
      expr(c->callee);
    std::vector<Sources> args;
    for (const Expr *arg : c->args)
      args.push_back(expr(arg));
    for (size_t i = 0; i < c->args.size(); ++i)
      if (c->spreads(i))
        store(args[i]);
    Sources out;
    if (c->builtin != Builtin::None)
      return builtin(c, receiver, args);
    auto methodCall = [&](const FunctionDecl *fn, bool virtualCall) {
      auto one = [&](const FunctionDecl *m) {
        bindArgs(m, args);
        if (m->thisVar)
          flowInto(var(m->thisVar), receiver);
        else
          store(receiver);
        out.push_back(result(m));
      };
      if (fn->impl) { // what a decorator made of it runs instead
        store(receiver);
        bindArgs(nullptr, args);
        return;
      }
      if (virtualCall)
        forEachOverride(fn, one);
      else
        one(fn);
    };
    if (c->superCall && !member) {
      // `super(args)`: the constructor of the class extended, and the
      // initializers, with this object.
      const FunctionDecl *ctor = c->method ? c->method->decl : nullptr;
      bindArgs(ctor, args);
      Sources self = current_ && current_->thisVar ? Sources{var(current_->thisVar)} : Sources{};
      for (const ClassDecl *cls = current_ && current_->cls ? current_->cls->parent : nullptr; cls;
           cls = cls->parent) {
        if (cls->init && cls->init->thisVar)
          flowInto(var(cls->init->thisVar), self);
        for (const FunctionDecl *k : cls->constructors)
          if (k->thisVar)
            flowInto(var(k->thisVar), self);
      }
      if (current_ && current_->cls && current_->cls->init && current_->cls->init->thisVar)
        flowInto(var(current_->cls->init->thisVar), self);
      return out;
    }
    if (c->method) {
      methodCall(c->method->decl, c->virtualCall);
      return out;
    }
    if (!c->unionMethods.empty()) {
      for (const CallExpr::UnionMethod &m : c->unionMethods)
        if (m.method)
          methodCall(m.method->decl, m.virtualCall);
        else
          bindArgs(nullptr, args);
      return out;
    }
    if (c->func) {
      bindArgs(c->func->decl, args);
      out.push_back(result(c->func->decl));
      return out;
    }
    // Through a function value, or a method of an interface: code not known.
    store(receiver);
    bindArgs(nullptr, args);
    return out;
  }

  // The built-ins that keep none of their operands: the others may.
  Sources builtin(const CallExpr *c, const Sources &receiver, const std::vector<Sources> &args) {
    Sources out;
    if (isSite_(c))
      out.push_back(node({Kind::Site, c}));
    // A callback that gets the receiver: an arrow written there gets it in
    // its third parameter (forEach of an array or a map...); one known only
    // as a value may keep it.
    for (const Expr *arg : c->args) {
      if (!arg->type || !arg->type->nonOptional()->isFunction())
        continue;
      auto *fe = dyn_cast<FunctionExpr>(arg);
      if (!fe)
        store(receiver);
      else if (fe->fn->params.size() >= 3 && fe->fn->params[2].sym)
        flowInto(var(fe->fn->params[2].sym), receiver);
    }
    switch (c->builtin) {
    case Builtin::ArrReverse:
    case Builtin::ArrSort:
    case Builtin::ArrSortWith:
      // The array itself is the result.
      out.insert(out.end(), receiver.begin(), receiver.end());
      break;
    case Builtin::ConsoleLog:
    case Builtin::ConsoleError:
    case Builtin::StringOf:
    case Builtin::ArrPop:
    case Builtin::ArrShift:
    case Builtin::ArrAt:
    case Builtin::ArrSlice:
    case Builtin::ArrIndexOf:
    case Builtin::ArrLastIndexOf:
    case Builtin::ArrIncludes:
    case Builtin::ArrJoin:
    case Builtin::ArrToString:
    case Builtin::ArrForEach:
    case Builtin::ArrFilter:
    case Builtin::ArrFind:
    case Builtin::ArrFindIndex:
    case Builtin::ArrFindLast:
    case Builtin::ArrFindLastIndex:
    case Builtin::ArrSome:
    case Builtin::ArrEvery:
    case Builtin::ObjectKeys:
    case Builtin::ObjectValues:
    case Builtin::ObjectEntries:
    case Builtin::MapGet:
    case Builtin::MapHas:
    case Builtin::MapDelete:
    case Builtin::MapClear:
    case Builtin::MapSize:
    case Builtin::MapForEach:
    case Builtin::JsonStringify:
    case Builtin::ArrayIsArray:
    case Builtin::WeakRefNew:
    case Builtin::WeakRefDeref:
      break;
    case Builtin::ArrPush:
    case Builtin::ArrUnshift:
    case Builtin::ArrSplice:
    case Builtin::ArrFill:
    case Builtin::ArrConcat:
    case Builtin::MapSet:
    case Builtin::SetAdd:
    case Builtin::RecordSet:
    case Builtin::RecordNullishSet:
      // The values go into the receiver (or a new array), which they do
      // not make stored; set() and add() give the receiver back.
      for (const Sources &a : args)
        store(a);
      if (c->builtin == Builtin::MapSet || c->builtin == Builtin::SetAdd)
        out.insert(out.end(), receiver.begin(), receiver.end());
      break;
    case Builtin::ArrMap:
    case Builtin::ArrFlatMap:
      // What the callback gives goes into the new array.
      for (const Expr *arg : c->args)
        if (auto *fe = dyn_cast<FunctionExpr>(arg))
          edge(stored(), result(fe->fn));
      break;
    default:
      store(receiver);
      for (const Sources &a : args)
        store(a);
      break;
    }
    return out;
  }

  std::function<bool(const Expr *)> isSite_;
  std::map<Key, unsigned> index_;
  std::vector<std::vector<unsigned>> edges_;
  llvm::DenseSet<const FunctionDecl *> scanned_;
  llvm::DenseSet<const Expr *> callee_;
  const FunctionDecl *current_ = nullptr;
};

} // namespace

void CodeGen::analyzeCycles(SourceFile &file) {
  CycleScanner scan;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn))
        scan.function(fn);
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods)
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted()))
          scan.function(fn);
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances)
          scan.function(fn);
        for (const FunctionDecl *fn : cls->constructors)
          scan.function(fn);
        scan.function(cls->init);
        for (const ClassField &f : cls->fields)
          scan.function(f.method);
        scan.classDecl(cls);
      }
      continue;
    }
    scan.stmt(item);
  }
  for (const FunctionDecl *fn : file.instances)
    scan.function(fn);
  for (const Type *type : subtypes_)
    scan.addType(type);
  for (const auto &[from, to] : file.views) {
    scan.addType(from);
    scan.addType(to);
  }

  // Layer 2: the functions in owned objects whose references to their
  // owners need no count.
  OwnerScanner owners;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn))
        owners.function(fn);
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods)
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted()))
          owners.function(fn);
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances)
          owners.function(fn);
        for (const FunctionDecl *fn : cls->constructors)
          owners.function(fn);
        owners.function(cls->init);
        for (const ClassField &f : cls->fields)
          owners.function(f.method);
      }
    }
    owners.topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances)
    owners.function(fn);
  // A class whose construction or methods let `this` go somewhere.
  auto escapes = [](const ClassDecl *cls) {
    for (const FunctionDecl *fn : cls->constructors)
      if (fn->thisVar && CycleScanner::thisEscapes(fn->body, fn->thisVar))
        return true;
    if (cls->init && cls->init->thisVar && CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar))
      return true;
    for (const FunctionDecl *fn : cls->methods)
      if (!fn->isStatic && fn->thisVar && fn->body && CycleScanner::thisEscapes(fn->body, fn->thisVar))
        return true;
    for (const ClassField &f : cls->fields)
      if (f.method && f.method->thisVar && CycleScanner::thisEscapes(f.method->body, f.method->thisVar))
        return true;
    return false;
  };
  std::vector<OwnerScanner::BackRef> backRefs = owners.proven(escapes);

  // The object types each type of reference may point at: itself, the
  // types declared to be it, and those code sees as it (views).
  std::vector<const Type *> objects, dicts;
  for (const Type *t : scan.types) {
    if (t->isObject() && !t->isAnyObject())
      objects.push_back(t);
    if (t->isDict())
      dicts.push_back(t);
  }
  std::map<const Type *, std::vector<const Type *>> runtimeTypes;
  auto runtimeTypesOf = [&](const Type *type) -> const std::vector<const Type *> & {
    auto [it, inserted] = runtimeTypes.try_emplace(type);
    if (inserted) {
      for (const Type *s : objects)
        if (s->isSubtypeOf(type))
          it->second.push_back(s);
      for (const auto &[from, to] : file.views)
        if (to == type && !llvm::is_contained(it->second, from))
          it->second.push_back(from);
    }
    return it->second;
  };

  // An array or dictionary type is added to when it, or how generic code
  // sees it, is.
  auto containerRepresentation = [](const Type *type) -> const Type * {
    if (type->isArray())
      return Type::array(representationOf(type->inner()));
    if (type->isDict())
      return Type::dict(type->dictKind(), representationOf(type->keyType()),
                        type->isSet() ? type->valueType() : representationOf(type->valueType()));
    return type;
  };
  auto isMutated = [&](const Type *type) {
    return scan.mutated.contains(type) || scan.mutated.contains(containerRepresentation(type));
  };

  // The graph, without the references layer 2 proved need no count; an
  // owned object that is cyclic all the same may outlive its owner through
  // its group, so its proof is dropped and the graph made again.
  for (;;) {
  uncounted_.clear();
  for (const OwnerScanner::BackRef &b : backRefs)
    uncounted_.insert({b.fn, b.self});
  cyclicTypes_.clear();
  cyclicEnvs_.clear();
  cyclicBoxes_.clear();
  cyclicArrayReps_.clear();
  refMember_.clear();
  hasCycles_ = anyCyclicObject_ = fnMayBeMember_ = false;
  CycleGraph graph;
  using Kind = CycleGraph::Kind;
  auto fnNodeOf = [&](const Type *type) { return graph.node(Kind::FnNode, type->nonOptional()); };
  const Type *anyFunction = representationOf(Type::function({}, 0, false, Type::voidTy()));
  const unsigned anyNode = graph.node(Kind::AnyObjectNode, nullptr);
  const unsigned unknownNode = graph.node(Kind::UnknownNode, nullptr);
  // The nodes a value of `type` may refer to.
  std::function<void(const Type *, std::vector<unsigned> &)> refers =
      [&](const Type *type, std::vector<unsigned> &out) {
        switch (type->kind()) {
        case Type::Kind::Optional:
          refers(type->inner(), out);
          break;
        case Type::Kind::Union:
          for (const Type *m : type->members())
            refers(m, out);
          break;
        case Type::Kind::Result:
          refers(type->okType(), out);
          refers(type->errType(), out);
          break;
        case Type::Kind::Object:
          if (type->isAnyObject())
            out.push_back(anyNode);
          else if (!type->isWeakRef())
            for (const Type *s : runtimeTypesOf(type))
              out.push_back(graph.node(Kind::TypeNode, s));
          break;
        case Type::Kind::Array:
        case Type::Kind::Dict:
          out.push_back(graph.node(Kind::TypeNode, type));
          break;
        case Type::Kind::Function:
          out.push_back(fnNodeOf(type));
          break;
        case Type::Kind::Unknown:
          out.push_back(unknownNode);
          break;
        case Type::Kind::Promise:
          // A promise may be a member (promiseIsMember): it holds its value
          // or its error, and an async function's frame whatever that
          // holds, so it may hold anything.
          out.push_back(anyNode);
          out.push_back(unknownNode);
          out.push_back(fnNodeOf(anyFunction));
          break;
        default:
          // Iterators and timers are never members: a cycle through what
          // they hold is not freed (see README).
          break;
        }
      };
  auto edges = [&](unsigned from, const Type *type, bool late) {
    std::vector<unsigned> to;
    refers(type, to);
    for (unsigned n : to)
      graph.edge(from, n, late);
  };
  auto captureEdges = [&](unsigned from, const VarSymbol *var) {
    if (var->boxed())
      graph.edge(from, graph.node(Kind::BoxNode, typeOf(var)), false);
    else
      edges(from, typeOf(var), false);
  };
  for (const Type *t : scan.types) {
    if (t->isObject() && !t->isAnyObject() && !t->isWeakRef()) {
      unsigned n = graph.node(Kind::TypeNode, t);
      for (const Field &f : t->fields())
        edges(n, f.type, scan.mutableFields.count(f.name) != 0);
      // What the methods of a literal capture lies in the object.
      if (const ClassDecl *cls = t->classDecl(); cls && cls->group)
        for (const VarSymbol *var : cls->group->captures)
          captureEdges(n, var);
    } else if (t->isArray()) {
      edges(graph.node(Kind::TypeNode, t), t->inner(), isMutated(t));
    } else if (t->isDict()) {
      unsigned n = graph.node(Kind::TypeNode, t);
      edges(n, t->keyType(), isMutated(t));
      if (!t->isSet())
        edges(n, t->valueType(), isMutated(t));
    }
  }
  for (const auto &[key, captures] : scan.envs) {
    unsigned n = graph.node(Kind::EnvNode, key);
    for (const VarSymbol *var : captures)
      if (!uncounted_.contains({key, var}))
        captureEdges(n, var);
  }
  // What a value of a function type may hold: the environments of the
  // closures of that type, the objects of the methods taken as values of
  // it, and whatever a value of a type converted to it holds. Generic code
  // sees every function as one type.
  for (const auto &[type, keys] : scan.closuresOf)
    for (const void *key : keys) {
      unsigned fn = fnNodeOf(type), env = graph.node(Kind::EnvNode, key);
      graph.edge(fn, env, false);
    }
  for (const auto &[from, to] : scan.fnCasts) {
    unsigned a = fnNodeOf(to), b = fnNodeOf(from);
    graph.edge(a, b, false);
  }
  for (const VarSymbol *var : scan.boxed) {
    unsigned n = graph.node(Kind::BoxNode, typeOf(var));
    edges(n, typeOf(var), true);
  }
  for (const auto &[fnType, holders] : scan.boundOf)
    for (const Type *type : holders) {
      std::vector<unsigned> to;
      refers(type, to);
      unsigned fn = fnNodeOf(fnType);
      for (unsigned n : to)
        graph.edge(fn, n, false);
    }
  {
    unsigned any = fnNodeOf(anyFunction);
    for (unsigned v = 0, end = unsigned(graph.nodes.size()); v < end; ++v)
      if (graph.nodes[v].kind == Kind::FnNode && v != any)
        graph.edge(any, v, false);
  }
  for (const Type *t : objects)
    if (!t->isWeakRef()) {
      unsigned n = graph.node(Kind::TypeNode, t);
      graph.edge(anyNode, n, false);
    }
  for (const Type *t : dicts) {
    unsigned n = graph.node(Kind::TypeNode, t);
    graph.edge(anyNode, n, false);
  }
  // An object in an unknown is the reference itself; a function is in a box,
  // which is a member when what it holds may be one (CodeGen::boxValue): a
  // listener in the Record of an emitter of node:events that captures the
  // emitter is a cycle through it. Any other value is in a box no group has.
  for (const Type *type : scan.toUnknown) {
    std::vector<unsigned> to;
    refers(type, to);
    for (unsigned n : to) {
      const CycleGraph::Node &node = graph.nodes[n];
      auto *t = static_cast<const Type *>(node.key);
      if ((node.kind == Kind::TypeNode && (t->isObject() || t->isDict())) || n == anyNode ||
          node.kind == Kind::FnNode)
        graph.edge(unknownNode, n, false);
    }
  }

  // The strongly connected components (Tarjan); one is cyclic if an edge
  // inside it may be set late.
  size_t count = graph.nodes.size();
  std::vector<int> order(count, -1), low(count, 0), component(count, -1);
  std::vector<bool> onStack(count, false);
  std::vector<unsigned> stack;
  int next = 0, components = 0;
  std::function<void(unsigned)> visit = [&](unsigned v) {
    order[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (auto [w, late] : graph.nodes[v].edges) {
      if (order[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], order[w]);
      }
    }
    if (low[v] != order[v])
      return;
    unsigned w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      component[w] = components;
    } while (w != v);
    ++components;
  };
  for (unsigned v = 0; v < count; ++v)
    if (order[v] < 0)
      visit(v);
  std::vector<bool> cyclicComponent(size_t(components), false);
  for (unsigned v = 0; v < count; ++v)
    for (auto [w, late] : graph.nodes[v].edges)
      if (late && component[v] == component[w])
        cyclicComponent[size_t(component[v])] = true;

  for (unsigned v = 0; v < count; ++v) {
    if (!cyclicComponent[size_t(component[v])])
      continue;
    const CycleGraph::Node &node = graph.nodes[v];
    switch (node.kind) {
    case Kind::TypeNode: {
      auto *type = static_cast<const Type *>(node.key);
      cyclicTypes_.insert(type);
      if (type->isObject() || type->isDict())
        anyCyclicObject_ = true;
      if (type->isArray())
        cyclicArrayReps_.insert(representationOf(type));
      hasCycles_ = true;
      break;
    }
    case Kind::EnvNode:
      cyclicEnvs_.insert(node.key);
      hasCycles_ = true;
      break;
    case Kind::BoxNode:
      cyclicBoxes_.insert(static_cast<const Type *>(node.key));
      hasCycles_ = true;
      break;
    default:
      break;
    }
  }
  // The function types whose values may hold a member: the environment of
  // a cyclic closure, or a cyclic object whose method it is, through the
  // types converted to them too.
  for (unsigned v = 0; v < count; ++v) {
    if (graph.nodes[v].kind != Kind::FnNode)
      continue;
    std::vector<unsigned> work{v};
    llvm::DenseSet<unsigned> seen{v};
    bool member = false;
    while (!work.empty() && !member) {
      unsigned n = work.back();
      work.pop_back();
      for (auto [w, late] : graph.nodes[n].edges) {
        const CycleGraph::Node &node = graph.nodes[w];
        if (node.kind == Kind::FnNode) {
          if (seen.insert(w).second)
            work.push_back(w);
        } else if ((node.kind == Kind::EnvNode && cyclicEnvs_.contains(node.key)) ||
                   (node.kind == Kind::TypeNode &&
                    cyclicTypes_.contains(static_cast<const Type *>(node.key)))) {
          member = true;
        }
      }
    }
    auto *type = static_cast<const Type *>(graph.nodes[v].key);
    fnMayBeMember_ = fnMayBeMember_ || member;
    // `resolve` of `new Promise` has its promise as its environment, which
    // may be a member in a program with cycles (promiseIsMember).
    refMember_[type] = member || hasCycles_;
  }
  // What may hold a member, and may be held by one, is one too: an object,
  // array, dictionary, environment or box outside the groups that a member
  // held, and that held a reference into a group, would count from outside
  // it a reference that the group may own through it (an X that holds a T
  // both in a field and in an array of them), and the group could then
  // never be freed though nothing outside reaches it. One that no member
  // holds stays outside: what it holds is held from outside indeed, and the
  // groups stay as small as the cycles make them.
  if (hasCycles_) {
    auto nodeOf = [&](Kind kind, const void *key) -> int {
      auto it = graph.index.find({int(kind), key});
      return it == graph.index.end() ? -1 : int(it->second);
    };
    auto isMemberNode = [&](unsigned v) {
      const CycleGraph::Node &node = graph.nodes[v];
      switch (node.kind) {
      case Kind::TypeNode:
        return cyclicTypes_.contains(static_cast<const Type *>(node.key));
      case Kind::EnvNode:
        return cyclicEnvs_.contains(node.key);
      case Kind::BoxNode:
        return cyclicBoxes_.contains(static_cast<const Type *>(node.key));
      default:
        return false;
      }
    };
    // A function value, an object of any type or an unknown is the reference
    // to what it holds: what holds it holds that.
    auto passesOn = [&](unsigned v) {
      Kind kind = graph.nodes[v].kind;
      return kind == Kind::FnNode || kind == Kind::AnyObjectNode || kind == Kind::UnknownNode;
    };
    // What the promises of each value hold, found before the nodes are
    // counted (refers may make nodes).
    std::vector<unsigned> callbacks;
    for (const Type *t : scan.reactionCallbacks)
      callbacks.push_back(fnNodeOf(t));
    std::vector<std::pair<const Type *, std::vector<unsigned>>> promised;
    for (const Type *t : scan.types)
      if (t->isPromise()) {
        promised.push_back({t->promised(), {}});
        refers(t->promised(), promised.back().second);
      }
    std::vector<bool> held;
    auto findHeld = [&] {
      held.assign(graph.nodes.size(), false);
      std::vector<unsigned> work;
      std::vector<bool> seen(graph.nodes.size(), false);
      auto reach = [&](unsigned v) {
        held[v] = true;
        if (passesOn(v) && !seen[v]) {
          seen[v] = true;
          work.push_back(v);
        }
      };
      for (unsigned v = 0; v < graph.nodes.size(); ++v)
        if (isMemberNode(v)) {
          seen[v] = true;
          work.push_back(v);
        }
      // A reaction to a promise is a member that holds its callbacks
      // (attachReaction), and a promise one that holds its value.
      for (unsigned n : callbacks)
        reach(n);
      for (const auto &[value, to] : promised)
        if (promiseIsMember(value))
          for (unsigned n : to)
            reach(n);
      while (!work.empty()) {
        unsigned v = work.back();
        work.pop_back();
        for (auto [w, late] : graph.nodes[v].edges)
          reach(w);
      }
    };
    auto isHeld = [&](Kind kind, const void *key) {
      int v = nodeOf(kind, key);
      return v >= 0 && held[unsigned(v)];
    };
    for (bool changed = true; changed;) {
      changed = false;
      findHeld();
      auto add = [&](llvm::DenseSet<const Type *> &set, const Type *t) {
        if (set.insert(t).second) {
          refMember_.clear();
          changed = true;
        }
      };
      for (const Type *t : scan.types) {
        if (cyclicTypes_.contains(t) || t->isWeakRef() || t->isAnyObject() || t->isRuntimeObject() ||
            t->isBytes() || t->isCrypto() || t->isIntl() || t->isDate() || t->isRegExp() ||
            t->isMatch() || t->isTimer())
          continue;
        if (!(t->isObject() || t->isArray() || t->isDict()) || !isHeld(Kind::TypeNode, t))
          continue;
        bool holds = false;
        if (t->isObject()) {
          for (const Field &f : t->fields())
            holds = holds || mayHoldMember(f.type);
        } else if (t->isArray()) {
          holds = mayHoldMember(t->inner());
        } else {
          holds = mayHoldMember(t->keyType()) || (!t->isSet() && mayHoldMember(t->valueType()));
        }
        if (!holds)
          continue;
        add(cyclicTypes_, t);
        if (t->isObject() || t->isDict())
          anyCyclicObject_ = true;
        if (t->isArray())
          cyclicArrayReps_.insert(representationOf(t));
      }
      for (const auto &[key, captures] : scan.envs) {
        if (cyclicEnvs_.contains(key) || !isHeld(Kind::EnvNode, key))
          continue;
        bool holds = false;
        for (const VarSymbol *var : captures)
          holds = holds || (var->boxed() ? isCyclicBox(typeOf(var)) : mayHoldMember(typeOf(var)));
        if (holds && cyclicEnvs_.insert(key).second) {
          refMember_.clear();
          changed = true;
        }
      }
      for (const VarSymbol *var : scan.boxed)
        if (isHeld(Kind::BoxNode, typeOf(var)) && mayHoldMember(typeOf(var)))
          add(cyclicBoxes_, typeOf(var));
    }
  }
  size_t before = backRefs.size();
  llvm::erase_if(backRefs, [&](const OwnerScanner::BackRef &b) {
    const ClassDecl *made = owners.madeBy(b.field);
    return !made || refMayBeMember(made->type);
  });
  if (backRefs.size() == before) {
    for (const OwnerScanner::BackRef &b : backRefs) {
      ownedFields_.insert({b.owner->type, b.field.str()});
      backRefFields_.insert({owners.madeBy(b.field)->type, b.callback.str()});
    }
    break;
  }
  }

  // WeakRef<T>: the objects of T, and of the types declared to be it.
  for (const Type *t : scan.types)
    if (t->isWeakRef())
      for (const Type *s : runtimeTypesOf(t->weakTarget()))
        weakTargets_.insert(s);

  if (hasCycles_)
    findUnstored(file);

  // The globals that hold their values for good: declared at the top level
  // of a file and never assigned again (see pinValue). Top-level code that
  // awaits keeps those no function uses in its coroutine, which ends while
  // timers may still run: they are not eternal there.
  for (Stmt *item : file.items) {
    if (auto *decl = dyn_cast<VarDeclStmt>(item))
      for (const VarDeclarator &d : decl->decls)
        if (d.sym && !d.alias && !d.sym->reassigned && mayHoldMember(d.sym->type) &&
            (d.sym->usedInFunction || !file.topLevelAwait))
          eternals_.insert(d.sym);
    // The static fields of a class are globals too.
    if (auto *cls = dyn_cast<ClassDecl>(item))
      for (const ClassField &f : cls->fields)
        if (f.isStatic && f.var && f.init && !f.var->reassigned && mayHoldMember(f.var->type))
          eternals_.insert(f.var);
  }
}

void CodeGen::findUnstored(SourceFile &file) {
  auto isSite = [this](const Expr *e) -> bool {
    switch (e->kind()) {
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      return create->cls && isCyclic(create->cls->type);
    }
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(e);
      const Type *type = lit->cls ? lit->cls->type : lit->type;
      return type && isCyclic(type);
    }
    case Expr::Kind::ArrayLit:
      return e->type && isCyclic(e->type);
    case Expr::Kind::Call:
      switch (cast<CallExpr>(e)->builtin) {
      case Builtin::ArrSlice:
      case Builtin::ArrMap:
      case Builtin::ArrFilter:
      case Builtin::ArrFlatMap:
      case Builtin::ArrConcat:
      case Builtin::DictNew:
        return e->type && isCyclic(e->type);
      default:
        return false;
      }
    case Expr::Kind::Function: {
      const FunctionDecl *fn = cast<FunctionExpr>(e)->fn;
      const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
      return !fn->nonEscaping && isCyclicEnv(key);
    }
    default:
      return false;
    }
  };
  StoreScanner scan(isSite);
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn))
        scan.function(fn);
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods)
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted()))
          scan.function(fn);
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances)
          scan.function(fn);
        for (const FunctionDecl *fn : cls->constructors)
          scan.function(fn);
        scan.function(cls->init);
        for (const ClassField &f : cls->fields)
          scan.function(f.method);
      }
      // Reflection may call any method of the class (Reflect.get, then
      // Reflect.apply), and whoever called it may keep what it gives.
      if (cls->reflectMethods)
        for (const ClassDecl *c = cls; c; c = c->parent)
          for (const FunctionDecl *fn : c->methods)
            if (!fn->isStatic)
              scan.escapes(fn);
    }
    scan.topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances)
    scan.function(fn);
  unstoredSites_ = scan.unstored();
}

bool CodeGen::refMayBeMember(const Type *type) {
  if (!hasCycles_)
    return false;
  auto found = refMember_.find(type);
  if (found != refMember_.end())
    return found->second;
  bool result = false;
  switch (type->kind()) {
  case Type::Kind::Object:
    if (type->isAnyObject())
      result = anyCyclicObject_;
    else if (!type->isWeakRef())
      result = isCyclic(type) || llvm::any_of(subtypes_, [&](const Type *s) {
                 return isCyclic(s) && s->isSubtypeOf(type);
               });
    break;
  case Type::Kind::Array:
    result = isCyclic(type) || cyclicArrayReps_.contains(type);
    break;
  case Type::Kind::Dict:
    result = isCyclic(type);
    break;
  case Type::Kind::Function:
    // The environment of a closure, or a promise: `resolve` of `new Promise`
    // has its promise as its environment, and a promise may be a member
    // (promiseIsMember, runtime/core/async/async.c).
    result = fnMayBeMember_ || hasCycles_;
    break;
  case Type::Kind::Promise:
    // By the type of its value, or of its error (promiseIsMember); a
    // promise of one type may be seen as one of another.
    result = hasCycles_;
    break;
  case Type::Kind::Unknown:
    // An object, a promise, or the box of a value that may hold a member,
    // which is a member itself (CodeGen::boxValue).
    result = hasCycles_;
    break;
  case Type::Kind::Union:
    result = llvm::any_of(type->members(), [&](const Type *m) { return mayHoldMember(m); });
    break;
  default:
    break;
  }
  refMember_[type] = result;
  return result;
}

bool CodeGen::mayHoldMember(const Type *type) {
  if (!hasCycles_)
    return false;
  switch (type->kind()) {
  case Type::Kind::Optional:
    return mayHoldMember(type->inner());
  case Type::Kind::Result:
    return mayHoldMember(type->okType()) || mayHoldMember(type->errType());
  default:
    return refMayBeMember(type);
  }
}

//===----------------------------------------------------------------------===//
// Emission
//===----------------------------------------------------------------------===//

llvm::FunctionCallee CodeGen::groupFunction(llvm::StringRef name, llvm::Type *ret,
                                            llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);
  return callee;
}

llvm::Value *CodeGen::allocMember(uint64_t size, const llvm::Twine &name) {
  llvm::FunctionCallee alloc =
      groupFunction("tz_group_alloc", builder_.getPtrTy(), {builder_.getInt64Ty()});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);
  return builder_.CreateCall(alloc, {builder_.getInt64(size)}, name);
}

void CodeGen::adoptFields(llvm::Value *object, const Type *type) {
  if (!isCyclic(type))
    return;
  const ObjectLayout &layout = objectLayout(type);
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i)
    if (mayHoldMember(fields[i].type))
      groupStore(object,
                 builder_.CreateLoad(lower(fields[i].type),
                                     builder_.CreateStructGEP(layout.type, object, layout.elements[i])),
                 fields[i].type);
}

llvm::Constant *CodeGen::memberDescriptor(llvm::Function *drop, llvm::StringRef name) {
  llvm::Constant *&descriptor = memberDescriptors_[drop];
  if (descriptor)
    return descriptor;
  llvm::Constant *null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      layoutDescTy_, {drop, null, builder_.getInt32(0), builder_.getInt32(0), utf8Constant(name),
                      builder_.getInt32(uint32_t(name.size())), builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(*module_, layoutDescTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          drop->getName() + ".layout");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;
  return global;
}

llvm::Value *CodeGen::newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                               const llvm::Twine &name) {
  return newArray(arrayType, length, capacity, isCyclic(arrayType), name);
}

llvm::Value *CodeGen::newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                               bool member, const llvm::Twine &name) {
  llvm::Constant *element = typeDescriptor(arrayType->inner());
  if (!member)
    return builder_.CreateCall(rt_.arrNew, {element, length, capacity}, name);
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  llvm::FunctionCallee alloc = groupFunction("tz_arr_new_group", ptr, {ptr, i64, i64});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);
  return builder_.CreateCall(alloc, {element, length, capacity}, name);
}

llvm::Function *CodeGen::groupAwareRelease(llvm::Function *plain) {
  llvm::Function *&fn = groupReleases_[plain];
  if (fn)
    return fn;
  llvm::Type *ptr = builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, plain->getName() + ".g",
                              module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(ctx_, "counted", fn);
  auto *memberBlock = llvm::BasicBlock::Create(ctx_, "member", fn);
  auto *plainBlock = llvm::BasicBlock::Create(ctx_, "plain", fn);
  auto *doneBlock = llvm::BasicBlock::Create(ctx_, "done", fn);
  llvm::Value *object = fn->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), object, "rc");
  if (parallel_) {
    // A shared object, no member: counted atomically; the last release
    // hands it to the plain release with a count of 1, which frees it.
    auto *notMemberBlock = llvm::BasicBlock::Create(ctx_, "not.member", fn);
    auto *sharedBlock = llvm::BasicBlock::Create(ctx_, "shared", fn);
    auto *lastBlock = llvm::BasicBlock::Create(ctx_, "shared.last", fn);
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
                   notMemberBlock);
    b.SetInsertPoint(notMemberBlock);
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), sharedBlock, plainBlock);
    b.SetInsertPoint(sharedBlock);
    llvm::Value *old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, object, b.getInt64(1),
                                         llvm::MaybeAlign(8), llvm::AtomicOrdering::AcquireRelease);
    b.CreateCondBr(b.CreateICmpEQ(old, b.getInt64(uint64_t(-(int64_t(1) << 60) + 1))), lastBlock, doneBlock);
    b.SetInsertPoint(lastBlock);
    b.CreateStore(b.getInt64(1), object);
    b.CreateBr(plainBlock);
  } else {
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
  }
  b.SetInsertPoint(memberBlock);
  b.CreateCall(groupFunction("tz_group_release", b.getVoidTy(), {ptr}), {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(plainBlock);
  b.CreateCall(plain, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();
  return fn;
}

void CodeGen::retainObject(llvm::Value *object) {
  builder_.CreateCall(hasCycles_ ? refRetainFn_ : arrRetainFn_, {object});
}

namespace {

// The code of a group function may land in a drop function that is emitted
// while another one is (objectLayout makes them when first needed): its
// blocks and stack slots go to the function the builder is in.
llvm::BasicBlock *blockHere(llvm::IRBuilder<> &b, const char *name) {
  return llvm::BasicBlock::Create(b.getContext(), name, b.GetInsertBlock()->getParent());
}

llvm::Value *spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name) {
  llvm::BasicBlock &entry = b.GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> at(&entry, entry.begin());
  llvm::AllocaInst *slot = at.CreateAlloca(value->getType(), nullptr, name);
  b.CreateStore(value, slot);
  return slot;
}

// How CodeGen walks the references in a value for the group functions: the
// reference itself, or the environment of a function; anything else goes to
// the runtime by its descriptor.
enum class RefShape { None, Pointer, Function, Optional, Other };

} // namespace

static RefShape refShape(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Object:
  case Type::Kind::Array:
  case Type::Kind::Dict:
  case Type::Kind::Unknown:
  case Type::Kind::Promise: // which may be a member (promiseIsMember)
    return RefShape::Pointer;
  case Type::Kind::Function:
    return RefShape::Function;
  case Type::Kind::Union:
    return type->isTagged() ? RefShape::Other : RefShape::Pointer; // one of objects
  case Type::Kind::Optional:
    return RefShape::Optional;
  case Type::Kind::Result:
    return RefShape::Other;
  default:
    return RefShape::None;
  }
}

void CodeGen::groupStore(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!mayHoldMember(type) || isa<llvm::Constant>(value))
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  // Debug mode notes where the store is, for what a walk reports.
  if (debugMemory_) {
    // Each thread's own in a parallel program (runtime/core/group/group.c).
    auto *site = cast<llvm::GlobalVariable>(module_->getOrInsertGlobal("tz_group_site", ptr));
    site->setThreadLocal(parallel_);
    builder_.CreateStore(groupSite(), parallel_ ? static_cast<llvm::Value *>(
                                                      builder_.CreateThreadLocalAddress(site))
                                                : site);
  }
  switch (refShape(type)) {
  case RefShape::Pointer:
    builder_.CreateCall(groupFunction("tz_group_store", builder_.getVoidTy(), {ptr, ptr}),
                        {owner, value});
    return;
  case RefShape::Function:
    builder_.CreateCall(groupFunction("tz_group_store", builder_.getVoidTy(), {ptr, ptr}),
                        {owner, builder_.CreateExtractValue(value, 1)});
    return;
  case RefShape::Optional:
    // Undefined holds zero, which refers to nothing.
    groupStore(owner, optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    builder_.CreateCall(
        groupFunction("tz_group_store_value", builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, typeDescriptor(type), spillHere(builder_, value, "stored")});
    return;
  case RefShape::None:
    return;
  }
}

void CodeGen::releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!mayHoldMember(type) || isa<llvm::Constant>(value)) {
    releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = builder_.getPtrTy();
  auto viaGroup = [&](llvm::Value *ref, const std::function<void()> &plain) {
    llvm::BasicBlock *refBlock = blockHere(builder_, "in.ref");
    llvm::BasicBlock *memberBlock = blockHere(builder_, "in.member");
    llvm::BasicBlock *plainBlock = blockHere(builder_, "in.plain");
    llvm::BasicBlock *doneBlock = blockHere(builder_, "in.done");
    builder_.CreateCondBr(builder_.CreateIsNotNull(ref), refBlock, doneBlock);
    builder_.SetInsertPoint(refBlock);
    llvm::Value *count = builder_.CreateLoad(builder_.getInt64Ty(), ref, "rc");
    builder_.CreateCondBr(builder_.CreateICmpSLT(count, builder_.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
                          plainBlock);
    builder_.SetInsertPoint(memberBlock);
    builder_.CreateCall(groupFunction("tz_group_unstore", builder_.getVoidTy(), {ptr, ptr}),
                        {owner, ref});
    builder_.CreateBr(doneBlock);
    builder_.SetInsertPoint(plainBlock);
    plain();
    builder_.CreateBr(doneBlock);
    builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    viaGroup(value, [&] { releaseValue(value, type); });
    return;
  case RefShape::Function:
    viaGroup(builder_.CreateExtractValue(value, 1), [&] { releaseValue(value, type); });
    return;
  case RefShape::Optional:
    releaseIn(owner, optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    builder_.CreateCall(
        groupFunction("tz_value_release_in", builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, typeDescriptor(type), spillHere(builder_, value, "released")});
    return;
  case RefShape::None:
    releaseValue(value, type);
    return;
  }
}

void CodeGen::takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!mayHoldMember(type) || isa<llvm::Constant>(value))
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  auto take = [&](llvm::Value *ref) {
    llvm::BasicBlock *refBlock = blockHere(builder_, "take.ref");
    llvm::BasicBlock *memberBlock = blockHere(builder_, "take.member");
    llvm::BasicBlock *doneBlock = blockHere(builder_, "take.done");
    builder_.CreateCondBr(builder_.CreateIsNotNull(ref), refBlock, doneBlock);
    builder_.SetInsertPoint(refBlock);
    llvm::Value *count = builder_.CreateLoad(builder_.getInt64Ty(), ref, "rc");
    builder_.CreateCondBr(builder_.CreateICmpSLT(count, builder_.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
                          doneBlock);
    builder_.SetInsertPoint(memberBlock);
    builder_.CreateCall(groupFunction("tz_group_take", builder_.getVoidTy(), {ptr, ptr}),
                        {owner, ref});
    builder_.CreateBr(doneBlock);
    builder_.SetInsertPoint(doneBlock);
  };
  switch (refShape(type)) {
  case RefShape::Pointer:
    take(value);
    return;
  case RefShape::Function:
    take(builder_.CreateExtractValue(value, 1));
    return;
  case RefShape::Optional:
    takeFrom(owner, optionalValue(value, type), type->inner());
    return;
  case RefShape::Other:
    builder_.CreateCall(
        groupFunction("tz_group_take_value", builder_.getVoidTy(), {ptr, ptr, ptr}),
        {owner, typeDescriptor(type), spillHere(builder_, value, "taken")});
    return;
  case RefShape::None:
    return;
  }
}

void CodeGen::storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type) {
  if (!parallel_ || !type->isManaged() || isa<llvm::Constant>(value))
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *count = builder_.CreateLoad(builder_.getInt64Ty(), owner, "owner.rc");
  llvm::BasicBlock *barrierBlock = newBlock("barrier");
  llvm::BasicBlock *doneBlock = newBlock("barrier.done");
  builder_.CreateCondBr(builder_.CreateICmpSLT(count, builder_.getInt64(0)), barrierBlock, doneBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(barrierBlock);
  // An array goes by its type: it has no descriptor of its own.
  RefShape shape = type->isString() || type->isArray() ? RefShape::Other : refShape(type);
  if (shape == RefShape::Pointer || shape == RefShape::Function) {
    llvm::Value *object = shape == RefShape::Function ? builder_.CreateExtractValue(value, 1) : value;
    builder_.CreateCall(processFunction("tz_store_barrier_ref", builder_.getVoidTy(), {ptr, ptr}),
                        {owner, object});
  } else {
    builder_.CreateCall(processFunction("tz_store_barrier", builder_.getVoidTy(), {ptr, ptr, ptr}),
                        {owner, typeDescriptor(type), spillHere(builder_, value, "published")});
  }
  builder_.CreateBr(doneBlock);
  switchTo(doneBlock);
}

void CodeGen::publishValue(llvm::Value *value, const Type *type) {
  if (!parallel_ || !type->isManaged() || isa<llvm::Constant>(value))
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(processFunction("tz_publish_value", builder_.getVoidTy(), {ptr, ptr}),
                      {typeDescriptor(type), spillHere(builder_, value, "published")});
}

void CodeGen::storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address,
                      llvm::Value *value, const Type *type) {
  storeBarrier(owner, value, type);
  if (!type->isManaged() || !mayHoldMember(type) || (ownerType && !ownerMayBeMember(ownerType))) {
    storeReplacing(address, value, type);
    return;
  }
  llvm::Value *previous = builder_.CreateLoad(lower(type), address);
  builder_.CreateStore(value, address);
  groupStore(owner, value, type);
  releaseIn(owner, previous, type);
}

void CodeGen::storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value,
                       bool replacing) {
  const Type *type = typeOf(var);
  // What a global holds, functions may read in any task; a box, closures.
  if (var->isGlobal && var->usedInFunction)
    publishValue(value, type);
  else if (var->boxed() && !(isCyclicBox(type) && mayHoldMember(type)))
    storeBarrier(boxPointer(var), value, type);
  if (var->boxed() && isCyclicBox(type) && mayHoldMember(type)) {
    llvm::Value *box = boxPointer(var);
    if (replacing) {
      storeIn(box, nullptr, address, value, type);
    } else {
      builder_.CreateStore(value, address);
      groupStore(box, value, type);
    }
    return;
  }
  if (replacing)
    storeReplacing(address, value, type);
  else
    builder_.CreateStore(value, address);
}

void CodeGen::releaseObjectIn(llvm::Value *owner, llvm::Value *object) {
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::BasicBlock *refBlock = blockHere(builder_, "in.ref");
  llvm::BasicBlock *memberBlock = blockHere(builder_, "in.member");
  llvm::BasicBlock *plainBlock = blockHere(builder_, "in.plain");
  llvm::BasicBlock *doneBlock = blockHere(builder_, "in.done");
  builder_.CreateCondBr(builder_.CreateIsNotNull(object), refBlock, doneBlock);
  builder_.SetInsertPoint(refBlock);
  llvm::Value *count = builder_.CreateLoad(builder_.getInt64Ty(), object, "rc");
  builder_.CreateCondBr(builder_.CreateICmpSLT(count, builder_.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
                        plainBlock);
  builder_.SetInsertPoint(memberBlock);
  builder_.CreateCall(groupFunction("tz_group_unstore", builder_.getVoidTy(), {ptr, ptr}),
                      {owner, object});
  builder_.CreateBr(doneBlock);
  builder_.SetInsertPoint(plainBlock);
  builder_.CreateCall(plainObjReleaseFn_, {object});
  builder_.CreateBr(doneBlock);
  builder_.SetInsertPoint(doneBlock);
}

void CodeGen::forgetWeakRefs(llvm::Value *object) {
  llvm::Type *i64 = builder_.getInt64Ty();
  llvm::Value *count = builder_.CreateLoad(i64, module_->getOrInsertGlobal("tz_weak_count", i64));
  llvm::BasicBlock *forgetBlock = blockHere(builder_, "weak.forget");
  llvm::BasicBlock *doneBlock = blockHere(builder_, "weak.done");
  builder_.CreateCondBr(builder_.CreateICmpNE(count, builder_.getInt64(0)), forgetBlock, doneBlock);
  builder_.SetInsertPoint(forgetBlock);
  builder_.CreateCall(groupFunction("tz_weak_forget", builder_.getVoidTy(), {builder_.getPtrTy()}),
                      {object});
  builder_.CreateBr(doneBlock);
  builder_.SetInsertPoint(doneBlock);
}

// Releases what each field holds as a member lets go of it, then frees the
// object unless it is one: a group frees its members' memory itself, after
// all of them released what they hold (runtime/core/group/group.c).
void CodeGen::emitMemberDrop(const Type *type, ObjectLayout &layout) {
  llvm::Function *drop = layout.drop;
  llvm::Function *saved = function_;
  function_ = drop;
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", drop));
  llvm::Value *object = drop->getArg(0);
  if (isWeakTarget(type))
    forgetWeakRefs(object);
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    const Type *fieldType = fields[i].type;
    if (!fieldType->isManaged())
      continue;
    llvm::Value *address = builder_.CreateStructGEP(layout.type, object, layout.elements[i]);
    llvm::Value *value = builder_.CreateLoad(lower(fieldType), address, fields[i].name);
    checkOwned(type, fields[i].name, value, fieldType);
    releaseIn(object, value, fieldType);
  }
  // The variables a literal's methods captured.
  if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
    const std::vector<VarSymbol *> &captures = cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *address =
          builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i));
      if (var->boxed()) {
        releaseObjectIn(object,
                        builder_.CreateLoad(builder_.getPtrTy(), address, var->name + ".box"));
      } else if (typeOf(var)->isManaged()) {
        const Type *varType = typeOf(var);
        releaseIn(object, builder_.CreateLoad(lower(varType), address, var->name), varType);
      }
    }
  }
  builder_.CreateCall(groupFunction("tz_group_obj_free", builder_.getVoidTy(), {builder_.getPtrTy()}),
                      {object});
  builder_.CreateRetVoid();
  function_ = saved;
}

llvm::Value *CodeGen::emitWeakRefCall(CallExpr *call) {
  llvm::Type *ptr = builder_.getPtrTy();
  if (call->builtin == Builtin::WeakRefNew) {
    // The WeakRef borrows the object, which it does not hold.
    Expr *arg = call->args[0];
    llvm::Value *target = emitExpr(arg);
    llvm::Value *weak = builder_.CreateCall(groupFunction("tz_weak_new", ptr, {ptr, ptr}),
                                            {layoutDescriptor(call->type, {}), target}, "weak");
    releaseValue(target, arg->type);
    return weak;
  }
  Operand weak = emitOperand(cast<MemberExpr>(call->callee)->object, /*canBorrow=*/true);
  llvm::Value *target =
      builder_.CreateCall(groupFunction("tz_weak_deref", ptr, {ptr}), {weak.value}, "target");
  dropOperand(weak);
  return target;
}

//===----------------------------------------------------------------------===//
// Debug mode
//===----------------------------------------------------------------------===//

llvm::Constant *CodeGen::groupSite() {
  llvm::Constant *frame = frameDescriptor(storeLoc_.isValid() ? storeLoc_ : loc_);
  llvm::Constant *&site = groupSites_[{frame, storeField_}];
  if (site)
    return site;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  auto *type = llvm::StructType::get(ctx_, {ptr, ptr, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type, {frame, utf8Constant(storeField_), builder_.getInt32(uint32_t(storeField_.size()))});
  auto *global = new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.site");
  site = global;
  return site;
}

void CodeGen::noteUnstored(llvm::Value *object, const Expr *site) {
  if (!debugMemory_ || !unstoredSites_.contains(site))
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(groupFunction("tz_group_note_unstored", builder_.getVoidTy(), {ptr, ptr}),
                      {object, frameDescriptor(site->loc())});
}

void CodeGen::checkOwned(const Type *type, llvm::StringRef name, llvm::Value *value,
                         const Type *fieldType) {
  if (!debugMemory_)
    return;
  auto held = [&](const std::set<std::pair<const Type *, std::string>> &fields) -> const Type * {
    for (const auto &[owner, field] : fields)
      if (field == name && type->isSubtypeOf(owner))
        return owner;
    return nullptr;
  };
  const Type *owner = held(ownedFields_);
  // The function in an owned object's field: only that object holds it.
  bool callback = !owner && (owner = held(backRefFields_)) != nullptr;
  if (!owner)
    return;
  llvm::Value *object = value;
  if (fieldType->isOptional() && !zeroIsUndefined(fieldType->inner()))
    object = optionalValue(value, fieldType);
  if (callback || fieldType->nonOptional()->isFunction())
    object = builder_.CreateExtractValue(object, 1);
  std::string text = owner->name().str() + "." + name.str();
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(groupFunction("tz_group_check_owned", builder_.getVoidTy(),
                                    {ptr, ptr, ptr, builder_.getInt32Ty()}),
                      {object, frameDescriptor(loc_), utf8Constant(text),
                       builder_.getInt32(uint32_t(text.size()))});
}

// void trace(ptr object, ptr walk): each field that may hold a member, and
// the variables captured after them, to the walk.
llvm::Function *CodeGen::traceFunction(
    const Type *type, llvm::StructType *layout,
    llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures, llvm::StringRef name) {
  llvm::Type *ptr = builder_.getPtrTy();
  auto *fnType = llvm::FunctionType::get(builder_.getVoidTy(), {ptr, ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage, "tz.trace." + name,
                                    module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
  llvm::Value *object = fn->getArg(0), *walk = fn->getArg(1);
  llvm::FunctionCallee value = groupFunction("tz_group_trace_value", builder_.getVoidTy(),
                                             {ptr, ptr, ptr, ptr});
  llvm::FunctionCallee ref = groupFunction("tz_group_trace_ref", builder_.getVoidTy(),
                                           {ptr, ptr, ptr, builder_.getInt32Ty()});
  auto one = [&](const Type *t, unsigned element, llvm::StringRef field) {
    if (!mayHoldMember(t))
      return;
    b.CreateCall(value, {walk, typeDescriptor(t), b.CreateStructGEP(layout, object, element),
                         utf8Constant(field)});
  };
  if (type) {
    const ObjectLayout &l = objectLayout(type);
    for (unsigned i = 0; i < type->fields().size(); ++i)
      one(type->fields()[i].type, l.elements[i], type->fields()[i].name);
  }
  for (auto [var, element] : captures) {
    if (var->boxed())
      b.CreateCall(ref, {walk, b.CreateLoad(ptr, b.CreateStructGEP(layout, object, element)),
                         utf8Constant(var->name), b.getInt32(uint32_t(var->name.size()))});
    else
      one(typeOf(var), element, var->name);
  }
  b.CreateRetVoid();
  return fn;
}

void CodeGen::finishGroupDebug(llvm::Instruction *init) {
  if (!debugMemory_ || !hasCycles_)
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(ctx_, {ptr, ptr});
  auto add = [&](llvm::Constant *descriptor, llvm::Function *trace) {
    entries.push_back(llvm::ConstantStruct::get(entryType, {descriptor, trace}));
  };
  std::map<const Type *, llvm::Function *> traces;
  auto objectTrace = [&](const Type *type) {
    llvm::Function *&trace = traces[type];
    if (!trace) {
      const ObjectLayout &l = objectLayout(type);
      std::vector<std::pair<const VarSymbol *, unsigned>> captures;
      if (const ClassDecl *cls = type->classDecl(); cls && cls->group)
        for (size_t i = 0; i < cls->group->captures.size(); ++i)
          captures.push_back({cls->group->captures[i], unsigned(l.captures + i)});
      trace = traceFunction(type, l.type, captures, "object");
    }
    return trace;
  };
  for (const auto &[key, descriptor] : layoutDescriptors_)
    if (isCyclic(key.first))
      add(descriptor, objectTrace(key.first));
  for (const auto &[cls, descriptor] : classDescriptors_)
    if (isCyclic(cls->type))
      add(descriptor, objectTrace(cls->type));
  for (const auto &[key, layout] : envLayouts_) {
    if (!layout.member)
      continue;
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i)
      if (!uncounted_.count({key, layout.fields[i]}))
        captures.push_back({layout.fields[i], unsigned(i + 2)});
    add(layout.descriptor, traceFunction(nullptr, layout.type, captures, "env"));
  }
  for (const auto &[descriptor, type] : boxDescriptors_) {
    llvm::Function *trace = traceFunction(nullptr, boxType(type), {}, "box");
    llvm::IRBuilder<> b(&trace->getEntryBlock(), trace->getEntryBlock().begin());
    if (mayHoldMember(type))
      b.CreateCall(groupFunction("tz_group_trace_value", builder_.getVoidTy(), {ptr, ptr, ptr, ptr}),
                   {trace->getArg(1), typeDescriptor(type),
                    b.CreateStructGEP(boxType(type), trace->getArg(0), 2), utf8Constant("value")});
    add(descriptor, trace);
  }
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         llvm::ConstantArray::get(arrayType, entries),
                                         "tz.group.traces");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(groupFunction("tz_group_debug_init", builder_.getVoidTy(), {ptr, builder_.getInt32Ty()}),
               {table, b.getInt32(uint32_t(entries.size()))});
}

// void publish(ptr object): every value the object holds, to the runtime,
// which makes it shared (runtime/core/publish/publish.c); the boxes of the
// variables it captures as objects.
llvm::Function *CodeGen::publishFunction(
    const Type *type, llvm::StructType *layout,
    llvm::ArrayRef<std::pair<const VarSymbol *, unsigned>> captures,
    llvm::ArrayRef<std::pair<unsigned, const Type *>> values, llvm::StringRef name) {
  llvm::Type *ptr = builder_.getPtrTy();
  auto *fnType = llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false);
  auto *fn = llvm::Function::Create(fnType, llvm::GlobalValue::InternalLinkage,
                                    "tz.publish." + name, module_.get());
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx_, "entry", fn));
  llvm::Value *object = fn->getArg(0);
  llvm::FunctionCallee value =
      processFunction("tz_publish_value", builder_.getVoidTy(), {ptr, ptr});
  llvm::FunctionCallee ref = processFunction("tz_publish_object", builder_.getVoidTy(), {ptr});
  auto one = [&](const Type *t, unsigned element) {
    if (!t->isManaged())
      return;
    b.CreateCall(value, {typeDescriptor(t), b.CreateStructGEP(layout, object, element)});
  };
  if (type) {
    const ObjectLayout &l = objectLayout(type);
    for (unsigned i = 0; i < type->fields().size(); ++i)
      one(type->fields()[i].type, l.elements[i]);
  }
  for (auto [var, element] : captures) {
    if (var->boxed())
      b.CreateCall(ref, {b.CreateLoad(ptr, b.CreateStructGEP(layout, object, element))});
    else
      one(typeOf(var), element);
  }
  for (auto [element, t] : values)
    one(t, element);
  b.CreateRetVoid();
  return fn;
}

void CodeGen::finishPublishTable(llvm::Instruction *init) {
  if (!parallel_)
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  std::vector<llvm::Constant *> entries;
  auto *entryType = llvm::StructType::get(ctx_, {ptr, ptr});
  llvm::DenseSet<llvm::Constant *> seen;
  auto add = [&](llvm::Constant *descriptor, llvm::Function *publish) {
    if (seen.insert(descriptor).second)
      entries.push_back(llvm::ConstantStruct::get(entryType, {descriptor, publish}));
  };
  std::map<const Type *, llvm::Function *> objects;
  auto objectPublish = [&](const Type *type) {
    llvm::Function *&publish = objects[type];
    if (!publish) {
      const ObjectLayout &l = objectLayout(type);
      std::vector<std::pair<const VarSymbol *, unsigned>> captures;
      if (const ClassDecl *cls = type->classDecl(); cls && cls->group)
        for (size_t i = 0; i < cls->group->captures.size(); ++i)
          captures.push_back({cls->group->captures[i], unsigned(l.captures + i)});
      publish = publishFunction(type, l.type, captures, {}, "object");
    }
    return publish;
  };
  // Making a layout may make more: by index, as the map grows.
  std::vector<std::pair<const Type *, llvm::Constant *>> layouts;
  for (const auto &[key, descriptor] : layoutDescriptors_)
    layouts.push_back({key.first, descriptor});
  for (const auto &[type, descriptor] : layouts)
    if (type->isObject())
      add(descriptor, objectPublish(type));
  for (const auto &[cls, descriptor] : classDescriptors_)
    add(descriptor, objectPublish(cls->type));
  for (const auto &[key, layout] : envLayouts_) {
    std::vector<std::pair<const VarSymbol *, unsigned>> captures;
    for (size_t i = 0; i < layout.fields.size(); ++i)
      if (!uncounted_.count({key, layout.fields[i]}))
        captures.push_back({layout.fields[i], unsigned(i + 2)});
    add(layout.descriptor, publishFunction(nullptr, layout.type, captures, {}, "env"));
  }
  for (const auto &[type, drop] : boxDrops_) {
    if (!type->isManaged())
      continue;
    llvm::Function *publish = publishFunction(nullptr, boxType(type), {}, {{2u, type}}, "box");
    if (llvm::Constant *descriptor = dropDescriptors_.lookup(drop))
      add(descriptor, publish);
    if (llvm::Constant *descriptor = memberDescriptors_.lookup(drop))
      add(descriptor, publish);
  }
  for (const PublishedObject &o : publishedObjects_)
    add(o.descriptor, publishFunction(nullptr, o.type, {}, o.values, "other"));
  auto *arrayType = llvm::ArrayType::get(entryType, entries.size());
  auto *table = new llvm::GlobalVariable(*module_, arrayType, /*isConstant=*/true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         llvm::ConstantArray::get(arrayType, entries),
                                         "tz.publish.table");
  llvm::IRBuilder<> b(init->getNextNode());
  b.CreateCall(processFunction("tz_publish_init", builder_.getVoidTy(), {ptr, builder_.getInt32Ty()}),
               {table, b.getInt32(uint32_t(entries.size()))});
}

void CodeGen::pinValue(llvm::Value *value, const Type *type) {
  if (!mayHoldMember(type) || isa<llvm::Constant>(value))
    return;
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(groupFunction("tz_group_pin_value", builder_.getVoidTy(), {ptr, ptr}),
                      {typeDescriptor(type), spillHere(builder_, value, "pinned")});
}

void CodeGen::endValue(llvm::Value *value, const Type *type) {
  if (!mayHoldMember(type)) {
    releaseValue(value, type);
    return;
  }
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(groupFunction("tz_group_end_value", builder_.getVoidTy(), {ptr, ptr}),
                      {typeDescriptor(type), spillHere(builder_, value, "ended")});
}

} // namespace tz
