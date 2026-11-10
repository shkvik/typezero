#include "compiler/interprocedural/held_value_checker/held_value_checker.h"
#include "compiler/interprocedural/held_value_checker/held_value_checker_errors.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/module/module.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

HeldValueChecker::HeldValueChecker(Program &checked, const LibraryRanges &library)
    : program(checked),
      library(library),
      current(nullptr),
      reporting(false) {}

void HeldValueChecker::childrenOf(const Expr *e, const std::function<void(const Expr *)> &visit) {
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

std::string HeldValueChecker::calleeName(const CallExpr *c) {
  const Expr *callee = c->callee;
  if (auto *member = dyn_cast_or_null<MemberExpr>(callee)) {
    return member->name.str();
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(callee)) {
    return name->name.str();
  }

  return "";
}

bool HeldValueChecker::produces(const CallExpr *) const {
  return false;
}

bool HeldValueChecker::takesOver(const CallExpr *) {
  return false;
}

void HeldValueChecker::sawNew(const NewExpr *) {}

void HeldValueChecker::sawVoid(const CallExpr *) {}

void HeldValueChecker::sawCall(const CallExpr *) {}

bool HeldValueChecker::heldMember(const MemberExpr *) const {
  return false;
}

void HeldValueChecker::spreadKept(const Expr *) {}

size_t HeldValueChecker::extraKnown() const {
  return 0;
}

bool HeldValueChecker::closuresCarry() const {
  return false;
}

void HeldValueChecker::finish() {}

void HeldValueChecker::startWalk() {}

bool HeldValueChecker::usesModule(llvm::StringRef name) const {
  return llvm::any_of(this->program.file.modules,
                      [&](const Module *m) { return m->builtin && m->name == name; });
}

bool HeldValueChecker::inLibrary(llvm::SMLoc loc) const {
  return this->library.contains(loc);
}

void HeldValueChecker::run() {
  if (!this->used()) {
    return;
  }
  auto known = [&] {
    return this->heldVars.size() + this->heldParams.size() + this->returnsHeld.size() +
           this->heldThis.size() + this->extraKnown();
  };
  for (int round = 0; round < 32; ++round) {
    size_t before = known();
    this->walkProgram();
    if (known() == before) {
      break;
    }
  }
  this->reporting = true;
  this->walkProgram();
  this->finish();
}

void HeldValueChecker::walkProgram() {
  this->walked.clear();
  this->callees.clear();
  this->allowedNews.clear();
  this->awaited.clear();
  this->allowedStores.clear();
  this->callSites.clear();
  this->startWalk();
  SourceFile &file = this->program.file;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        this->function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic() ? !cls->generic() : cls->isEmitted())) {
          this->function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          this->function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          this->function(fn);
        }
        this->function(cls->init);
        for (const ClassField &f : cls->fields) {
          this->function(f.method());
        }
      }
    }
    this->topLevel(item);
  }
  for (const FunctionDecl *fn : file.inventory.instances) {
    this->function(fn);
  }
}

void HeldValueChecker::function(const FunctionDecl *fn) {
  if (!fn || !fn->body || this->inLibrary(fn->loc()) || !this->walked.insert(fn).second) {
    return;
  }
  const FunctionDecl *saved = this->current;
  this->current = fn;
  if (fn->thisVar() && this->heldThis.contains(fn)) {
    this->heldVars.insert(fn->thisVar());
  }
  for (unsigned i = 0; i < fn->params.size(); ++i) {
    const Param &param = fn->params[i];
    if (param.sym && this->heldParams.count({fn, i})) {
      this->heldVars.insert(param.sym);
    }
    this->expr(param.defaultValue());
  }
  this->stmt(fn->body);
  this->current = saved;
}

void HeldValueChecker::topLevel(const Stmt *s) {
  if (!s || this->inLibrary(s->loc())) {
    return;
  }
  this->current = nullptr;
  if (auto *decl = dyn_cast<VarDeclStmt>(s); decl && decl->isConst) {
    for (const VarDeclarator &d : decl->decls) {
      if (!d.destructured()) {
        this->allowedNews.insert(HeldValueChecker::bare(d.init));
      }
    }
  }
  this->stmt(s);
}

