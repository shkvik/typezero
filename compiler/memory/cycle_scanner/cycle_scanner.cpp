#include "compiler/memory/cycle_scanner/cycle_scanner.h"
#include "compiler/memory/cycle_escapes/cycle_escapes.h"
#include "compiler/memory/cycle_exprs/cycle_exprs.h"

namespace tz {

void CycleScanner::addType(const Type *type) {
  if (!type || !seenTypes.insert(type).second) {
    return;
  }
  types.push_back(type);
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
  case Type::Kind::Iterator:
    addType(type->inner());
    break;
  case Type::Kind::Function:
    for (const Type *p : type->params()) {
      addType(p);
    }
    addType(type->ret());
    break;
  case Type::Kind::Union:
    for (const Type *m : type->members()) {
      addType(m);
    }
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
    for (const Field &f : type->fields()) {
      addType(f.type);
    }
    for (const Type *b : type->bases()) {
      addType(b);
    }
    if (const ClassDecl *cls = type->classDecl(); cls && cls->group) {
      for (const VarSymbol *var : cls->group->captures) {
        addVar(var);
      }
    }
    break;
  default:
    break;
  }
}

void CycleScanner::addVar(const VarSymbol *var) {
  if (!var) {
    return;
  }
  addType(var->type);
  if (var->boxed()) {
    boxed.insert(var);
  }
}

void CycleScanner::function(const FunctionDecl *fn) {
  if (!fn || !scanned_.insert(fn).second) {
    return;
  }
  if (fn->sym) {
    for (const Type *p : fn->sym->params) {
      addType(p);
    }
    addType(fn->sym->ret);
  }
  for (const Param &param : fn->params) {
    addVar(param.sym);
    cycleExprs().expr(param.defaultValue);
  }
  addVar(fn->thisVar);
  for (const VarSymbol *var : fn->captures) {
    addVar(var);
  }
  const FunctionDecl *saved = current_;
  current_ = fn;
  stmt(fn->body);
  current_ = saved;
}

void CycleScanner::classDecl(const ClassDecl *cls) {
  addType(cls->type);
  for (const ClassField &f : cls->fields) {
    addType(f.resolved);
    if (f.isStatic && f.var) {
      addVar(f.var);
      cycleExprs().expr(f.init);
    }
  }
  for (const Stmt *s : cls->decorations) {
    stmt(s);
  }
}

void CycleScanner::stmt(const Stmt *stmt) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block: {
    auto *block = llvm::cast<BlockStmt>(stmt);
    if (NestedGroup *group = block->group) {
      envs[group].assign(group->captures.begin(), group->captures.end());
      for (const VarSymbol *var : group->captures) {
        addVar(var);
      }
      for (const FunctionDecl *member : group->members) {
        if (member->sym) {
          closuresOf[functionType(member)].insert(group);
        }
        function(member);
      }
    }
    for (const Stmt *s : block->body) {
      this->stmt(s);
    }
    break;
  }
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : llvm::cast<VarDeclStmt>(stmt)->decls) {
      addVar(d.sym);
      cycleExprs().expr(d.init);
    }
    break;
  case Stmt::Kind::Function:
    function(llvm::cast<FunctionDecl>(stmt));
    break;
  case Stmt::Kind::Expr:
    cycleExprs().expr(llvm::cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = llvm::cast<IfStmt>(stmt);
    cycleExprs().expr(s->cond);
    this->stmt(s->thenStmt);
    this->stmt(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    cycleExprs().expr(llvm::cast<WhileStmt>(stmt)->cond);
    this->stmt(llvm::cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    this->stmt(llvm::cast<DoWhileStmt>(stmt)->body);
    cycleExprs().expr(llvm::cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = llvm::cast<ForStmt>(stmt);
    this->stmt(s->init);
    cycleExprs().expr(s->cond);
    cycleExprs().expr(s->step);
    this->stmt(s->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = llvm::cast<ForOfStmt>(stmt);
    addVar(s->binding.sym);
    cycleExprs().expr(s->iterable);
    this->stmt(s->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = llvm::cast<SwitchStmt>(stmt);
    cycleExprs().expr(s->discriminant);
    for (const SwitchCase &c : s->cases) {
      cycleExprs().expr(c.test);
      for (const Stmt *body : c.body) {
        this->stmt(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    this->stmt(llvm::cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    cycleExprs().expr(llvm::cast<ReturnStmt>(stmt)->value);
    break;
  case Stmt::Kind::Throw:
    cycleExprs().expr(llvm::cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = llvm::cast<TryStmt>(stmt);
    this->stmt(s->block);
    addVar(s->param.sym);
    this->stmt(s->handler);
    this->stmt(s->finalizer);
    break;
  }
  case Stmt::Kind::Class:
    classDecl(llvm::cast<ClassDecl>(stmt));
    break;
  default:
    break;
  }
}

const Type *CycleScanner::functionType(const FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;

  return Type::function(sym->params, sym->minArgs, sym->hasRest, sym->ret, sym->hasThis);
}

bool CycleScanner::thisEscapes(const Stmt *stmt, const VarSymbol *self) {
  bool escapes = false;
  std::function<void(const Expr *)> visit = [&](const Expr *e) {
    if (!e || escapes) {
      return;
    }
    if (auto *call = llvm::dyn_cast<CallExpr>(e)) {
      auto *member = llvm::dyn_cast<MemberExpr>(call->callee);
      auto *object = member ? llvm::dyn_cast<NameExpr>(member->object) : nullptr;
      if ((call->superCall && member) ||
          (object && object->var == self && (call->method || !call->unionMethods.empty()))) {
        escapes = true;
        return;
      }
    }
    if (auto *member = llvm::dyn_cast<MemberExpr>(e)) {
      auto *object = llvm::dyn_cast<NameExpr>(member->object);
      if (object && object->var == self && !member->method) {
        return;
      }
      visit(member->object);
      return;
    }
    if (auto *name = llvm::dyn_cast<NameExpr>(e)) {
      escapes = name->var == self;
      return;
    }
    if (auto *fe = llvm::dyn_cast<FunctionExpr>(e)) {
      escapes = llvm::is_contained(fe->fn->captures, self) ||
                CycleEscapes::mentionsIn(fe->fn->body, self);
      return;
    }
    CycleEscapes::forEachChild(e, visit);
  };
  CycleEscapes::walkStmt(stmt, visit);

  return escapes;
}

CycleScanner::CycleScanner() {
  cycleExprsPart_ = std::make_unique<CycleExprs>(*this);
  cycleEscapesPart_ = std::make_unique<CycleEscapes>(*this);
}

CycleScanner::~CycleScanner() = default;
}
