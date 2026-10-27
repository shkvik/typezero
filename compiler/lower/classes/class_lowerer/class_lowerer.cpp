#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

bool staysDuringCall(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit || !conv->operand->type->isOptional()) {
      return false;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name || !name->var) {
    return false;
  }
  const VarSymbol *var = name->var;

  return var->isConst || (!var->boxed() && !(var->isGlobal && var->usedInFunction));
}

ClassLowerer::ClassLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *ClassLowerer::allocInstance(const ClassDecl *cls) {
  return allocInstance(cls, lowering_.isCyclic(cls->type));
}

llvm::Value *ClassLowerer::allocInstance(const ClassDecl *cls, bool member) {
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(cls->type);
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = lowering_.groupLowerer().allocMember(size, "object");
  } else {
    object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                           {lowering_.builder_.getInt64(size)}, "object");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                   lowering_.builder_.CreateStructGEP(layout.type, object, 0));
  }
  lowering_.builder_.CreateStore(lowering_.classDescriptors().classDescriptor(cls),
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 1));
  if (size > 16) {
    lowering_.builder_.CreateMemSet(
        lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 16),
        lowering_.builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  }

  return object;
}

llvm::Value *ClassLowerer::emitNew(NewExpr *expr) {
  const ClassDecl *cls = expr->cls;
  if (expr->callee && !isa<NameExpr>(expr->callee)) {
    lowering_.stmtLowerer().emitDiscarded(expr->callee);
  }
  if (cls->builtin) {
    return lowering_.errorLowerer().emitNewError(expr);
  }
  const CallExpr *ctor = expr->ctor;
  std::vector<llvm::Value *> args = lowering_.exprLowerer().emitExprs(ctor->args);
  const FuncSymbol *sym = ctor->func;
  if (sym) {
    for (size_t i = args.size(); i < sym->params.size(); ++i) {
      args.push_back(lowering_.optionalLowerer().undefinedValue(sym->params[i]));
    }
  }
  llvm::Value *object = allocInstance(cls, lowering_.makesMember(expr, cls->type));
  lowering_.groupLowerer().noteUnstored(object, expr);
  llvm::Value *held =
      object;
  emitConstruct(object, cls, sym, args);

  return held;
}

bool ClassLowerer::constructionThrows(const ClassDecl *cls, const FuncSymbol *ctor) const {
  if (ctor && ctor->decl->cls == cls) {
    return (!cls->parent && cls->init && false) ||
           false;
  }

  return (cls->parent && constructionThrows(cls->parent, ctor)) ||
         (cls->init && false);
}

void ClassLowerer::emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                                 llvm::ArrayRef<llvm::Value *> args) {
  if (ctor && ctor->decl->cls == cls) {
    std::vector<llvm::Value *> all{object};
    if (!cls->parent && cls->init) {
      bool initThrows = false;
      std::vector<llvm::Value *> waiting;
      for (size_t i = 0; i < args.size(); ++i) {
        waiting.push_back(args[i]);
      }
      lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(cls->init), {object});
      for (llvm::Value *&w : waiting) {
        all.push_back(w);
      }
    } else {
      all.insert(all.end(), args.begin(), args.end());
    }
    lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(ctor->decl), all);
    return;
  }
  if (cls->parent) {
    emitConstruct(object, cls->parent, ctor, args);
  }
  if (cls->builtin && cls->parent) {
    lowering_.builder_.CreateStore(
        lowering_.stringLowerer().stringLiteral(std::u16string(cls->name.begin(), cls->name.end())),
        lowering_.objectLayouts().fieldAddress(object, cls->type, "name"));
  }
  if (cls->init) {
    lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(cls->init), {object});
  }
}

llvm::Value *ClassLowerer::emitSuperCall(CallExpr *call) {
  const ClassDecl *cls = lowering_.currentFn_->cls;
  std::vector<llvm::Value *> args = lowering_.exprLowerer().emitExprs(call->args);
  if (const FuncSymbol *sym = call->method) {
    for (size_t i = args.size(); i < sym->params.size(); ++i) {
      args.push_back(lowering_.optionalLowerer().undefinedValue(sym->params[i]));
    }
  }
  emitConstruct(lowering_.envParam_, cls->parent, call->method, args);
  if (cls->init) {
    lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(cls->init),
                                  {lowering_.envParam_});
  }

  return nullptr;
}