void HeldValueChecker::stmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
    case Stmt::Kind::Block:
      if (NestedGroup *group = cast<BlockStmt>(s)->group) {
        for (const FunctionDecl *member : group->members) {
          this->function(member);
        }
      }
      for (const Stmt *x : cast<BlockStmt>(s)->body) {
        this->stmt(x);
      }
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls) {
        this->expr(d.init);
        if (!d.sym || d.destructured() || !this->isHeld(d.init)) {
          continue;
        }
        if (d.sym->isGlobal()) {
          std::string where = ErrorTextHeldPlace::moduleVar();
          this->keep(d.init, where);
        } else {
          this->heldVars.insert(d.sym);
        }
      }
      break;
    case Stmt::Kind::Function:
      this->function(cast<FunctionDecl>(s));
      break;
    case Stmt::Kind::Expr:
      this->expr(cast<ExprStmt>(s)->expr);
      break;
    case Stmt::Kind::If:
      this->expr(cast<IfStmt>(s)->cond);
      this->stmt(cast<IfStmt>(s)->thenStmt);
      this->stmt(cast<IfStmt>(s)->elseStmt);
      break;
    case Stmt::Kind::While:
      this->expr(cast<WhileStmt>(s)->cond);
      this->stmt(cast<WhileStmt>(s)->body);
      break;
    case Stmt::Kind::DoWhile:
      this->stmt(cast<DoWhileStmt>(s)->body);
      this->expr(cast<DoWhileStmt>(s)->cond);
      break;
    case Stmt::Kind::For:
      this->stmt(cast<ForStmt>(s)->init);
      this->expr(cast<ForStmt>(s)->cond);
      this->expr(cast<ForStmt>(s)->step);
      this->stmt(cast<ForStmt>(s)->body);
      break;
    case Stmt::Kind::ForOf:
      this->expr(cast<ForOfStmt>(s)->iterable);
      this->stmt(cast<ForOfStmt>(s)->body);
      break;
    case Stmt::Kind::Switch:
      this->expr(cast<SwitchStmt>(s)->discriminant);
      for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
        this->expr(c.test);
        for (const Stmt *x : c.body) {
          this->stmt(x);
        }
      }
      break;
    case Stmt::Kind::Labeled:
      this->stmt(cast<LabeledStmt>(s)->body);
      break;
    case Stmt::Kind::Return: {
      const Expr *value = cast<ReturnStmt>(s)->value;
      this->awaited.insert(HeldValueChecker::bare(value));
      this->expr(value);
      if (this->current && this->isHeld(value)) {
        this->returnsHeld.insert(this->current);
      }
      break;
    }
    case Stmt::Kind::Class: {
      auto *cls = cast<ClassDecl>(s);
      for (const ClassField &f : cls->fields) {
        if (f.isStatic()) {
          this->expr(f.init);
        }
      }
      for (const Stmt *x : cls->decorations) {
        this->stmt(x);
      }
      break;
    }
    default:
      break;
  }
}

void HeldValueChecker::expr(const Expr *e) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
    case Expr::Kind::Function:
      this->function(cast<FunctionExpr>(e)->fn);
      return;
    case Expr::Kind::Await:
      this->awaited.insert(HeldValueChecker::bare(cast<AwaitExpr>(e)->value));
      this->expr(cast<AwaitExpr>(e)->value);
      return;
    case Expr::Kind::Call:
      this->call(cast<CallExpr>(e));
      return;
    case Expr::Kind::New:
      this->create(cast<NewExpr>(e));
      return;
    case Expr::Kind::Assign:
      this->assign(cast<AssignExpr>(e));
      return;
    case Expr::Kind::Unary: {
      auto *unary = cast<UnaryExpr>(e);
      this->expr(unary->operand);
      if (auto *c = dyn_cast_or_null<CallExpr>(HeldValueChecker::bare(unary->operand));
          c && unary->op == UnaryOp::Void) {
        this->sawVoid(c);
      }
      return;
    }
    case Expr::Kind::ObjectLit:
      for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
        this->expr(p.key());
        this->expr(p.value);
        if (p.spread() && this->isHeld(p.value)) {
          this->spreadKept(p.value);
        } else if (!p.spread() && this->isHeld(p.value) && !this->allowedStores.contains(e)) {
          std::string where = ErrorTextHeldPlace::newObject();
          this->keep(p.value, where);
        }
      }
      return;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &x : cast<ArrayLitExpr>(e)->elements) {
        this->expr(x.expr);
        if (!x.spread && this->isHeld(x.expr)) {
          std::string where = ErrorTextHeldPlace::array();
          this->keep(x.expr, where);
        }
      }
      return;
    default:
      HeldValueChecker::childrenOf(e, [&](const Expr *x) { this->expr(x); });
      return;
  }
}

