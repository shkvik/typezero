#include "compiler/memory/cycle_exprs/cycle_exprs.h"
#include "compiler/memory/cycle_escapes/cycle_escapes.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

CycleExprs::CycleExprs(CycleScanner &scanner)
    : scanner(scanner) {}

bool CycleExprs::runsAndDrops(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseNew:
  case Builtin::PromiseThen:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
    return true;
  default:
    return false;
  }
}

const FunctionExpr *CycleExprs::closureLiteral(const Expr *arg) {
  auto *fn = llvm::dyn_cast<FunctionExpr>(arg);

  return fn && !fn->fn->group ? fn : nullptr;
}

void CycleExprs::closure(const FunctionExpr *expr, bool held) {
  const FunctionDecl *fn = expr->fn;
  if (!fn->nonEscaping) {
    const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
    const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
    this->scanner.envs[key].assign(captures.begin(), captures.end());
    if (held && expr->type) {
      this->scanner.closuresOf[expr->type->nonOptional()].insert(key);
    }
    if (held && fn->sym) {
      this->scanner.closuresOf[CycleScanner::functionType(fn)].insert(key);
    }
  }
  this->scanner.function(fn);
}

void CycleExprs::expr(const Expr *expr) {
  if (!expr) {
    return;
  }
  this->scanner.addType(expr->type);
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
      this->scanner.addType(lit->cls->type);
      for (const FunctionDecl *fn : lit->cls->methods) {
        this->scanner.function(fn);
      }
    }
    for (const ObjectLitExpr::Property &p : lit->props) {
      this->expr(p.key());
      if (!p.method()) {
        this->expr(p.value);
      }
      if (p.spread() && p.value->type && p.value->type->isObject()) {
        for (const Field &f : p.value->type->fields()) {
          if (f.type->isFunction()) {
            this->scanner.boundOf[f.type].insert(p.value->type);
          }
        }
      }
    }
    break;
  }
  case Expr::Kind::Name:
    if (auto *name = llvm::cast<NameExpr>(expr); name->var()) {
      this->scanner.addVar(name->var());
    } else if (name->func() && name->func()->decl && name->func()->decl->isInstance && name->type) {
      const FuncSymbol *fn = name->func();
      const Type *rep = Type::function(
          fn->params, fn->minArgs, fn->hasRest,
          fn->hasReturnType() ? fn->getKnownReturnType() : Type::voidTy(), fn->hasThis);
      this->scanner.instanceFlows.push_back({name->type, rep, true});
    }
    break;
  case Expr::Kind::Member: {
    auto *member = llvm::cast<MemberExpr>(expr);
    this->expr(member->object);
    const Type *object = member->object->type ? member->object->type->nonOptional() : nullptr;
    if (member->type && member->type->nonOptional()->isFunction() && object &&
        !this->scanner.callee.contains(member) &&
        (member->method() || (object->isObject() && !object->isClass() && object->isBase()))) {
      this->scanner.boundOf[member->type->nonOptional()].insert(member->object->type);
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
      this->scanner.mutated.insert(index->object->type->nonOptional());
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
    this->scanner.callee.insert(call->callee);
    this->expr(call->callee);
    if (const FuncSymbol *fn = call->func() ? call->func() : call->method();
        fn && fn->decl && fn->decl->isInstance) {
      if (fn->hasReturnType() && call->type) {
        this->scanner.instanceFlows.push_back({call->type, fn->getKnownReturnType(), true});
      }
      for (size_t i = 0; i < call->args.size() && i < fn->params.size(); ++i) {
        if (call->args[i] && call->args[i]->type && fn->params[i]) {
          this->scanner.instanceFlows.push_back({call->args[i]->type, fn->params[i], false});
        }
      }
    }
    bool drops = CycleExprs::runsAndDrops(call->builtin());
    for (const Expr *arg : call->args) {
      if (const FunctionExpr *fn = drops ? CycleExprs::closureLiteral(arg) : nullptr) {
        this->scanner.addType(arg->type);
        closure(fn, false);
      } else {
        this->expr(arg);
      }
    }
    if (BuiltinGroups::isPromiseBuiltin(call->builtin())) {
      for (const Expr *arg : call->args) {
        if (drops && CycleExprs::closureLiteral(arg)) {
          continue;
        }
        if (arg->type && arg->type->nonOptional()->isFunction()) {
          this->scanner.reactionCallbacks.insert(arg->type->nonOptional());
        }
      }
    }
    switch (call->builtin()) {
      case Builtin::ArrPush:
      case Builtin::ArrUnshift:
      case Builtin::ArrSplice:
      case Builtin::ArrFill:
      case Builtin::MapSet:
      case Builtin::SetAdd:
      case Builtin::RecordSet:
      case Builtin::RecordNullishSet:
        if (auto *member = llvm::dyn_cast<MemberExpr>(call->callee);
            member && member->object->type) {
          this->scanner.mutated.insert(member->object->type->nonOptional());
        }
        break;
      default:
        break;
    }
    break;
  }
  case Expr::Kind::New: {
    auto *create = llvm::cast<NewExpr>(expr);
    for (const Expr *arg : create->ctor() ? create->ctor()->args : create->args) {
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
      this->scanner.toUnknown.insert(conv->operand->type);
    }
    if (conv->type && conv->operand->type && conv->type->nonOptional()->isFunction() &&
        conv->operand->type->nonOptional()->isFunction()) {
      this->scanner.fnCasts.insert({conv->operand->type->nonOptional(), conv->type->nonOptional()});
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
  case Expr::Kind::Function:
    closure(llvm::cast<FunctionExpr>(expr), true);
    break;
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
    if (!index->field().empty()) {
      names.push_back(index->field().str());
    }
    if (index->keyed() && index->index->type) {
      const Type *key = index->index->type;
      for (const Type *n : key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key)) {
        if (n->isStringLiteral()) {
          const std::u16string &text = n->literalText();
          std::string name = ToUtf8::toUtf8(text);
          names.push_back(std::move(name));
        }
      }
    }
  }
  for (std::string &name : names) {
    this->scanner.mutableFields.insert(std::move(name));
  }
}

bool CycleExprs::constructionWrite(const AssignExpr *assign) {
  const FunctionDecl *fn = this->scanner.current;
  if (!fn || !fn->cls() || !fn->thisVar() || (!fn->isConstructor() && fn != fn->cls()->init)) {
    return false;
  }
  auto [it, inserted] = this->scanner.safeWrites.try_emplace(fn);
  if (inserted) {
    const ClassDecl *cls = fn->cls();
    bool initEscapes = cls->init && cls->init->thisVar() &&
                       CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar());
    bool escaped =
        fn == cls->init
            ? cls->parent && this->scanner.cycleEscapes().constructionEscapes(cls->parent)
            : !cls->parent && initEscapes;
    bool afterSuper =
        (cls->parent && this->scanner.cycleEscapes().constructionEscapes(cls->parent)) ||
        initEscapes;
    CycleEscapes::ordered(fn->body, fn->thisVar(), escaped, afterSuper, it->second);
  }

  return it->second.contains(assign);
}

}
