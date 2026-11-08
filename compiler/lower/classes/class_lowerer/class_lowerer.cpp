#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool ClassLowerer::staysDuringCall(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit || !conv->operand->type->isOptional()) {
      return false;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name || !name->var()) {
    return false;
  }
  const VarSymbol *var = name->var();

  return var->isConst || (!var->boxed() && !(var->isGlobal() && var->usedInFunction()));
}

ClassLowerer::ClassLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *ClassLowerer::allocInstance(const ClassDecl *cls) {
  return this->allocInstance(cls, this->lowering.isCyclic(cls->type));
}

llvm::Value *ClassLowerer::allocInstance(const ClassDecl *cls, bool member) {
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(cls->type);
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = this->lowering.groupLowerer().allocMember(size, "object");
  } else {
    object = this->lowering.builder.CreateCall(this->lowering.rt_.objAlloc,
                                               {this->lowering.builder.getInt64(size)}, "object");
    this->lowering.builder.CreateStore(
        this->lowering.builder.getInt64(1),
        this->lowering.builder.CreateStructGEP(layout.type, object, 0));
  }
  this->lowering.builder.CreateStore(
      this->lowering.classDescriptors().classDescriptor(cls),
      this->lowering.builder.CreateStructGEP(layout.type, object, 1));
  if (size > 16) {
    this->lowering.builder.CreateMemSet(this->lowering.builder.CreateConstInBoundsGEP1_64(
                                            this->lowering.builder.getInt8Ty(), object, 16),
                                        this->lowering.builder.getInt8(0), size - 16,
                                        llvm::MaybeAlign(8));
  }

  return object;
}

llvm::Value *ClassLowerer::emitNew(NewExpr *expr) {
  const ClassDecl *cls = expr->cls();
  if (expr->callee && !isa<NameExpr>(expr->callee)) {
    this->lowering.stmtLowerer().emitDiscarded(expr->callee);
  }
  const CallExpr *ctor = expr->ctor();
  std::vector<llvm::Value *> args = this->lowering.exprLowerer().emitExprs(ctor->args);
  const FuncSymbol *sym = ctor->func();
  if (sym) {
    for (size_t i = args.size(); i < sym->params.size(); ++i) {
      args.push_back(this->lowering.optionalLowerer().undefinedValue(sym->params[i]));
    }
  }
  llvm::Value *object = this->allocInstance(cls, this->lowering.makesMember(expr, cls->type));
  this->lowering.groupLowerer().noteUnstored(object, expr);
  this->emitConstruct(object, cls, sym, args);

  return object;
}

void ClassLowerer::emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                                 llvm::ArrayRef<llvm::Value *> args) {
  if (ctor && ctor->decl->cls() == cls) {
    std::vector<llvm::Value *> all{object};
    if (!cls->parent && cls->init) {
      this->lowering.builder.CreateCall(this->lowering.classDescriptors().methodCode(cls->init),
                                        {object});
    }
    all.insert(all.end(), args.begin(), args.end());
    this->lowering.builder.CreateCall(this->lowering.classDescriptors().methodCode(ctor->decl),
                                      all);
    return;
  }
  if (cls->parent) {
    this->emitConstruct(object, cls->parent, ctor, args);
  }
  if (cls->init) {
    this->lowering.builder.CreateCall(this->lowering.classDescriptors().methodCode(cls->init),
                                      {object});
  }
}

llvm::Value *ClassLowerer::emitSuperCall(CallExpr *call) {
  const ClassDecl *cls = this->lowering.currentFn->cls();
  std::vector<llvm::Value *> args = this->lowering.exprLowerer().emitExprs(call->args);
  const FuncSymbol *sym = call->method();
  if (sym) {
    for (size_t i = args.size(); i < sym->params.size(); ++i) {
      args.push_back(this->lowering.optionalLowerer().undefinedValue(sym->params[i]));
    }
  }
  this->emitConstruct(this->lowering.envParam, cls->parent, call->method(), args);
  if (cls->init) {
    this->lowering.builder.CreateCall(this->lowering.classDescriptors().methodCode(cls->init),
                                      {this->lowering.envParam});
  }

  return nullptr;
}

llvm::Value *ClassLowerer::emitMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const FuncSymbol *method = call->method();
  bool borrow = ClassLowerer::staysDuringCall(member->object) &&
                llvm::all_of(call->args, AssignLowerer::cannotAssign);
  Lowering::Operand self = this->lowering.refCounts().emitOperand(member->object, borrow);
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < method->params.size(); ++i) {
    args.push_back(this->lowering.optionalLowerer().undefinedValue(method->params[i]));
  }
  if (!call->virtualCall()) {
    this->lowering.markAwaitedCall(call, method->decl);
  }
  llvm::CallInst *result =
      call->virtualCall()
          ? this->lowering.builder.CreateCall(this->lowering.closureLowerer().closureFnType(
                                                  ClosureLowerer::functionTypeOf(method)),
                                              this->lowering.inheritLowerer().vtableEntry(
                                                  self.value, method->decl->slot(), false),
                                              args)
          : this->lowering.builder.CreateCall(
                this->lowering.classDescriptors().methodCode(method->decl), args);
  this->lowering.refCounts().dropOperand(self);

  return result->getType()->isVoidTy() ? nullptr : result;
}

