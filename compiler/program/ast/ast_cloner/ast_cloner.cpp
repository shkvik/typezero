#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

namespace tz {

AstCloner::AstCloner(AstContext &target)
    : context(target),
      renames(true) {}

ClassDecl *AstCloner::copyClass(const ClassDecl *cls) {
  const ClassDecl *source = cls->pristine != nullptr ? cls->pristine : cls;
  ClassDecl *copy = this->cloneClass(source);
  this->finishCopies(false);

  return copy;
}

FunctionDecl *AstCloner::copyFunction(const FunctionDecl *fn) {
  const FunctionDecl *source = fn->pristine != nullptr ? fn->pristine : fn;
  FunctionDecl *copy = this->cloneFunction(source);
  this->finishCopies(false);

  return copy;
}

void AstCloner::keepPristine(llvm::ArrayRef<Stmt *> items) {
  this->renames = false;
  for (const Stmt *item : items) {
    this->cloneStmt(item);
  }
  this->finishCopies(true);
}

Expr *AstCloner::cloneExpr(const Expr *expr) {
  if (expr == nullptr) {
    return nullptr;
  }
  Node *known = this->copies.lookup(expr);
  if (known != nullptr) {
    return static_cast<Expr *>(known);
  }
  switch (expr->kind()) {
    case Expr::Kind::IntLit: {
      IntLitExpr *copy = this->context.make<IntLitExpr>(*llvm::cast<IntLitExpr>(expr));
      this->copies[expr] = copy;

      return copy;
    }
    case Expr::Kind::FloatLit: {
      FloatLitExpr *copy = this->context.make<FloatLitExpr>(*llvm::cast<FloatLitExpr>(expr));
      this->copies[expr] = copy;

      return copy;
    }
    case Expr::Kind::BoolLit: {
      BoolLitExpr *copy = this->context.make<BoolLitExpr>(*llvm::cast<BoolLitExpr>(expr));
      this->copies[expr] = copy;

      return copy;
    }
    case Expr::Kind::StringLit: {
      StringLitExpr *copy = this->context.make<StringLitExpr>(*llvm::cast<StringLitExpr>(expr));
      this->copies[expr] = copy;

      return copy;
    }
    case Expr::Kind::Undefined: {
      UndefinedExpr *copy = this->context.make<UndefinedExpr>(*llvm::cast<UndefinedExpr>(expr));
      this->copies[expr] = copy;

      return copy;
    }
    case Expr::Kind::RegexLit: {
      RegexLitExpr *copy = this->context.make<RegexLitExpr>(*llvm::cast<RegexLitExpr>(expr));
      this->copies[expr] = copy;

      return copy;
    }
    case Expr::Kind::Template: {
      TemplateExpr *copy = this->context.make<TemplateExpr>(*llvm::cast<TemplateExpr>(expr));
      this->copies[expr] = copy;
      this->cloneExpressions(copy->exprs);

      return copy;
    }
    case Expr::Kind::ArrayLit: {
      ArrayLitExpr *copy = this->context.make<ArrayLitExpr>(*llvm::cast<ArrayLitExpr>(expr));
      this->copies[expr] = copy;
      for (ArrayLitExpr::Element &element : copy->elements) {
        element.expr = this->cloneExpr(element.expr);
      }

      return copy;
    }
    case Expr::Kind::ObjectLit: {
      ObjectLitExpr *copy = this->context.make<ObjectLitExpr>(*llvm::cast<ObjectLitExpr>(expr));
      this->copies[expr] = copy;
      for (ObjectLitExpr::Property &property : copy->props) {
        this->cloneProperty(property);
      }

      return copy;
    }
    case Expr::Kind::Name: {
      NameExpr *copy = this->context.make<NameExpr>(*llvm::cast<NameExpr>(expr));
      this->copies[expr] = copy;
      copy->name = this->usedName(copy->name);

      return copy;
    }
    case Expr::Kind::Member: {
      MemberExpr *copy = this->context.make<MemberExpr>(*llvm::cast<MemberExpr>(expr));
      this->copies[expr] = copy;
      copy->object = this->cloneExpr(copy->object);

      return copy;
    }
    case Expr::Kind::Index: {
      IndexExpr *copy = this->context.make<IndexExpr>(*llvm::cast<IndexExpr>(expr));
      this->copies[expr] = copy;
      copy->object = this->cloneExpr(copy->object);
      copy->index = this->cloneExpr(copy->index);

      return copy;
    }
    case Expr::Kind::Unary: {
      UnaryExpr *copy = this->context.make<UnaryExpr>(*llvm::cast<UnaryExpr>(expr));
      this->copies[expr] = copy;
      copy->operand = this->cloneExpr(copy->operand);

      return copy;
    }
    case Expr::Kind::Binary: {
      BinaryExpr *copy = this->context.make<BinaryExpr>(*llvm::cast<BinaryExpr>(expr));
      this->copies[expr] = copy;
      copy->lhs = this->cloneExpr(copy->lhs);
      copy->rhs = this->cloneExpr(copy->rhs);

      return copy;
    }
    case Expr::Kind::Assign: {
      AssignExpr *copy = this->context.make<AssignExpr>(*llvm::cast<AssignExpr>(expr));
      this->copies[expr] = copy;
      copy->target = this->cloneExpr(copy->target);
      copy->value = this->cloneExpr(copy->value);

      return copy;
    }
    case Expr::Kind::Update: {
      UpdateExpr *copy = this->context.make<UpdateExpr>(*llvm::cast<UpdateExpr>(expr));
      this->copies[expr] = copy;
      copy->target = this->cloneExpr(copy->target);

      return copy;
    }
    case Expr::Kind::Call: {
      CallExpr *copy = this->context.make<CallExpr>(*llvm::cast<CallExpr>(expr));
      this->copies[expr] = copy;
      copy->callee = this->cloneExpr(copy->callee);
      this->cloneExpressions(copy->args);

      return copy;
    }
    case Expr::Kind::New: {
      NewExpr *copy = this->context.make<NewExpr>(*llvm::cast<NewExpr>(expr));
      this->copies[expr] = copy;
      copy->callee = this->cloneExpr(copy->callee);
      this->cloneExpressions(copy->args);

      return copy;
    }
    case Expr::Kind::Conditional: {
      ConditionalExpr *copy =
          this->context.make<ConditionalExpr>(*llvm::cast<ConditionalExpr>(expr));
      this->copies[expr] = copy;
      copy->cond = this->cloneExpr(copy->cond);
      copy->thenExpr = this->cloneExpr(copy->thenExpr);
      copy->elseExpr = this->cloneExpr(copy->elseExpr);

      return copy;
    }
    case Expr::Kind::Cast: {
      CastExpr *copy = this->context.make<CastExpr>(*llvm::cast<CastExpr>(expr));
      this->copies[expr] = copy;
      copy->operand = this->cloneExpr(copy->operand);

      return copy;
    }
    case Expr::Kind::Sequence: {
      SequenceExpr *copy = this->context.make<SequenceExpr>(*llvm::cast<SequenceExpr>(expr));
      this->copies[expr] = copy;
      this->cloneExpressions(copy->exprs);

      return copy;
    }
    case Expr::Kind::OptionalChain: {
      OptionalChainExpr *copy =
          this->context.make<OptionalChainExpr>(*llvm::cast<OptionalChainExpr>(expr));
      this->copies[expr] = copy;
      copy->chain = this->cloneExpr(copy->chain);

      return copy;
    }
    case Expr::Kind::Function: {
      FunctionExpr *copy = this->context.make<FunctionExpr>(*llvm::cast<FunctionExpr>(expr));
      this->copies[expr] = copy;
      copy->fn = this->cloneFunction(copy->fn);

      return copy;
    }
    case Expr::Kind::Yield: {
      YieldExpr *copy = this->context.make<YieldExpr>(*llvm::cast<YieldExpr>(expr));
      this->copies[expr] = copy;
      copy->value = this->cloneExpr(copy->value);

      return copy;
    }
    case Expr::Kind::Await: {
      AwaitExpr *copy = this->context.make<AwaitExpr>(*llvm::cast<AwaitExpr>(expr));
      this->copies[expr] = copy;
      copy->value = this->cloneExpr(copy->value);

      return copy;
    }
    case Expr::Kind::Class: {
      ClassExpr *copy = this->context.make<ClassExpr>(*llvm::cast<ClassExpr>(expr));
      this->copies[expr] = copy;
      copy->decl = this->cloneClass(copy->decl);

      return copy;
    }
  }

  return nullptr;
}

Stmt *AstCloner::cloneStmt(const Stmt *stmt) {
  if (stmt == nullptr) {
    return nullptr;
  }
  Node *known = this->copies.lookup(stmt);
  if (known != nullptr) {
    return static_cast<Stmt *>(known);
  }
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return this->cloneBlock(llvm::cast<BlockStmt>(stmt));
    case Stmt::Kind::Function:
      return this->cloneFunction(llvm::cast<FunctionDecl>(stmt));
    case Stmt::Kind::Class:
      return this->cloneClass(llvm::cast<ClassDecl>(stmt));
    case Stmt::Kind::VarDecl: {
      VarDeclStmt *copy = this->context.make<VarDeclStmt>(*llvm::cast<VarDeclStmt>(stmt));
      this->copies[stmt] = copy;
      for (VarDeclarator &declarator : copy->decls) {
        this->cloneDeclarator(declarator);
      }

      return copy;
    }
    case Stmt::Kind::Expr: {
      ExprStmt *copy = this->context.make<ExprStmt>(*llvm::cast<ExprStmt>(stmt));
      this->copies[stmt] = copy;
      copy->expr = this->cloneExpr(copy->expr);

      return copy;
    }
    case Stmt::Kind::If: {
      IfStmt *copy = this->context.make<IfStmt>(*llvm::cast<IfStmt>(stmt));
      this->copies[stmt] = copy;
      copy->cond = this->cloneExpr(copy->cond);
      copy->thenStmt = this->cloneStmt(copy->thenStmt);
      copy->elseStmt = this->cloneStmt(copy->elseStmt);

      return copy;
    }
    case Stmt::Kind::While: {
      WhileStmt *copy = this->context.make<WhileStmt>(*llvm::cast<WhileStmt>(stmt));
      this->copies[stmt] = copy;
      copy->cond = this->cloneExpr(copy->cond);
      copy->body = this->cloneStmt(copy->body);

      return copy;
    }
    case Stmt::Kind::DoWhile: {
      DoWhileStmt *copy = this->context.make<DoWhileStmt>(*llvm::cast<DoWhileStmt>(stmt));
      this->copies[stmt] = copy;
      copy->body = this->cloneStmt(copy->body);
      copy->cond = this->cloneExpr(copy->cond);

      return copy;
    }
    case Stmt::Kind::For: {
      ForStmt *copy = this->context.make<ForStmt>(*llvm::cast<ForStmt>(stmt));
      this->copies[stmt] = copy;
      copy->init = this->cloneStmt(copy->init);
      copy->cond = this->cloneExpr(copy->cond);
      copy->step = this->cloneExpr(copy->step);
      copy->body = this->cloneStmt(copy->body);

      return copy;
    }
    case Stmt::Kind::ForOf: {
      ForOfStmt *copy = this->context.make<ForOfStmt>(*llvm::cast<ForOfStmt>(stmt));
      this->copies[stmt] = copy;
      this->cloneDeclarator(copy->binding);
      copy->iterable = this->cloneExpr(copy->iterable);
      copy->body = this->cloneStmt(copy->body);

      return copy;
    }
    case Stmt::Kind::Switch: {
      SwitchStmt *copy = this->context.make<SwitchStmt>(*llvm::cast<SwitchStmt>(stmt));
      this->copies[stmt] = copy;
      copy->discriminant = this->cloneExpr(copy->discriminant);
      for (SwitchCase &switchCase : copy->cases) {
        this->cloneSwitchCase(switchCase);
      }

      return copy;
    }
    case Stmt::Kind::Labeled: {
      LabeledStmt *copy = this->context.make<LabeledStmt>(*llvm::cast<LabeledStmt>(stmt));
      this->copies[stmt] = copy;
      copy->body = this->cloneStmt(copy->body);

      return copy;
    }
    case Stmt::Kind::Return: {
      ReturnStmt *copy = this->context.make<ReturnStmt>(*llvm::cast<ReturnStmt>(stmt));
      this->copies[stmt] = copy;
      copy->value = this->cloneExpr(copy->value);

      return copy;
    }
    case Stmt::Kind::Break: {
      BreakStmt *copy = this->context.make<BreakStmt>(*llvm::cast<BreakStmt>(stmt));
      this->copies[stmt] = copy;

      return copy;
    }
    case Stmt::Kind::Continue: {
      ContinueStmt *copy = this->context.make<ContinueStmt>(*llvm::cast<ContinueStmt>(stmt));
      this->copies[stmt] = copy;

      return copy;
    }
    case Stmt::Kind::Empty: {
      EmptyStmt *copy = this->context.make<EmptyStmt>(*llvm::cast<EmptyStmt>(stmt));
      this->copies[stmt] = copy;

      return copy;
    }
    case Stmt::Kind::Debugger: {
      DebuggerStmt *copy = this->context.make<DebuggerStmt>(*llvm::cast<DebuggerStmt>(stmt));
      this->copies[stmt] = copy;

      return copy;
    }
    case Stmt::Kind::TypeAlias: {
      TypeAliasDecl *copy = this->context.make<TypeAliasDecl>(*llvm::cast<TypeAliasDecl>(stmt));
      this->copies[stmt] = copy;

      return copy;
    }
    case Stmt::Kind::Enum: {
      EnumDecl *copy = this->context.make<EnumDecl>(*llvm::cast<EnumDecl>(stmt));
      this->copies[stmt] = copy;
      for (EnumMember &member : copy->members) {
        member.init = this->cloneExpr(member.init);
      }

      return copy;
    }
  }

