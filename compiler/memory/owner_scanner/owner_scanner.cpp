#include "compiler/memory/owner_scanner/owner_scanner.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::string utf8Of(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

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

void OwnerScanner::function(const FunctionDecl *fn) {
  if (!fn || !scanned_.insert(fn).second) {
    return;
  }
  const FunctionDecl *saved = current_;
  current_ = fn;
  for (const Param &param : fn->params) {
    expr(param.defaultValue, Use::Value);
  }
  stmt(fn->body);
  current_ = saved;
}

void OwnerScanner::topLevel(const Stmt *s) {
  current_ = nullptr;
  stmt(s);
}

std::vector<OwnerScanner::BackRef>
OwnerScanner::proven(const std::function<bool(const ClassDecl *)> &escapes) const {
  std::vector<BackRef> out;
  for (const BackRef &b : candidates_) {
    if (valueReads_.contains(b.field) || nonCallReads_.contains(b.callback) ||
        bad_.contains(b.field)) {
      continue;
    }
    auto writes = writes_.find(b.field);
    if (writes == writes_.end()) {
      continue;
    }
    const ClassDecl *made = nullptr;
    bool fresh = true;
    for (const Write &w : writes->second) {
      if (!w.made || !w.cls || !w.cls->derivesFrom(b.owner) || (made && w.made != made)) {
        fresh = false;
      }
      made = w.made;
    }
    if (!fresh || !made || made->builtin || !made->type->field(b.callback)) {
      continue;
    }
    bool keeps = true;
    for (const ClassDecl *c = made; c && keeps; c = c->parent) {
      keeps = !escapes(c);
    }
    if (keeps) {
      out.push_back(b);
    }
  }

  return out;
}

const ClassDecl *OwnerScanner::madeBy(llvm::StringRef field) const {
  auto writes = writes_.find(field);

  return writes == writes_.end() || writes->second.empty() ? nullptr : writes->second.front().made;
}

void OwnerScanner::stmt(const Stmt *stmt) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    if (NestedGroup *group = llvm::cast<BlockStmt>(stmt)->group) {
      for (const FunctionDecl *member : group->members) {
        function(member);
      }
    }
    for (const Stmt *s : llvm::cast<BlockStmt>(stmt)->body) {
      this->stmt(s);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : llvm::cast<VarDeclStmt>(stmt)->decls) {
      expr(d.init, Use::Value);
    }
    break;
  case Stmt::Kind::Function:
    function(llvm::cast<FunctionDecl>(stmt));
    break;
  case Stmt::Kind::Expr:
    expr(llvm::cast<ExprStmt>(stmt)->expr, Use::Value);
    break;
  case Stmt::Kind::If:
    expr(llvm::cast<IfStmt>(stmt)->cond, Use::Value);
    this->stmt(llvm::cast<IfStmt>(stmt)->thenStmt);
    this->stmt(llvm::cast<IfStmt>(stmt)->elseStmt);
    break;
  case Stmt::Kind::While:
    expr(llvm::cast<WhileStmt>(stmt)->cond, Use::Value);
    this->stmt(llvm::cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    this->stmt(llvm::cast<DoWhileStmt>(stmt)->body);
    expr(llvm::cast<DoWhileStmt>(stmt)->cond, Use::Value);
    break;
  case Stmt::Kind::For:
    this->stmt(llvm::cast<ForStmt>(stmt)->init);
    expr(llvm::cast<ForStmt>(stmt)->cond, Use::Value);
    expr(llvm::cast<ForStmt>(stmt)->step, Use::Value);
    this->stmt(llvm::cast<ForStmt>(stmt)->body);
    break;
  case Stmt::Kind::ForOf:
    expr(llvm::cast<ForOfStmt>(stmt)->iterable, Use::Value);
    this->stmt(llvm::cast<ForOfStmt>(stmt)->body);
    break;
  case Stmt::Kind::Switch:
    expr(llvm::cast<SwitchStmt>(stmt)->discriminant, Use::Value);
    for (const SwitchCase &c : llvm::cast<SwitchStmt>(stmt)->cases) {
      expr(c.test, Use::Value);
      for (const Stmt *s : c.body) {
        this->stmt(s);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    this->stmt(llvm::cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    expr(llvm::cast<ReturnStmt>(stmt)->value, Use::Value);
    break;
  case Stmt::Kind::Throw:
    expr(llvm::cast<ThrowStmt>(stmt)->value, Use::Value);
    break;
  case Stmt::Kind::Try:
    this->stmt(llvm::cast<TryStmt>(stmt)->block);
    this->stmt(llvm::cast<TryStmt>(stmt)->handler);
    this->stmt(llvm::cast<TryStmt>(stmt)->finalizer);
    break;
  case Stmt::Kind::Class: {
    auto *cls = llvm::cast<ClassDecl>(stmt);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic) {
        expr(f.init, Use::Value);
      }
    }
    for (const Stmt *s : cls->decorations) {
      this->stmt(s);
    }
    break;
  }
  default:
    break;
  }
}