llvm::Value *ClassLowerer::emitMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const FuncSymbol *method = call->method;
  bool borrow =
      staysDuringCall(member->object) && llvm::all_of(call->args, AssignLowerer::cannotAssign);
  Lowering::Operand self = lowering_.refCounts().emitOperand(member->object, borrow);
  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < method->params.size(); ++i) {
    args.push_back(lowering_.optionalLowerer().undefinedValue(method->params[i]));
  }
  llvm::CallInst *result =
      call->virtualCall
          ? lowering_.builder_.CreateCall(
                lowering_.closureLowerer().closureFnType(ClosureLowerer::functionTypeOf(method)),
                lowering_.inheritLowerer().vtableEntry(self.value, method->decl->slot, false), args)
          : lowering_.builder_.CreateCall(lowering_.classDescriptors().methodCode(method->decl),
                                          args);
  lowering_.refCounts().dropOperand(self);

  return result->getType()->isVoidTy() ? nullptr : result;
}

llvm::Value *ClassLowerer::emitBoundMethod(MemberExpr *member) {
  llvm::Value *object = lowering_.exprLowerer().emitExpr(member->object);
  llvm::Value *info =
      member->virtualCall
          ? lowering_.inheritLowerer().vtableEntry(object, member->method->decl->slot, true)
          : lowering_.closureLowerer().functionInfo(member->method->decl);

  return lowering_.closureLowerer().makeFunctionValue(info, object);
}

llvm::Value *ClassLowerer::emitInstanceOf(BinaryExpr *bin) {
  const ClassDecl *cls = bin->rhs->type->classDecl();
  const Type *type = bin->lhs->type;
  Lowering::Operand value = lowering_.refCounts().emitOperand(bin->lhs, true);
  llvm::Value *first = nullptr, *last = nullptr;
  if (bin->rhs->type->isConstructor()) {
    llvm::Value *rhs = lowering_.exprLowerer().emitExpr(bin->rhs);
    first = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(),
        lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), rhs, 24),
        "first");
    last = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(),
        lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), rhs, 28),
        "last");
  }
  auto classMatches = [&](llvm::Value *object) {
    if (first) {
      llvm::Value *descriptor = lowering_.builder_.CreateLoad(
          lowering_.builder_.getPtrTy(),
          lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 8),
          "descriptor");
      llvm::Value *id = lowering_.builder_.CreateLoad(
          lowering_.builder_.getInt32Ty(),
          lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, descriptor, 6), "class");
      return lowering_.builder_.CreateAnd(
          lowering_.builder_.CreateICmpNE(first, lowering_.builder_.getInt32(0)),
          lowering_.builder_.CreateICmpULE(lowering_.builder_.CreateSub(id, first),
                                           lowering_.builder_.CreateSub(last, first)));
    }
    if (!cls) {
      const Type *those = bin->rhs->type;
      return lowering_.unionLowerer().emitIsOneOf(
          lowering_.unionLowerer().objectClassId(object),
          those->isUnion() ? those->members() : llvm::ArrayRef<const Type *>(those));
    }
    llvm::Value *descriptor = lowering_.builder_.CreateLoad(
        lowering_.builder_.getPtrTy(),
        lowering_.builder_.CreateConstInBoundsGEP1_64(lowering_.builder_.getInt8Ty(), object, 8),
        "descriptor");
    llvm::Value *id = lowering_.builder_.CreateLoad(
        lowering_.builder_.getInt32Ty(),
        lowering_.builder_.CreateStructGEP(lowering_.layoutDescTy_, descriptor, 6), "class");
    if (cls->lastId <= cls->id) {
      return lowering_.builder_.CreateICmpEQ(id, lowering_.builder_.getInt32(cls->id));
    }

    return lowering_.builder_.CreateICmpULE(
        lowering_.builder_.CreateSub(id, lowering_.builder_.getInt32(cls->id)),
        lowering_.builder_.getInt32(cls->lastId - cls->id));
  };
  llvm::Value *result;
  if (type->nonOptional()->isTagged()) {
    llvm::Value *tagged = value.value;
    const Type *u = type->nonOptional();
    llvm::BasicBlock *testBlock = lowering_.functionFrame().newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("instanceof.end");
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateCondBr(
        lowering_.builder_.CreateICmpEQ(lowering_.unionLayout().unionTag(tagged),
                                        lowering_.builder_.getInt32(unsigned(UnionSlot::Object))),
        testBlock, endBlock);
    lowering_.functionFrame().switchTo(testBlock);
    llvm::Value *matches =
        classMatches(lowering_.unionLayout().taggedPart(tagged, u, UnionSlot::Object));
    llvm::BasicBlock *testEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2);
    phi->addIncoming(lowering_.builder_.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  } else if (!type->isOptional() && !type->isUnknown()) {
    result = classMatches(value.value);
  } else {
    llvm::Value *object = lowering_.optionalLowerer().optionalValue(value.value, type);
    llvm::BasicBlock *testBlock = lowering_.functionFrame().newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = lowering_.functionFrame().newBlock("instanceof.end");
    llvm::BasicBlock *entry = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateCondBr(lowering_.optionalLowerer().optionalHas(value.value, type),
                                    testBlock, endBlock);
    lowering_.functionFrame().switchTo(testBlock);
    llvm::Value *matches = classMatches(object);
    llvm::BasicBlock *testEnd = lowering_.builder_.GetInsertBlock();
    lowering_.builder_.CreateBr(endBlock);
    lowering_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = lowering_.builder_.CreatePHI(lowering_.builder_.getInt1Ty(), 2);
    phi->addIncoming(lowering_.builder_.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  }
  lowering_.refCounts().dropOperand(value);

  return result;
}

