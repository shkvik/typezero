#include "compiler/library/async_hooks/context_checker/context_checker.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/module/module.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kSection = "(docs/async.md, section 10)";

void children(const Expr *e, const std::function<void(const Expr *)> &visit) {
  switch (e->kind()) {
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs) {
      visit(x);
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
  case Expr::Kind::Update:
    visit(cast<UpdateExpr>(e)->target);
    break;
  case Expr::Kind::Conditional:
    visit(cast<ConditionalExpr>(e)->cond);
    visit(cast<ConditionalExpr>(e)->thenExpr);
    visit(cast<ConditionalExpr>(e)->elseExpr);
    break;
  case Expr::Kind::Cast:
    visit(cast<CastExpr>(e)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *x : cast<SequenceExpr>(e)->exprs) {
      visit(x);
    }
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

std::string calleeName(const CallExpr *c) {
  const Expr *callee = c->callee;
  if (auto *member = dyn_cast_or_null<MemberExpr>(callee)) {
    return member->name.str();
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(callee)) {
    return name->name.str();
  }

  return "";
}

}

ContextChecker::ContextChecker(Program &program) : program_(program) {}

bool ContextChecker::usesStorage() const {
  return llvm::any_of(program_.file.modules,
                      [](const Module *m) { return m->builtin && m->name == "node:async_hooks"; });
}

bool ContextChecker::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();

  return llvm::any_of(libraryRanges_, [&](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  });
}

void ContextChecker::run() {
  if (!usesStorage()) {
    return;
  }
  for (const Module *module : program_.file.modules) {
    if (!module->builtin) {
      continue;
    }
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = program_.sourceMgr.getMemoryBuffer(id);
      libraryRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
    }
  }
  for (int round = 0; round < 32; ++round) {
    size_t known = storeVars_.size() + storeParams_.size() + returnsStore_.size();
    walkProgram();
    if (storeVars_.size() + storeParams_.size() + returnsStore_.size() == known) {
      break;
    }
  }
  report_ = true;
  walkProgram();
  checkNews();
  checkRuns();
  checkIntervals();
}

void ContextChecker::walkProgram() {
  walked_.clear();
  callees_.clear();
  readsDirectly_.clear();
  intervals_.clear();
  runs_.clear();
  news_.clear();
  allowedNews_.clear();
  SourceFile &file = program_.file;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
          function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          function(fn);
        }
        function(cls->init);
        for (const ClassField &f : cls->fields) {
          function(f.method);
        }
      }
    }
    topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    function(fn);
  }
}

void ContextChecker::function(const FunctionDecl *fn) {
  if (!fn || !fn->body || inLibrary(fn->loc()) || !walked_.insert(fn).second) {
    return;
  }
  const FunctionDecl *saved = current_;
  current_ = fn;
  for (unsigned i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    if (param.sym && storeParams_.count({fn, i})) {
      storeVars_.insert(param.sym);
    }
    expr(param.defaultValue);
  }
  stmt(fn->body);
  current_ = saved;
}

void ContextChecker::topLevel(const Stmt *s) {
  if (!s || inLibrary(s->loc())) {
    return;
  }
  current_ = nullptr;
  if (auto *decl = dyn_cast<VarDeclStmt>(s); decl && decl->isConst) {
    for (const VarDeclarator &d : decl->decls) {
      if (!d.destructured) {
        allowedNews_.insert(bare(d.init));
      }
    }
  }
  stmt(s);
}