void OwnerScanner::expr(const Expr *e, Use use) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::Cast:
    expr(llvm::cast<CastExpr>(e)->operand,
         llvm::cast<CastExpr>(e)->isImplicit() ? use : Use::Value);
    return;
  case Expr::Kind::Unary: {
    auto *unary = llvm::cast<UnaryExpr>(e);
    expr(unary->operand, unary->op == UnaryOp::NonNull ? use : Use::Value);
    return;
  }
  case Expr::Kind::OptionalChain:
    expr(llvm::cast<OptionalChainExpr>(e)->chain, use);
    return;
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(e);
    if (use != Use::Callee) {
      nonCallReads_.insert(member->name);
    }
    if (use == Use::Value) {
      valueReads_.insert(member->name);
    }
    bool binds = member->method ||
                 (member->type && member->type->nonOptional()->isFunction() && use == Use::Value);
    expr(member->object, binds ? Use::Value : Use::Receiver);
    return;
  }
  case Expr::Kind::Index: {
    auto *index = llvm::cast<IndexExpr>(e);
    if (!index->field.empty()) {
      readAll(index->field);
    }
    if (index->keyed && index->index->type) {
      const Type *key = index->index->type;
      for (const Type *n : key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key)) {
        if (n->isStringLiteral()) {
          readAll(utf8Of(n->literalText()));
        }
      }
    }
    expr(index->object, index->field.empty() && !index->keyed ? Use::Receiver : Use::Value);
    expr(index->index, Use::Value);
    return;
  }
  case Expr::Kind::Call: {
    auto *call = llvm::cast<CallExpr>(e);
    auto *member = llvm::dyn_cast<MemberExpr>(bare(call->callee));
    if (member && (call->method || call->builtin != Builtin::None || call->superCall ||
                   !call->unionMethods.empty())) {
      expr(member->object, Use::Receiver);
    } else {
      expr(call->callee, Use::Callee);
    }
    for (const Expr *arg : call->args) {
      expr(arg, Use::Value);
    }
    if ((call->builtin == Builtin::ObjectValues || call->builtin == Builtin::ObjectEntries) &&
        !call->args.empty()) {
      readFields(call->args[0]->type);
    }
    return;
  }
  case Expr::Kind::Assign: {
    auto *assign = llvm::cast<AssignExpr>(e);
    expr(assign->value, Use::Value);
    if (auto *member = llvm::dyn_cast<MemberExpr>(assign->target)) {
      wrote(member, assign);
      expr(member->object, Use::Receiver);
      if (assign->op) {
        nonCallReads_.insert(member->name), valueReads_.insert(member->name);
      }
    } else if (auto *index = llvm::dyn_cast<IndexExpr>(assign->target)) {
      if (!index->field.empty()) {
        bad_.insert(index->field);
      }
      expr(index->object, Use::Receiver);
      expr(index->index, Use::Value);
    } else {
      expr(assign->target, Use::Value);
    }
    return;
  }
  case Expr::Kind::Update: {
    auto *update = llvm::cast<UpdateExpr>(e);
    if (auto *member = llvm::dyn_cast<MemberExpr>(update->target)) {
      expr(member->object, Use::Receiver);
    } else {
      expr(update->target, Use::Value);
    }
    return;
  }
  case Expr::Kind::Function:
    function(llvm::cast<FunctionExpr>(e)->fn);
    return;
  case Expr::Kind::ObjectLit: {
    auto *lit = llvm::cast<ObjectLitExpr>(e);
    for (const ObjectLitExpr::Property &p : lit->props) {
      expr(p.key, Use::Value);
      if (!p.method) {
        expr(p.value, Use::Value);
      }
      if (p.spread && p.value) {
        readFields(p.value->type);
      }
    }
    if (lit->cls) {
      for (const FunctionDecl *fn : lit->cls->methods) {
        function(fn);
      }
    }
    return;
  }
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(e);
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      expr(arg, Use::Value);
    }
    return;
  }
  default:
    break;
  }
  visitChildren(e, [&](const Expr *child) { expr(child, Use::Value); });
}

void OwnerScanner::readAll(llvm::StringRef name) {
  valueReads_.insert(name);
  nonCallReads_.insert(name);
}

void OwnerScanner::readFields(const Type *type) {
  if (!type) {
    return;
  }
  type = type->nonOptional();
  if (type->isUnion()) {
    for (const Type *m : type->members()) {
      readFields(m);
    }
    return;
  }
  if (type->isObject()) {
    for (const Field &f : type->fields()) {
      readAll(f.name);
    }
  }
}

