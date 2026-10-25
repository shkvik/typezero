#include "compiler/memory/cycle_exprs/cycle_exprs.h"
#include "compiler/memory/cycle_escapes/cycle_escapes.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

CycleExprs::CycleExprs(CycleScanner &scanner) : scanner_(scanner) {}

void CycleExprs::expr(const Expr *expr) {
  if (!expr) {
    return;
  }
  scanner_.addType(expr->type);
  switch (expr->kind()) {
  case Expr::Kind::Template:
    for (const Expr *e : llvm::cast<TemplateExpr>(expr)->exprs) {
      this->expr(e);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : llvm::cast<ArrayLitExpr>(expr)->elements) {
      this->expr(e.expr);
    }
    break;
  case Expr::Kind::ObjectLit: {
    auto *lit = llvm::cast<ObjectLitExpr>(expr);
    if (lit->cls) {
      scanner_.addType(lit->cls->type);
      for (const FunctionDecl *fn : lit->cls->methods) {
        scanner_.function(fn);
      }
    }
    for (const ObjectLitExpr::Property &p : lit->props) {
      this->expr(p.key);
      if (!p.method) {
        this->expr(p.value);
      }
      // A method of an interface, bound to the object a spread copies it
      // from (see emitSpreadLiteral).
      if (p.spread && p.value->type && p.value->type->isObject()) {
        for (const Field &f : p.value->type->fields()) {
          if (f.type->isFunction()) {
            scanner_.boundOf[f.type].insert(p.value->type);
          }
        }
      }
    }
    break;
  }
  case Expr::Kind::Name:
    if (auto *name = llvm::cast<NameExpr>(expr); name->var) {
      scanner_.addVar(name->var);
    }
    break;
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(expr);
    this->expr(member->object);
    // A method as a value is bound to its object; so is a member of an
    // interface that holds a function, which a method of a class that
    // implements it may be.
    const Type *object = member->object->type ? member->object->type->nonOptional() : nullptr;
    if (member->type && member->type->nonOptional()->isFunction() && object &&
        !scanner_.callee_.contains(member) &&
        (member->method || (object->isObject() && !object->isClass() && object->isBase()))) {
      scanner_.boundOf[member->type->nonOptional()].insert(member->object->type);
    }
    break;
  }
  case Expr::Kind::Index:
    this->expr(llvm::cast<IndexExpr>(expr)->object);
    this->expr(llvm::cast<IndexExpr>(expr)->index);
    break;
  case Expr::Kind::Unary:
    this->expr(llvm::cast<UnaryExpr>(expr)->operand);
    break;
  case Expr::Kind::Binary: {
    auto *bin = llvm::cast<BinaryExpr>(expr);
    this->expr(bin->lhs);
    if (bin->op != BinaryOp::InstanceOf) {
      this->expr(bin->rhs);
    }
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = llvm::cast<AssignExpr>(expr);
    assigned(assign);
    if (auto *index = llvm::dyn_cast<IndexExpr>(assign->target); index && index->object->type) {
      scanner_.mutated.insert(index->object->type->nonOptional());
    }
    target(assign->target);
    this->expr(assign->value);
    break;
  }
  case Expr::Kind::Update:
    target(llvm::cast<UpdateExpr>(expr)->target);
    break;
  case Expr::Kind::Call: {
    auto *call = llvm::cast<CallExpr>(expr);
    scanner_.callee_.insert(call->callee);
    this->expr(call->callee);
    for (const Expr *arg : call->args) {
      this->expr(arg);
    }
    if (isPromiseBuiltin(call->builtin) || (call->builtin >= Builtin::ResultAsyncFromPromise &&
                                            call->builtin <= Builtin::ResultAsyncTeeDone)) {
      for (const Expr *arg : call->args) {
        if (arg->type && arg->type->nonOptional()->isFunction()) {
          scanner_.reactionCallbacks.insert(arg->type->nonOptional());
        }
      }
    }
    switch (call->builtin) {
    case Builtin::ArrPush:
    case Builtin::ArrUnshift:
    case Builtin::ArrSplice:
    case Builtin::ArrFill:
    case Builtin::MapSet:
    case Builtin::SetAdd:
    case Builtin::RecordSet:
    case Builtin::RecordNullishSet:
      if (auto *member = llvm::dyn_cast<MemberExpr>(call->callee); member && member->object->type) {
        scanner_.mutated.insert(member->object->type->nonOptional());
      }
      break;
    default:
      break;
    }
    break;
  }
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(expr);
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args) {
      this->expr(arg);
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *cond = llvm::cast<ConditionalExpr>(expr);
    this->expr(cond->cond);
    this->expr(cond->thenExpr);
    this->expr(cond->elseExpr);
    break;
  }
  case Expr::Kind::Cast: {
    auto *conv = llvm::cast<CastExpr>(expr);
    this->expr(conv->operand);
    if (conv->type && conv->operand->type &&
        (conv->type->nonOptional()->isUnknown() || conv->type->nonOptional()->isAnyObject())) {
      scanner_.toUnknown.insert(conv->operand->type);
    }
    if (conv->type && conv->operand->type && conv->type->nonOptional()->isFunction() &&
        conv->operand->type->nonOptional()->isFunction()) {
      scanner_.fnCasts.insert({conv->operand->type->nonOptional(), conv->type->nonOptional()});
    }
    break;
  }
  case Expr::Kind::Sequence:
    for (const Expr *e : llvm::cast<SequenceExpr>(expr)->exprs) {
      this->expr(e);
    }
    break;
  case Expr::Kind::OptionalChain:
    this->expr(llvm::cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Function: {
    const FunctionDecl *fn = llvm::cast<FunctionExpr>(expr)->fn;
    if (!fn->nonEscaping) {
      const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
      const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
      scanner_.envs[key].assign(captures.begin(), captures.end());
      if (expr->type) {
        scanner_.closuresOf[expr->type->nonOptional()].insert(key);
      }
      if (fn->sym) {
        scanner_.closuresOf[CycleScanner::functionType(fn)].insert(key);
      }
    }
    scanner_.function(fn);
    break;
  }
  case Expr::Kind::Yield:
    this->expr(llvm::cast<YieldExpr>(expr)->value);
    break;
  case Expr::Kind::Await:
    this->expr(llvm::cast<AwaitExpr>(expr)->value);
    break;
  default:
    break;
  }
}