void ContextChecker::stmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    if (NestedGroup *group = cast<BlockStmt>(s)->group) {
      for (const FunctionDecl *member : group->members) {
        function(member);
      }
    }
    for (const Stmt *x : cast<BlockStmt>(s)->body) {
      stmt(x);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
      expr(d.init);
      if (!d.sym || d.destructured || !isStore(d.init)) {
        continue;
      }
      if (d.sym->isGlobal) {
        keep(d.init, "in a module variable");
      } else {
        storeVars_.insert(d.sym);
      }
    }
    break;
  case Stmt::Kind::Function:
    function(cast<FunctionDecl>(s));
    break;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(s)->expr);
    break;
  case Stmt::Kind::If:
    expr(cast<IfStmt>(s)->cond);
    stmt(cast<IfStmt>(s)->thenStmt);
    stmt(cast<IfStmt>(s)->elseStmt);
    break;
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(s)->cond);
    stmt(cast<WhileStmt>(s)->body);
    break;
  case Stmt::Kind::DoWhile:
    stmt(cast<DoWhileStmt>(s)->body);
    expr(cast<DoWhileStmt>(s)->cond);
    break;
  case Stmt::Kind::For:
    stmt(cast<ForStmt>(s)->init);
    expr(cast<ForStmt>(s)->cond);
    expr(cast<ForStmt>(s)->step);
    stmt(cast<ForStmt>(s)->body);
    break;
  case Stmt::Kind::ForOf:
    expr(cast<ForOfStmt>(s)->iterable);
    stmt(cast<ForOfStmt>(s)->body);
    break;
  case Stmt::Kind::Switch:
    expr(cast<SwitchStmt>(s)->discriminant);
    for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
      expr(c.test);
      for (const Stmt *x : c.body) {
        stmt(x);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    stmt(cast<LabeledStmt>(s)->body);
    break;
  case Stmt::Kind::Return: {
    const Expr *value = cast<ReturnStmt>(s)->value;
    expr(value);
    if (current_ && isStore(value)) {
      returnsStore_.insert(current_);
    }
    break;
  }
  case Stmt::Kind::Class: {
    auto *cls = cast<ClassDecl>(s);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic) {
        expr(f.init);
      }
    }
    for (const Stmt *x : cls->decorations) {
      stmt(x);
    }
    break;
  }
  default:
    break;
  }
}

void ContextChecker::expr(const Expr *e) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Function:
    function(cast<FunctionExpr>(e)->fn);
    return;
  case Expr::Kind::Call:
    call(cast<CallExpr>(e));
    return;
  case Expr::Kind::New:
    create(cast<NewExpr>(e));
    return;
  case Expr::Kind::Assign:
    assign(cast<AssignExpr>(e));
    return;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
      expr(p.key);
      expr(p.value);
      if (!p.spread && isStore(p.value)) {
        keep(p.value, "in a new object");
      }
    }
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &x : cast<ArrayLitExpr>(e)->elements) {
      expr(x.expr);
      if (!x.spread && isStore(x.expr)) {
        keep(x.expr, "in an array");
      }
    }
    return;
  default:
    children(e, [&](const Expr *x) { expr(x); });
    return;
  }
}

void ContextChecker::call(const CallExpr *c) {
  expr(c->callee);
  for (const Expr *arg : c->args) {
    expr(arg);
  }
  const FunctionDecl *target = programTarget(c);
  bool runs = storageMethod(c, "run");
  if (current_) {
    if (target) {
      callees_[current_].push_back(target);
    }
    if (!runs) {
      for (const Expr *arg : c->args) {
        if (auto *fe = dyn_cast<FunctionExpr>(bare(arg))) {
          callees_[current_].push_back(fe->fn);
        }
      }
    }
    if (storageMethod(c, "getStore")) {
      readsDirectly_.insert(current_);
    }
  }
  if (runs) {
    runs_.push_back(c);
    return;
  }
  if (c->builtin == Builtin::SetInterval ||
      (c->builtin == Builtin::SetTimeout && c->args.size() > 2)) {
    intervals_.push_back(c);
  }
  passed(c, target, calleeName(c));
}