void OwnerScanner::wrote(const MemberExpr *target, const AssignExpr *assign) {
  const FunctionDecl *fn = current_;
  auto *object = llvm::dyn_cast<NameExpr>(bare(target->object));
  bool self = fn && fn->thisVar && object && object->var == fn->thisVar;
  bool construction = self && fn->cls && (fn->isConstructor || fn == fn->cls->init);
  const ClassDecl *made = nullptr;
  if (auto *create = llvm::dyn_cast<NewExpr>(bare(assign->value));
      create && create->cls && construction) {
    bool holdsSelf = false;
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      holdsSelf = holdsSelf || mentions(arg, fn->thisVar);
    }
    if (!holdsSelf) {
      made = create->cls;
    }
  }
  if (assign->op) {
    made = nullptr;
  }
  writes_[target->name].push_back({construction ? fn->cls : nullptr, made});
  auto *owned = llvm::dyn_cast<MemberExpr>(bare(target->object));
  auto *fe = llvm::dyn_cast<FunctionExpr>(bare(assign->value));
  if (!owned || !fe || fe->fn->nonEscaping || fe->fn->isAsync || fe->fn->isGenerator || !fn ||
      !fn->thisVar || !fn->cls) {
    return;
  }
  auto *holder = llvm::dyn_cast<NameExpr>(bare(owned->object));
  if (holder && holder->var == fn->thisVar && llvm::is_contained(fe->fn->captures, fn->thisVar)) {
    candidates_.push_back({fn->cls, owned->name, target->name, fe->fn, fn->thisVar});
  }
}

bool OwnerScanner::mentions(const Expr *e, const VarSymbol *self) {
  bool found = false;
  std::function<void(const Expr *)> visit = [&](const Expr *x) {
    if (!x || found) {
      return;
    }
    if (auto *name = llvm::dyn_cast<NameExpr>(x)) {
      found = name->var == self;
      return;
    }
    if (auto *fe = llvm::dyn_cast<FunctionExpr>(x)) {
      found = llvm::is_contained(fe->fn->captures, self);
      return;
    }
    visitChildren(x, visit);
  };
  visit(e);

  return found;
}

void OwnerScanner::visitChildren(const Expr *e, const std::function<void(const Expr *)> &visit) {
  switch (e->kind()) {
  case Expr::Kind::Template:
    for (const Expr *x : llvm::cast<TemplateExpr>(e)->exprs) {
      visit(x);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &x : llvm::cast<ArrayLitExpr>(e)->elements) {
      visit(x.expr);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : llvm::cast<ObjectLitExpr>(e)->props) {
      visit(p.key);
      visit(p.value);
    }
    break;
  case Expr::Kind::Member:
    visit(llvm::cast<MemberExpr>(e)->object);
    break;
  case Expr::Kind::Index:
    visit(llvm::cast<IndexExpr>(e)->object);
    visit(llvm::cast<IndexExpr>(e)->index);
    break;
  case Expr::Kind::Unary:
    visit(llvm::cast<UnaryExpr>(e)->operand);
    break;
  case Expr::Kind::Binary:
    visit(llvm::cast<BinaryExpr>(e)->lhs);
    visit(llvm::cast<BinaryExpr>(e)->rhs);
    break;
  case Expr::Kind::Assign:
    visit(llvm::cast<AssignExpr>(e)->target);
    visit(llvm::cast<AssignExpr>(e)->value);
    break;
  case Expr::Kind::Update:
    visit(llvm::cast<UpdateExpr>(e)->target);
    break;
  case Expr::Kind::Call:
    visit(llvm::cast<CallExpr>(e)->callee);
    for (const Expr *x : llvm::cast<CallExpr>(e)->args) {
      visit(x);
    }
    break;
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(e);
    for (const Expr *x : create->ctor ? create->ctor->args : create->args) {
      visit(x);
    }
    break;
  }
  case Expr::Kind::Conditional:
    visit(llvm::cast<ConditionalExpr>(e)->cond);
    visit(llvm::cast<ConditionalExpr>(e)->thenExpr);
    visit(llvm::cast<ConditionalExpr>(e)->elseExpr);
    break;
  case Expr::Kind::Cast:
    visit(llvm::cast<CastExpr>(e)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *x : llvm::cast<SequenceExpr>(e)->exprs) {
      visit(x);
    }
    break;
  case Expr::Kind::OptionalChain:
    visit(llvm::cast<OptionalChainExpr>(e)->chain);
    break;
  case Expr::Kind::Yield:
    visit(llvm::cast<YieldExpr>(e)->value);
    break;
  case Expr::Kind::Await:
    visit(llvm::cast<AwaitExpr>(e)->value);
    break;
  default:
    break;
  }
}

}