llvm::Value *ClassLowerer::emitMethodLiteral(ObjectLitExpr *lit) {
  const ClassDecl *cls = lit->cls;
  const Type *type = cls->type;
  const Lowering::ObjectLayout &layout = lowering_.objectLayouts().objectLayout(type);
  uint64_t size = lowering_.module_->getDataLayout().getTypeAllocSize(layout.type);
  bool member = lowering_.makesMember(lit, type);
  llvm::Value *object;
  if (member) {
    object = lowering_.groupLowerer().allocMember(size, "object");
  } else {
    object = lowering_.builder_.CreateCall(lowering_.rt_.objAlloc,
                                           {lowering_.builder_.getInt64(size)}, "object");
    lowering_.builder_.CreateStore(lowering_.builder_.getInt64(1),
                                   lowering_.builder_.CreateStructGEP(layout.type, object, 0));
  }
  lowering_.builder_.CreateStore(lowering_.classDescriptors().classDescriptor(cls),
                                 lowering_.builder_.CreateStructGEP(layout.type, object, 1));
  lowering_.groupLowerer().noteUnstored(object, lit);
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method) {
      continue;
    }
    llvm::Value *value = lowering_.exprLowerer().emitExpr(prop.value);
    lowering_.builder_.CreateStore(value,
                                   lowering_.objectLayouts().fieldAddress(object, type, prop.name));
    if (member) {
      lowering_.storeLowerer().groupStore(object, value, type->field(prop.name)->type);
    }
  }
  const std::vector<VarSymbol *> &captures = cls->group->captures;
  for (size_t i = 0; i < captures.size(); ++i) {
    const VarSymbol *var = captures[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = lowering_.envLowerer().boxPointer(var);
      lowering_.builder_.CreateCall(lowering_.objRetainFn_, {value});
      if (member) {
        lowering_.builder_.CreateCall(
            lowering_.groupLowerer().groupFunction(
                "tz_group_store", lowering_.builder_.getVoidTy(),
                {lowering_.builder_.getPtrTy(), lowering_.builder_.getPtrTy()}),
            {object, value});
      }
    } else {
      value = lowering_.envLowerer().emitVarValue(var, false);
      if (member) {
        lowering_.storeLowerer().groupStore(object, value, ClosureLowerer::typeOf(var));
      }
    }
    lowering_.builder_.CreateStore(value, lowering_.builder_.CreateStructGEP(
                                              layout.type, object, unsigned(layout.captures + i)));
  }
  for (FunctionDecl *fn : cls->methods) {
    lowering_.closureLowerer().closureCode(fn);
  }

  return object;
}

void ClassLowerer::emitStaticFields(ClassDecl *cls) {
  for (const ClassField &f : cls->fields) {
    if (f.isStatic && f.var && f.init) {
      llvm::Value *value = lowering_.exprLowerer().emitExpr(f.init);
      lowering_.storeLowerer().publishValue(value, f.var->type);
      lowering_.stmtLowerer().storeReplacing(lowering_.functionFrame().addressOf(f.var), value,
                                             f.var->type);
      if (lowering_.isEternal(f.var)) {
        lowering_.publishLowerer().pinValue(value, f.var->type);
      }
    }
  }
}

}