  return nullptr;
}

void AstCloner::cloneParam(Param &param) {
  param.name = this->ownName(param.name);
  DefaultedParam *defaulted = param.form.as<DefaultedParam>();
  if (defaulted != nullptr) {
    defaulted->defaultValue = this->cloneExpr(defaulted->defaultValue);
  }
  this->cloneDecorators(param.decorators);
}

void AstCloner::cloneField(ClassField &field) {
  field.init = this->cloneExpr(field.init);
  this->cloneDecorators(field.decorators);
  ArrowMethodField *arrow = field.form.as<ArrowMethodField>();
  if (arrow != nullptr) {
    arrow->method = this->cloneFunction(arrow->method);
  }
}

void AstCloner::finishCopies(bool snapshot) {
  for (const std::pair<const FunctionDecl *, FunctionDecl *> &copied : this->functions) {
    FunctionDecl *copy = copied.second;
    ClassMember *member = copy->role.as<ClassMember>();
    if (member != nullptr && member->cls != nullptr) {
      Node *cls = this->copies.lookup(member->cls);
      if (cls != nullptr) {
        member->cls = static_cast<ClassDecl *>(cls);
      }
    }
    if (snapshot) {
      const_cast<FunctionDecl *>(copied.first)->pristine = copy;
      copy->pristine = nullptr;
    } else {
      const FunctionDecl *source = copied.first;
      copy->pristine = source->pristine != nullptr ? source->pristine : source;
    }
  }
  for (const std::pair<const ClassDecl *, ClassDecl *> &copied : this->classes) {
    ClassDecl *copy = copied.second;
    LiteralClass *literal = copy->syntax.as<LiteralClass>();
    if (literal != nullptr && literal->literal != nullptr) {
      Node *object = this->copies.lookup(literal->literal);
      if (object != nullptr) {
        literal->literal = static_cast<ObjectLitExpr *>(object);
      }
    }
    if (snapshot) {
      const_cast<ClassDecl *>(copied.first)->pristine = copy;
      copy->pristine = nullptr;
    } else {
      const ClassDecl *source = copied.first;
      copy->pristine = source->pristine != nullptr ? source->pristine : source;
    }
  }
  for (SpreadProperty *spread : this->spreads) {
    for (const IndexExpr *&key : spread->omitKeys) {
      Node *index = this->copies.lookup(key);
      if (index != nullptr) {
        key = static_cast<const IndexExpr *>(index);
      }
    }
  }
  this->functions.clear();
  this->classes.clear();
  this->spreads.clear();
}