void ContextChecker::passed(const CallExpr *c, const FunctionDecl *target, llvm::StringRef name) {
  for (unsigned i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    if (!isStore(arg)) {
      continue;
    }
    if (target) {
      markParam(target, i);
      continue;
    }
    if (parallel::onlyReadsArgs(c->builtin)) {
      continue;
    }
    if (name.empty()) {
      keep(arg, "by a function the compiler can't follow");
    } else {
      keep(arg, "by '" + name + "', which may store it");
    }
  }
}

void ContextChecker::create(const NewExpr *n) {
  const std::vector<Expr *> &args = n->ctor ? n->ctor->args : n->args;
  for (const Expr *arg : args) {
    expr(arg);
  }
  if (storageClass(n->cls)) {
    news_.push_back(n);
  }
  const FunctionDecl *target = n->ctor ? programTarget(n->ctor) : nullptr;
  if (current_ && target) {
    callees_[current_].push_back(target);
  }
  for (unsigned i = 0; i < args.size(); ++i) {
    if (!isStore(args[i])) {
      continue;
    }
    if (target) {
      markParam(target, i);
    } else {
      keep(args[i], "in a new object");
    }
  }
}

void ContextChecker::assign(const AssignExpr *a) {
  expr(a->target);
  expr(a->value);
  if (a->op || !isStore(a->value)) {
    return;
  }
  const Expr *target = bare(a->target);
  if (auto *name = dyn_cast<NameExpr>(target); name && name->var) {
    const VarSymbol *var = name->var;
    if (var->isGlobal) {
      keep(a->value, "in a module variable");
    } else if (var->owner && var->owner != current_) {
      keep(a->value, "in a variable of an outer function");
    } else {
      storeVars_.insert(var);
    }
    return;
  }
  if (isa<IndexExpr>(target)) {
    keep(a->value, "in an array or a map");
    return;
  }
  keep(a->value, "in a field");
}

bool ContextChecker::isStore(const Expr *e) const {
  e = bare(e);
  if (!e) {
    return false;
  }
  switch (e->kind()) {
  case Expr::Kind::Call: {
    auto *c = cast<CallExpr>(e);
    if (storageMethod(c, "getStore")) {
      return true;
    }
    const FunctionDecl *target = programTarget(c);
    return target && returnsStore_.contains(target);
  }
  case Expr::Kind::Name: {
    const VarSymbol *var = cast<NameExpr>(e)->var;
    return var && storeVars_.contains(var);
  }
  case Expr::Kind::Conditional:
    return isStore(cast<ConditionalExpr>(e)->thenExpr) ||
           isStore(cast<ConditionalExpr>(e)->elseExpr);
  case Expr::Kind::Binary: {
    auto *binary = cast<BinaryExpr>(e);
    bool picks = binary->op == BinaryOp::LogOr || binary->op == BinaryOp::LogAnd ||
                 binary->op == BinaryOp::Nullish;
    return picks && (isStore(binary->lhs) || isStore(binary->rhs));
  }
  case Expr::Kind::Assign: {
    auto *a = cast<AssignExpr>(e);
    return !a->op && isStore(a->value);
  }
  default:
    return false;
  }
}

void ContextChecker::keep(const Expr *value, const llvm::Twine &where) {
  if (!report_ || !value || !reported_.insert(value->loc().getPointer()).second) {
    return;
  }
  program_.diags->error(value->loc(), "the store of an AsyncLocalStorage lives only inside "
                                      "'run', so it can't be kept " +
                                          where + ": copy the fields you need instead " + kSection);
}

void ContextChecker::markParam(const FunctionDecl *fn, unsigned index) {
  if (index < fn->params.size()) {
    storeParams_.insert({fn, index});
  }
}

void ContextChecker::checkNews() {
  for (const NewExpr *n : news_) {
    if (allowedNews_.contains(n)) {
      continue;
    }
    program_.diags->error(n->loc(), "an AsyncLocalStorage is made only as a 'const' at the top "
                                    "level of a module, e.g. 'export const requestContext = new "
                                    "AsyncLocalStorage<RequestContext>()' " +
                                        kSection);
  }
}

