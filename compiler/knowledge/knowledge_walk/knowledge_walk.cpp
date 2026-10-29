#include "compiler/knowledge/knowledge_walk/knowledge_walk.h"
#include "compiler/knowledge/knowledge_pass/knowledge_pass.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/type/type.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

KnowledgeWalk::KnowledgeWalk(KnowledgePass &pass, size_t module, size_t from)
    : pass_(pass), module_(module), from_(from) {}

void KnowledgeWalk::param(const Param &param) {
  if (param.type) {
    typeRef(*param.type);
  }
  if (param.defaultValue) {
    expr(param.defaultValue);
  }
  for (const Decorator &decorator : param.decorators) {
    expr(decorator.expr);
  }
  const Type *type = param.sym && param.sym->type ? param.sym->type : param.declaredType;
  pass_.typed(module_, param.loc, param.name, type);
  pass_.local(param.sym, module_, from_, llvm::SMLoc());
}

void KnowledgeWalk::function(const FunctionDecl *fn) {
  if (!fn) {
    return;
  }
  for (const Decorator &decorator : fn->decorators) {
    expr(decorator.expr);
  }
  for (const TypeParamDecl &typeParam : fn->typeParams) {
    if (typeParam.constraint) {
      typeRef(*typeParam.constraint);
    }
  }
  for (const Param &each : fn->params) {
    param(each);
  }
  if (fn->returnType) {
    typeRef(*fn->returnType);
  }
  if (fn->body) {
    stmt(fn->body);
  }
}

void KnowledgeWalk::classDecl(const ClassDecl *cls) {
  if (!cls) {
    return;
  }
  for (const Decorator &decorator : cls->decorators) {
    expr(decorator.expr);
  }
  if (cls->base) {
    typeRef(*cls->base);
  }
  if (cls->baseExpr) {
    expr(cls->baseExpr);
  }
  for (const TypeRef &ref : cls->interfaces) {
    typeRef(ref);
  }
  size_t outer = from_;
  for (const ClassField &field : cls->fields) {
    if (field.parameter) {
      continue;
    }
    size_t own = pass_.fieldSymbol(cls, field.name, field.isStatic);
    from_ = own == Knowledge::kNone ? outer : own;
    for (const Decorator &decorator : field.decorators) {
      expr(decorator.expr);
    }
    if (field.type) {
      typeRef(*field.type);
    }
    if (field.init) {
      expr(field.init);
    }
    from_ = outer;
  }
  for (const auto *list : {&cls->constructors, &cls->methods}) {
    for (const FunctionDecl *method : *list) {
      size_t own = pass_.functionSymbol(method);
      from_ = own == Knowledge::kNone ? outer : own;
      function(method);
      from_ = outer;
    }
  }
}

void KnowledgeWalk::declarator(const VarDeclarator &declarator) {
  if (declarator.typeAnn) {
    typeRef(*declarator.typeAnn);
  }
  if (declarator.init) {
    expr(declarator.init);
  }
  if (!declarator.name.starts_with("%") && declarator.sym) {
    pass_.typed(module_, declarator.loc, declarator.name, declarator.sym->type);
    pass_.local(declarator.sym, module_, from_, llvm::SMLoc());
  }
}

void KnowledgeWalk::typeRef(const TypeRef &ref) {
  if ((ref.kind == TypeRef::Kind::Name || ref.kind == TypeRef::Kind::Generic) &&
      !ref.name.empty()) {
    size_t symbol = pass_.typeSymbol(module_, ref.name);
    if (symbol != Knowledge::kNone) {
      pass_.reference(symbol, from_, module_, ref.loc, false);
    }
  }
  for (const TypeRef &arg : ref.args) {
    typeRef(arg);
  }
}

bool KnowledgeWalk::use(const Expr *e, bool call) {
  if (auto *name = dyn_cast<NameExpr>(e)) {
    for (const TypeRef &arg : name->typeArgs) {
      typeRef(arg);
    }
    pass_.typed(module_, name->loc(), name->name, name->type);
    size_t symbol = Knowledge::kNone;
    if (name->var) {
      symbol = pass_.variableSymbol(name->var);
      if (symbol == Knowledge::kNone) {
        pass_.local(name->var, module_, from_, name->loc());
      }
    }
    if (symbol == Knowledge::kNone && name->func) {
      symbol = pass_.functionSymbol(name->func->decl);
    }
    if (symbol == Knowledge::kNone && name->cls) {
      symbol = pass_.classSymbol(name->cls);
    }
    return symbol != Knowledge::kNone && pass_.reference(symbol, from_, module_, name->loc(), call);
  }
  if (auto *member = dyn_cast<MemberExpr>(e)) {
    expr(member->object);
    for (const TypeRef &arg : member->typeArgs) {
      typeRef(arg);
    }
    pass_.typed(module_, member->loc(), member->name, member->type);
    size_t symbol = Knowledge::kNone;
    if (member->method) {
      symbol = pass_.functionSymbol(member->method->decl);
    }
    const Type *object = member->object ? member->object->type : nullptr;
    if (symbol == Knowledge::kNone && object) {
      if (const ClassDecl *cls = object->nonOptional()->classRef()) {
        symbol = pass_.fieldSymbol(cls, member->name, true);
      } else if (const ClassDecl *owner = object->nonOptional()->classDecl()) {
        symbol = pass_.fieldSymbol(owner, member->name, false);
      }
    }
    return symbol != Knowledge::kNone &&
           pass_.reference(symbol, from_, module_, member->loc(), call);
  }
  expr(e);

  return false;
}