ClassDecl *AstCloner::cloneClass(const ClassDecl *cls) {
  if (cls == nullptr) {
    return nullptr;
  }
  Node *known = this->copies.lookup(cls);
  if (known != nullptr) {
    return static_cast<ClassDecl *>(known);
  }
  ClassDecl *copy = this->context.make<ClassDecl>(*cls);
  this->copies[cls] = copy;
  this->classes.push_back({cls, copy});
  copy->baseExpr = this->cloneExpr(copy->baseExpr);
  for (ClassField &field : copy->fields) {
    this->cloneField(field);
  }
  for (FunctionDecl *&method : copy->methods) {
    method = this->cloneFunction(method);
  }
  for (FunctionDecl *&constructor : copy->constructors) {
    constructor = this->cloneFunction(constructor);
  }
  copy->init = this->cloneFunction(copy->init);
  this->cloneDecorators(copy->decorators);
  this->cloneStatements(copy->decorations);

  return copy;
}

BlockStmt *AstCloner::cloneBlock(const BlockStmt *block) {
  if (block == nullptr) {
    return nullptr;
  }
  Node *known = this->copies.lookup(block);
  if (known != nullptr) {
    return static_cast<BlockStmt *>(known);
  }
  BlockStmt *copy = this->context.make<BlockStmt>(*block);
  this->copies[block] = copy;
  this->cloneStatements(copy->body);

  return copy;
}