void ContextChecker::checkRuns() {
  for (const CallExpr *c : runs_) {
    if (c->args.empty() || !parallel::holdsReferences(c->args[0]->type)) {
      continue;
    }
    const Expr *store = bare(c->args[0]);
    if (isa<ObjectLitExpr, ArrayLitExpr, NewExpr, UndefinedExpr>(store)) {
      continue;
    }
    program_.diags->error(store->loc(), "the store given to 'run' is a new object written right "
                                        "here, e.g. 'run({ requestId }, ...)': an object that "
                                        "already exists could get into two runs at once " +
                                            kSection);
  }
}

const FunctionDecl *ContextChecker::reader(const FunctionDecl *fn) const {
  llvm::DenseMap<const FunctionDecl *, const FunctionDecl *> via;
  for (const FunctionDecl *f : readsDirectly_) {
    via[f] = f;
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &[f, targets] : callees_) {
      if (via.count(f)) {
        continue;
      }
      for (const FunctionDecl *g : targets) {
        if (auto found = via.find(g); found != via.end()) {
          via[f] = found->second;
          grew = true;
          break;
        }
      }
    }
  }
  auto found = via.find(fn);

  return found == via.end() ? nullptr : found->second;
}

void ContextChecker::checkIntervals() {
  for (const CallExpr *c : intervals_) {
    const FunctionDecl *fn = c->args.empty() ? nullptr : callbackOf(c->args[0]);
    const FunctionDecl *reads = fn ? reader(fn) : nullptr;
    if (!reads) {
      continue;
    }
    std::string how = reads == fn || reads->name.empty()
                          ? std::string("this function reads it with getStore()")
                          : "this function reads it through '" + reads->name.str() + "'";
    if (c->builtin == Builtin::SetInterval) {
      program_.diags->error(c->args[0]->loc(),
                            "setInterval does not carry the store of an AsyncLocalStorage (an "
                            "interval outlives 'run'), but " +
                                how +
                                "; read what it needs before setInterval and keep it in the "
                                "interval's object " +
                                kSection);
    } else {
      program_.diags->error(c->args[0]->loc(),
                            "a setTimeout that restarts itself does not carry the store of an "
                            "AsyncLocalStorage (like an interval, it outlives 'run'), but " +
                                how + "; read what it needs before and keep it in a field " +
                                kSection);
    }
  }
}

const ClassDecl *ContextChecker::storageClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic ? cls->generic : cls;
    if ((base->library || cls->library) && base->name == "AsyncLocalStorage") {
      return base;
    }
  }

  return nullptr;
}

bool ContextChecker::storageMethod(const CallExpr *c, llvm::StringRef name) {
  const FunctionDecl *decl = c->method ? c->method->decl : nullptr;

  return decl && decl->name == name && storageClass(decl->cls);
}

const Expr *ContextChecker::bare(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

const FunctionDecl *ContextChecker::callbackOf(const Expr *e) {
  e = bare(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    return fe->fn;
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
    if (name->func) {
      return name->func->decl;
    }
    if (name->var) {
      return name->var->lambda ? name->var->lambda : name->var->function;
    }
    return nullptr;
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(e); member && member->method) {
    return member->method->decl;
  }

  return nullptr;
}

const FunctionDecl *ContextChecker::programTarget(const CallExpr *c) const {
  if (c->builtin != Builtin::None) {
    return nullptr;
  }
  const FunctionDecl *decl = c->func ? c->func->decl : c->method ? c->method->decl : nullptr;
  if (!decl) {
    if (auto *name = dyn_cast_or_null<NameExpr>(c->callee);
        name && name->var && !name->var->reassigned) {
      decl = name->var->lambda ? name->var->lambda : name->var->function;
    }
  }
  if (!decl || !decl->body || inLibrary(decl->loc())) {
    return nullptr;
  }

  return decl;
}

}