llvm::Value *ClassLowerer::emitBoundMethod(MemberExpr *member) {
  llvm::Value *object = this->lowering.exprLowerer().emitExpr(member->object);
  llvm::Value *info = member->virtualCall()
                          ? this->lowering.inheritLowerer().vtableEntry(
                                object, member->method()->decl->slot(), true)
                          : this->lowering.closureLowerer().functionInfo(member->method()->decl);

  return this->lowering.closureLowerer().makeFunctionValue(info, object);
}

llvm::Value *ClassLowerer::emitInstanceOf(BinaryExpr *bin) {
  const ClassDecl *cls = bin->rhs->type->classDecl();
  const Type *type = bin->lhs->type;
  Lowering::Operand value = this->lowering.refCounts().emitOperand(bin->lhs, true);
  llvm::Value *first = nullptr;
  llvm::Value *last = nullptr;
  if (bin->rhs->type->isConstructor()) {
    llvm::Value *rhs = this->lowering.exprLowerer().emitExpr(bin->rhs);
    first = this->lowering.builder.CreateLoad(this->lowering.builder.getInt32Ty(),
                                              this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                  this->lowering.builder.getInt8Ty(), rhs, 24),
                                              "first");
    last = this->lowering.builder.CreateLoad(this->lowering.builder.getInt32Ty(),
                                             this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                 this->lowering.builder.getInt8Ty(), rhs, 28),
                                             "last");
  }
  auto classMatches = [&](llvm::Value *object) {
    if (first) {
      llvm::Value *descriptor =
          this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                            this->lowering.builder.CreateConstInBoundsGEP1_64(
                                                this->lowering.builder.getInt8Ty(), object, 8),
                                            "descriptor");
      llvm::Value *id = this->lowering.builder.CreateLoad(
          this->lowering.builder.getInt32Ty(),
          this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, descriptor, 6),
          "class");
      return this->lowering.builder.CreateAnd(
          this->lowering.builder.CreateICmpNE(first, this->lowering.builder.getInt32(0)),
          this->lowering.builder.CreateICmpULE(this->lowering.builder.CreateSub(id, first),
                                               this->lowering.builder.CreateSub(last, first)));
    }
    if (!cls) {
      const Type *those = bin->rhs->type;
      return this->lowering.unionLowerer().emitIsOneOf(
          this->lowering.unionLowerer().objectClassId(object),
          those->isUnion() ? those->members() : llvm::ArrayRef<const Type *>(those));
    }
    llvm::Value *descriptor =
        this->lowering.builder.CreateLoad(this->lowering.builder.getPtrTy(),
                                          this->lowering.builder.CreateConstInBoundsGEP1_64(
                                              this->lowering.builder.getInt8Ty(), object, 8),
                                          "descriptor");
    llvm::Value *id = this->lowering.builder.CreateLoad(
        this->lowering.builder.getInt32Ty(),
        this->lowering.builder.CreateStructGEP(this->lowering.layoutDescTy, descriptor, 6),
        "class");
    if (cls->lastId <= cls->id) {
      return this->lowering.builder.CreateICmpEQ(id, this->lowering.builder.getInt32(cls->id));
    }

    return this->lowering.builder.CreateICmpULE(
        this->lowering.builder.CreateSub(id, this->lowering.builder.getInt32(cls->id)),
        this->lowering.builder.getInt32(cls->lastId - cls->id));
  };
  llvm::Value *result;
  if (type->nonOptional()->isTagged()) {
    llvm::Value *tagged = value.value;
    const Type *u = type->nonOptional();
    llvm::BasicBlock *testBlock = this->lowering.functionFrame().newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("instanceof.end");
    llvm::BasicBlock *entry = this->lowering.builder.GetInsertBlock();
    this->lowering.builder.CreateCondBr(
        this->lowering.builder.CreateICmpEQ(
            this->lowering.unionLayout().unionTag(tagged),
            this->lowering.builder.getInt32(unsigned(UnionSlot::Object))),
        testBlock, endBlock);
    this->lowering.functionFrame().switchTo(testBlock);
    llvm::Value *matches =
        classMatches(this->lowering.unionLayout().taggedPart(tagged, u, UnionSlot::Object));
    llvm::BasicBlock *testEnd = this->lowering.builder.GetInsertBlock();
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.builder.getInt1Ty(), 2);
    phi->addIncoming(this->lowering.builder.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  } else if (!type->isOptional() && !type->isUnknown()) {
    result = classMatches(value.value);
  } else {
    llvm::Value *object = this->lowering.optionalLowerer().optionalValue(value.value, type);
    llvm::BasicBlock *testBlock = this->lowering.functionFrame().newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = this->lowering.functionFrame().newBlock("instanceof.end");
    llvm::BasicBlock *entry = this->lowering.builder.GetInsertBlock();
    this->lowering.builder.CreateCondBr(
        this->lowering.optionalLowerer().optionalHas(value.value, type), testBlock, endBlock);
    this->lowering.functionFrame().switchTo(testBlock);
    llvm::Value *matches = classMatches(object);
    llvm::BasicBlock *testEnd = this->lowering.builder.GetInsertBlock();
    this->lowering.builder.CreateBr(endBlock);
    this->lowering.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = this->lowering.builder.CreatePHI(this->lowering.builder.getInt1Ty(), 2);
    phi->addIncoming(this->lowering.builder.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  }
  this->lowering.refCounts().dropOperand(value);

  return result;
}