void HeldValueChecker::call(const CallExpr *c) {
  this->sawCall(c);
  this->expr(c->callee);
  for (const Expr *arg : c->args) {
    this->expr(arg);
  }
  const FunctionDecl *target = this->programTarget(c);
  if (this->current && target) {
    this->callees[this->current].push_back(target);
  }
  if (target) {
    this->callSites[target].push_back(c);
    if (target->generic) {
      this->callSites[target->generic].push_back(c);
    }
  }
  bool over = this->takesOver(c);
  if (this->current && !over) {
    for (const Expr *arg : c->args) {
      if (auto *fe = dyn_cast<FunctionExpr>(HeldValueChecker::bare(arg))) {
        this->callees[this->current].push_back(fe->fn);
      }
    }
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(c->callee);
      member && target && c->method() && this->isHeld(member->object)) {
    this->markThis(target);
  }
  if (over) {
    return;
  }
  std::string name = HeldValueChecker::calleeName(c);
  this->passed(c, target, name);
}

void HeldValueChecker::passed(const CallExpr *c, const FunctionDecl *target, llvm::StringRef name) {
  for (unsigned i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    if (!this->isHeld(arg)) {
      continue;
    }
    if (target) {
      this->markParam(target, i);
      continue;
    }
    if (parallel::ParallelHelpers::onlyReadsArgs(c->builtin()) ||
        c->builtin() == Builtin::ResultOk || c->builtin() == Builtin::ResultErr) {
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(HeldValueChecker::bare(arg)); fe && fe->fn->nonEscaping) {
      continue;
    }
    std::string where =
        name.empty() ? ErrorTextHeldPlace::unknownFn() : ErrorTextHeldPlace::storedBy(name);
    this->keep(arg, where);
  }
}

void HeldValueChecker::create(const NewExpr *n) {
  const std::vector<Expr *> &args = n->ctor() ? n->ctor()->args : n->args;
  for (const Expr *arg : args) {
    this->expr(arg);
  }
  this->sawNew(n);
  const FunctionDecl *target = n->ctor() ? this->programTarget(n->ctor()) : nullptr;
  if (this->current && target) {
    this->callees[this->current].push_back(target);
  }
  if (this->allowedStores.contains(n)) {
    return;
  }
  for (unsigned i = 0; i < args.size(); ++i) {
    if (!this->isHeld(args[i])) {
      continue;
    }
    if (target) {
      this->markParam(target, i);
    } else {
      std::string where = ErrorTextHeldPlace::newObject();
      this->keep(args[i], where);
    }
  }
}

void HeldValueChecker::assign(const AssignExpr *a) {
  this->expr(a->target);
  this->expr(a->value);
  if (a->op || !this->isHeld(a->value)) {
    return;
  }
  const Expr *target = HeldValueChecker::bare(a->target);
  if (auto *name = dyn_cast<NameExpr>(target); name && name->var()) {
    const VarSymbol *var = name->var();
    if (var->isGlobal()) {
      std::string where = ErrorTextHeldPlace::moduleVar();
      this->keep(a->value, where);
    } else if (var->owner() && var->owner() != this->current) {
      std::string where = ErrorTextHeldPlace::outerVar();
      this->keep(a->value, where);
    } else {
      this->heldVars.insert(var);
    }
    return;
  }
  std::string where =
      isa<IndexExpr>(target) ? ErrorTextHeldPlace::container() : ErrorTextHeldPlace::field();
  this->keep(a->value, where);
}

bool HeldValueChecker::isHeld(const Expr *e) const {
  e = HeldValueChecker::bare(e);
  if (!e) {
    return false;
  }
  switch (e->kind()) {
    case Expr::Kind::Call: {
      auto *c = cast<CallExpr>(e);
      if (this->produces(c)) {
        return true;
      }
      if ((c->builtin() == Builtin::ResultOk || c->builtin() == Builtin::ResultErr) &&
          !c->args.empty()) {
        return this->isHeld(c->args[0]);
      }
      const FunctionDecl *target = this->programTarget(c);
      return target && this->returnsHeld.contains(target);
    }
    case Expr::Kind::Name: {
      const VarSymbol *var = cast<NameExpr>(e)->var();
      return var && this->heldVars.contains(var);
    }
    case Expr::Kind::Member: {
      auto *member = cast<MemberExpr>(e);
      const Type *object =
          member->object && member->object->type ? member->object->type->nonOptional() : nullptr;
      if (this->heldMember(member)) {
        return true;
      }
      return object && object->isResult() && (member->name == "value" || member->name == "error") &&
             this->isHeld(member->object);
    }
    case Expr::Kind::OptionalChain:
      return this->isHeld(cast<OptionalChainExpr>(e)->chain);
    case Expr::Kind::Function:
      return this->closuresCarry() && this->carries(cast<FunctionExpr>(e)->fn);
    case Expr::Kind::Conditional:
      return this->isHeld(cast<ConditionalExpr>(e)->thenExpr) ||
             this->isHeld(cast<ConditionalExpr>(e)->elseExpr);
    case Expr::Kind::Binary: {
      auto *binary = cast<BinaryExpr>(e);
      bool picks = binary->op == BinaryOp::LogOr || binary->op == BinaryOp::LogAnd ||
                   binary->op == BinaryOp::Nullish;
      return picks && (this->isHeld(binary->lhs) || this->isHeld(binary->rhs));
    }
    case Expr::Kind::Assign: {
      auto *a = cast<AssignExpr>(e);
      return !a->op && this->isHeld(a->value);
    }
    default:
      return false;
  }
}