void KnowledgeWalk::expr(const Expr *e) {
  if (!e) {
    return;
  }
  switch (e->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    return;
  case Expr::Kind::Name:
  case Expr::Kind::Member:
    use(e, false);
    return;
  case Expr::Kind::Template:
    for (const Expr *part : cast<TemplateExpr>(e)->exprs) {
      expr(part);
    }
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &element : cast<ArrayLitExpr>(e)->elements) {
      expr(element.expr);
    }
    return;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &property : cast<ObjectLitExpr>(e)->props) {
      expr(property.key);
      expr(property.value);
    }
    return;
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(e);
    expr(index->object);
    expr(index->index);
    return;
  }
  case Expr::Kind::Unary:
    expr(cast<UnaryExpr>(e)->operand);
    return;
  case Expr::Kind::Binary: {
    auto *binary = cast<BinaryExpr>(e);
    expr(binary->lhs);
    expr(binary->rhs);
    return;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(e);
    expr(assign->target);
    expr(assign->value);
    return;
  }
  case Expr::Kind::Update:
    expr(cast<UpdateExpr>(e)->target);
    return;
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(e);
    bool named = call->callee && use(call->callee, true);
    const FuncSymbol *callee = call->func ? call->func : call->method;
    if (!named && callee && call->callee) {
      size_t symbol = pass_.functionSymbol(callee->decl);
      if (symbol != Knowledge::kNone) {
        pass_.reference(symbol, from_, module_, call->callee->loc(), true);
      }
    }
    for (const Expr *arg : call->args) {
      expr(arg);
    }
    return;
  }
  case Expr::Kind::New: {
    auto *create = cast<NewExpr>(e);
    for (const TypeRef &arg : create->typeArgs) {
      typeRef(arg);
    }
    if (create->callee) {
      use(create->callee, true);
    } else if (create->cls) {
      size_t symbol = pass_.classSymbol(create->cls);
      if (symbol != Knowledge::kNone) {
        pass_.reference(symbol, from_, module_, create->loc(), false);
      }
    }
    if (create->ctor && create->ctor->func) {
      size_t symbol = pass_.functionSymbol(create->ctor->func->decl);
      if (symbol != Knowledge::kNone) {
        pass_.reference(symbol, from_, module_, create->loc(), true);
      }
    }
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      expr(arg);
    }
    return;
  }
  case Expr::Kind::Conditional: {
    auto *conditional = cast<ConditionalExpr>(e);
    expr(conditional->cond);
    expr(conditional->thenExpr);
    expr(conditional->elseExpr);
    return;
  }
  case Expr::Kind::Cast:
    expr(cast<CastExpr>(e)->operand);
    return;
  case Expr::Kind::Sequence:
    for (const Expr *part : cast<SequenceExpr>(e)->exprs) {
      expr(part);
    }
    return;
  case Expr::Kind::OptionalChain:
    expr(cast<OptionalChainExpr>(e)->chain);
    return;
  case Expr::Kind::Function:
    function(cast<FunctionExpr>(e)->fn);
    return;
  case Expr::Kind::Yield:
    expr(cast<YieldExpr>(e)->value);
    return;
  case Expr::Kind::Await:
    expr(cast<AwaitExpr>(e)->value);
    return;
  case Expr::Kind::Class:
    classDecl(cast<ClassExpr>(e)->decl);
    return;
  }
}

void KnowledgeWalk::stmt(const Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *inner : cast<BlockStmt>(s)->body) {
      stmt(inner);
    }
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &each : cast<VarDeclStmt>(s)->decls) {
      declarator(each);
    }
    return;
  case Stmt::Kind::Function:
    function(cast<FunctionDecl>(s));
    return;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(s)->expr);
    return;
  case Stmt::Kind::If: {
    auto *branch = cast<IfStmt>(s);
    expr(branch->cond);
    stmt(branch->thenStmt);
    stmt(branch->elseStmt);
    return;
  }
  case Stmt::Kind::While: {
    auto *loop = cast<WhileStmt>(s);
    expr(loop->cond);
    stmt(loop->body);
    return;
  }
  case Stmt::Kind::DoWhile: {
    auto *loop = cast<DoWhileStmt>(s);
    stmt(loop->body);
    expr(loop->cond);
    return;
  }
  case Stmt::Kind::For: {
    auto *loop = cast<ForStmt>(s);
    stmt(loop->init);
    expr(loop->cond);
    expr(loop->step);
    stmt(loop->body);
    return;
  }
  case Stmt::Kind::ForOf: {
    auto *loop = cast<ForOfStmt>(s);
    declarator(loop->binding);
    expr(loop->iterable);
    stmt(loop->body);
    return;
  }
  case Stmt::Kind::Switch: {
    auto *choice = cast<SwitchStmt>(s);
    expr(choice->discriminant);
    for (const SwitchCase &each : choice->cases) {
      expr(each.test);
      for (const Stmt *inner : each.body) {
        stmt(inner);
      }
    }
    return;
  }
  case Stmt::Kind::Labeled:
    stmt(cast<LabeledStmt>(s)->body);
    return;
  case Stmt::Kind::Return:
    expr(cast<ReturnStmt>(s)->value);
    return;
  case Stmt::Kind::TypeAlias: {
    auto *alias = cast<TypeAliasDecl>(s);
    typeRef(alias->type);
    for (const TypeRef &base : alias->bases) {
      typeRef(base);
    }
    return;
  }
  case Stmt::Kind::Enum:
    for (const EnumMember &member : cast<EnumDecl>(s)->members) {
      expr(member.init);
    }
    return;
  case Stmt::Kind::Class:
    classDecl(cast<ClassDecl>(s));
    return;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    return;
  }
}

}