llvm::Value *ClassLowerer::emitMethodLiteral(ObjectLitExpr *lit) {
  const ClassDecl *cls = lit->cls;
  const Type *type = cls->type;
  const Lowering::ObjectLayout &layout = this->lowering.objectLayouts().objectLayout(type);
  uint64_t size = this->lowering.module_->getDataLayout().getTypeAllocSize(layout.type);
  bool member = this->lowering.makesMember(lit, type);
  llvm::Value *object;
  if (member) {
    object = this->lowering.groupLowerer().allocMember(size, "object");
  } else {
    object = this->lowering.builder.CreateCall(this->lowering.rt_.objAlloc,
                                               {this->lowering.builder.getInt64(size)}, "object");
    this->lowering.builder.CreateStore(
        this->lowering.builder.getInt64(1),
        this->lowering.builder.CreateStructGEP(layout.type, object, 0));
  }
  this->lowering.builder.CreateStore(
      this->lowering.classDescriptors().classDescriptor(cls),
      this->lowering.builder.CreateStructGEP(layout.type, object, 1));
  this->lowering.groupLowerer().noteUnstored(object, lit);
  std::vector<std::pair<llvm::Value *, const Type *>> fields;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method()) {
      continue;
    }
    llvm::Value *value = this->lowering.exprLowerer().emitExpr(prop.value);
    this->lowering.builder.CreateStore(
        value, this->lowering.objectLayouts().fieldAddress(object, type, prop.name));
    fields.push_back({value, type->field(prop.name)->type});
  }
  const std::vector<VarSymbol *> &captures = cls->group->captures;
  std::vector<llvm::Value *> boxes;
  for (size_t i = 0; i < captures.size(); ++i) {
    const VarSymbol *var = captures[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = this->lowering.envLowerer().boxPointer(var);
      this->lowering.builder.CreateCall(this->lowering.objRetainFn, {value});
      boxes.push_back(value);
    } else {
      value = this->lowering.envLowerer().emitVarValue(var, false);
      fields.push_back({value, ClosureLowerer::typeOf(var)});
    }
    this->lowering.builder.CreateStore(
        value,
        this->lowering.builder.CreateStructGEP(layout.type, object, unsigned(layout.captures + i)));
  }
  if (member) {
    for (auto [value, fieldType] : fields) {
      this->lowering.storeLowerer().groupStore(object, value, fieldType);
    }
    for (llvm::Value *box : boxes) {
      this->lowering.builder.CreateCall(
          this->lowering.groupLowerer().groupFunction(
              "tz_group_store", this->lowering.builder.getVoidTy(),
              {this->lowering.builder.getPtrTy(), this->lowering.builder.getPtrTy()}),
          {object, box});
    }
  }
  for (FunctionDecl *fn : cls->methods) {
    this->lowering.closureLowerer().closureCode(fn);
  }

  return object;
}

void ClassLowerer::emitStaticFields(ClassDecl *cls) {
  for (const ClassField &f : cls->fields) {
    if (f.isStatic() && f.var() && f.init) {
      llvm::Value *value = this->lowering.exprLowerer().emitExpr(f.init);
      this->lowering.storeLowerer().publishValue(value, f.var()->type);
      this->lowering.stmtLowerer().storeReplacing(this->lowering.functionFrame().addressOf(f.var()),
                                                  value, f.var()->type);
      if (this->lowering.isEternal(f.var())) {
        this->lowering.publishLowerer().pinValue(value, f.var()->type);
      }
    }
  }
}

}