bool HeldValueChecker::carries(const FunctionDecl *fn) const {
  if (!fn) {
    return false;
  }
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;

  return llvm::any_of(captures, [&](const VarSymbol *v) { return this->heldVars.contains(v); });
}

void HeldValueChecker::keep(const Expr *value, llvm::StringRef where) {
  if (!this->reporting || !value || !this->reported.insert(value->loc().getPointer()).second) {
    return;
  }
  const Type *type = value->type ? value->type->nonOptional() : nullptr;
  this->kept(value, type && type->isFunction(), where);
}

bool HeldValueChecker::once(const Expr *value) {
  return this->reporting && value && this->reported.insert(value->loc().getPointer()).second;
}

void HeldValueChecker::markParam(const FunctionDecl *fn, unsigned index) {
  if (fn && index < fn->params.size()) {
    this->heldParams.insert({fn, index});
  }
}

void HeldValueChecker::markThis(const FunctionDecl *fn) {
  if (!fn || !fn->thisVar() || fn->isStatic()) {
    return;
  }
  this->heldThis.insert(fn);
  if (!fn->cls()) {
    return;
  }
  std::vector<const ClassDecl *> pending(fn->cls()->children.begin(), fn->cls()->children.end());
  while (!pending.empty()) {
    const ClassDecl *cls = pending.back();
    pending.pop_back();
    for (const FunctionDecl *method : cls->methods) {
      for (const FunctionDecl *up = method->overrides(); up; up = up->overrides()) {
        if (up == fn) {
          this->heldThis.insert(method);
          break;
        }
      }
    }
    pending.insert(pending.end(), cls->children.begin(), cls->children.end());
  }
}

const Expr *HeldValueChecker::bare(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

const FunctionDecl *HeldValueChecker::callbackOf(const Expr *e) {
  e = HeldValueChecker::bare(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    return fe->fn;
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
    if (name->func()) {
      return name->func()->decl;
    }
    if (name->var()) {
      return name->var()->lambda() ? name->var()->lambda() : name->var()->function();
    }
    return nullptr;
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(e); member && member->method()) {
    return member->method()->decl;
  }

  return nullptr;
}

const ClassDecl *HeldValueChecker::storageClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic() ? cls->generic() : cls;
    if ((base->library() || cls->library()) && base->name == "AsyncLocalStorage") {
      return base;
    }
  }

  return nullptr;
}

bool HeldValueChecker::storageMethod(const CallExpr *c, llvm::StringRef name) {
  const FunctionDecl *decl = c->method() ? c->method()->decl : nullptr;

  return decl && decl->name == name && HeldValueChecker::storageClass(decl->cls());
}

const VarSymbol *HeldValueChecker::storageOf(const CallExpr *c) {
  auto *member = dyn_cast_or_null<MemberExpr>(c->callee);
  auto *name =
      member ? dyn_cast_or_null<NameExpr>(HeldValueChecker::bare(member->object)) : nullptr;

  return name ? name->var() : nullptr;
}

const FunctionDecl *HeldValueChecker::programTarget(const CallExpr *c) const {
  if (c->builtin() != Builtin::None) {
    return nullptr;
  }
  const FunctionDecl *decl = c->func()     ? c->func()->decl
                             : c->method() ? c->method()->decl
                                           : nullptr;
  if (!decl) {
    if (auto *name = dyn_cast_or_null<NameExpr>(c->callee);
        name && name->var() && !name->var()->reassigned) {
      decl = name->var()->lambda() ? name->var()->lambda() : name->var()->function();
    }
  }
  if (!decl || !decl->body || this->inLibrary(decl->loc())) {
    return nullptr;
  }

  return decl;
}

}