void CycleExprs::target(const Expr *target) {
  if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    expr(index->object);
    expr(index->index);
  } else if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    expr(member->object);
  } else {
    expr(target);
  }
}

void CycleExprs::assigned(const AssignExpr *assign) {
  const Expr *target = assign->target;
  std::vector<std::string> names;
  if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    if (constructionWrite(assign)) {
      return;
    }
    names.push_back(member->name.str());
  } else if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    if (!index->field.empty()) {
      names.push_back(index->field.str());
    }
    if (index->keyed && index->index->type) {
      const Type *key = index->index->type;
      for (const Type *n : key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key)) {
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
  }
  for (std::string &name : names) {
    scanner_.mutableFields.insert(std::move(name));
  }
}

bool CycleExprs::constructionWrite(const AssignExpr *assign) {
  const FunctionDecl *fn = scanner_.current_;
  if (!fn || !fn->cls || !fn->thisVar || (!fn->isConstructor && fn != fn->cls->init)) {
    return false;
  }
  auto [it, inserted] = scanner_.safeWrites_.try_emplace(fn);
  if (inserted) {
    const ClassDecl *cls = fn->cls;
    bool initEscapes = cls->init && cls->init->thisVar &&
                       CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar);
    // Before a constructor's body: the initializers, which run first in a
    // class that extends none; in one that does, `super()` runs the class
    // extended and then them.
    bool escaped = fn == cls->init
                       ? cls->parent && scanner_.cycleEscapes().constructionEscapes(cls->parent)
                       : !cls->parent && initEscapes;
    bool afterSuper =
        (cls->parent && scanner_.cycleEscapes().constructionEscapes(cls->parent)) || initEscapes;
    CycleEscapes::ordered(fn->body, fn->thisVar, escaped, afterSuper, it->second);
  }

  return it->second.contains(assign);
}

} // namespace tz