void AstCloner::cloneSwitchCase(SwitchCase &switchCase) {
  switchCase.test = this->cloneExpr(switchCase.test);
  this->cloneStatements(switchCase.body);
}

void AstCloner::cloneDeclarator(VarDeclarator &declarator) {
  declarator.name = this->ownName(declarator.name);
  declarator.init = this->cloneExpr(declarator.init);
}

llvm::StringRef AstCloner::ownName(llvm::StringRef name) {
  if (!this->renames || !AstCloner::isHiddenName(name)) {
    return name;
  }
  llvm::StringRef fresh = this->context.hiddenName();
  this->hiddenNames[name] = fresh;

  return fresh;
}

llvm::StringRef AstCloner::usedName(llvm::StringRef name) const {
  llvm::StringMap<llvm::StringRef>::const_iterator found = this->hiddenNames.find(name);
  if (found == this->hiddenNames.end()) {
    return name;
  }

  return found->second;
}

FunctionDecl *AstCloner::cloneFunction(const FunctionDecl *fn) {
  if (fn == nullptr) {
    return nullptr;
  }
  Node *known = this->copies.lookup(fn);
  if (known != nullptr) {
    return static_cast<FunctionDecl *>(known);
  }
  FunctionDecl *copy = this->context.make<FunctionDecl>(*fn);
  this->copies[fn] = copy;
  this->functions.push_back({fn, copy});
  ClassMember *member = copy->role.as<ClassMember>();
  if (member != nullptr) {
    this->cloneDecorators(member->decorators);
  }
  for (Param &param : copy->params) {
    this->cloneParam(param);
  }
  copy->body = this->cloneBlock(copy->body);

  return copy;
}

void AstCloner::cloneStatements(std::vector<Stmt *> &statements) {
  for (Stmt *&statement : statements) {
    statement = this->cloneStmt(statement);
  }
}

void AstCloner::cloneProperty(ObjectLitExpr::Property &property) {
  property.value = this->cloneExpr(property.value);
  ComputedProperty *computed = property.form.as<ComputedProperty>();
  if (computed != nullptr) {
    computed->key = this->cloneExpr(computed->key);
  }
  SpreadProperty *spread = property.form.as<SpreadProperty>();
  if (spread != nullptr) {
    this->spreads.push_back(spread);
  }
}

void AstCloner::cloneDecorators(std::vector<Decorator> &decorators) {
  for (Decorator &decorator : decorators) {
    decorator.expr = this->cloneExpr(decorator.expr);
  }
}

void AstCloner::cloneExpressions(std::vector<Expr *> &expressions) {
  for (Expr *&expression : expressions) {
    expression = this->cloneExpr(expression);
  }
}

bool AstCloner::isHiddenName(llvm::StringRef name) {
  if (name.size() < 2 || name.front() != '%') {
    return false;
  }
  llvm::StringRef digits = name.drop_front();

  return llvm::all_of(digits, [](char c) { return llvm::isDigit(c); });
}

}
