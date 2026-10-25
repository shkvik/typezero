#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/may_throw/may_throw.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

// A variable a method can neither change nor release while it runs, so the
// object in it can be lent to the method.
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

ClassLowerer::ClassLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::Value *ClassLowerer::allocInstance(const ClassDecl *cls) {
  return allocInstance(cls, codegen_.isCyclic(cls->type));
}

llvm::Value *ClassLowerer::allocInstance(const ClassDecl *cls, bool member) {
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(cls->type);
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object;
  if (member) {
    object = codegen_.groupLowerer().allocMember(size, "object"); // counted by its group
  } else {
    object = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "object");
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(layout.type, object, 0));
  }
  codegen_.builder_.CreateStore(codegen_.classDescriptors().classDescriptor(cls), codegen_.builder_.CreateStructGEP(layout.type, object, 1));
  // The fields start empty, so that setting one releases nothing.
  if (size > 16) {
    codegen_.builder_.CreateMemSet(codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 16),
                          codegen_.builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  }

  return object;
}

// The arguments are evaluated first, as in JavaScript; then the object is
// made and its code runs. If that throws, the object goes: its fields that
// are set are released with it.
llvm::Value *ClassLowerer::emitNew(NewExpr *expr) {
  const ClassDecl *cls = expr->cls;
  // A class as a value, known when compiling: what makes it runs first.
  if (expr->callee && !isa<NameExpr>(expr->callee)) {
    codegen_.stmtLowerer().emitDiscarded(expr->callee);
  }
  if (cls->builtin) {
    return codegen_.errorLowerer().emitNewError(expr);
  }
  const CallExpr *ctor = expr->ctor;
  std::vector<llvm::Value *> args = codegen_.throwLowerer().emitHeldValues(ctor->args);
  const FuncSymbol *sym = ctor->func;
  if (sym) {
    for (size_t i = args.size(); i < sym->params.size(); ++i) {
      args.push_back(codegen_.optionalLowerer().undefinedValue(sym->params[i]));
    }
  }
  llvm::Value *object = allocInstance(cls, codegen_.makesMember(expr, cls->type));
  codegen_.groupLowerer().noteUnstored(object, expr);
  // The object goes with what its code throws, released as the error leaves.
  CodeGen::Held held = codegen_.throwLowerer().hold(object, cls->type, constructionThrows(cls, sym));
  emitConstruct(object, cls, sym, args);

  return codegen_.throwLowerer().take(held);
}

bool ClassLowerer::constructionThrows(const ClassDecl *cls, const FuncSymbol *ctor) const {
  if (ctor && ctor->decl->cls == cls) {
    return (!cls->parent && cls->init && codegen_.mayThrow(cls->init)) || codegen_.mayThrow(ctor->decl);
  }

  return (cls->parent && constructionThrows(cls->parent, ctor)) ||
         (cls->init && codegen_.mayThrow(cls->init));
}

// A class with a constructor of its own runs it: in a class that extends
// another, its `super(...)` runs the classes up the chain and then the
// initializers. A class without one runs the class it extends with the
// arguments, and then its initializers, as JavaScript's implicit
// `constructor(...args) { super(...args) }` does. After each part that may
// throw, an error goes up.
void ClassLowerer::emitConstruct(llvm::Value *object, const ClassDecl *cls, const FuncSymbol *ctor,
                            llvm::ArrayRef<llvm::Value *> args) {
  if (ctor && ctor->decl->cls == cls) {
    std::vector<llvm::Value *> all{object};
    if (!cls->parent && cls->init) {
      // The arguments wait meanwhile, if the initializers may throw.
      bool initThrows = codegen_.mayThrow(cls->init);
      std::vector<CodeGen::Held> waiting;
      for (size_t i = 0; i < args.size(); ++i) {
        waiting.push_back(codegen_.throwLowerer().hold(args[i], ctor->params[i], initThrows));
      }
      codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(cls->init), {object});
      codegen_.emitThrowCheckIf(initThrows);
      for (CodeGen::Held &w : waiting) {
        all.push_back(codegen_.throwLowerer().take(w));
      }
    } else {
      all.insert(all.end(), args.begin(), args.end());
    }
    codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(ctor->decl), all);
    codegen_.emitThrowCheckIf(codegen_.mayThrow(ctor->decl));
    return;
  }
  if (cls->parent) {
    emitConstruct(object, cls->parent, ctor, args);
  }
  // A kind of Error (TypeError...) names the objects of the classes that
  // extend it, as its prototype does in JavaScript. The names are ASCII.
  if (cls->builtin && cls->parent) {
    codegen_.builder_.CreateStore(codegen_.stringLowerer().stringLiteral(std::u16string(cls->name.begin(), cls->name.end())),
                         codegen_.objectLayouts().fieldAddress(object, cls->type, "name"));
  }
  if (cls->init) {
    codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(cls->init), {object});
    codegen_.emitThrowCheckIf(codegen_.mayThrow(cls->init));
  }
}

llvm::Value *ClassLowerer::emitSuperCall(CallExpr *call) {
  const ClassDecl *cls = codegen_.currentFn_->cls;
  std::vector<llvm::Value *> args = codegen_.throwLowerer().emitHeldValues(call->args);
  if (const FuncSymbol *sym = call->method) {
    for (size_t i = args.size(); i < sym->params.size(); ++i) {
      args.push_back(codegen_.optionalLowerer().undefinedValue(sym->params[i]));
    }
  }
  emitConstruct(codegen_.envParam_, cls->parent, call->method, args);
  if (cls->init) {
    codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(cls->init), {codegen_.envParam_});
    codegen_.emitThrowCheckIf(codegen_.mayThrow(cls->init));
  }

  return nullptr;
}

llvm::Value *ClassLowerer::emitMethodCall(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const FuncSymbol *method = call->method;
  bool borrow = staysDuringCall(member->object) && llvm::all_of(call->args, AssignLowerer::cannotAssign);
  CodeGen::Operand self = codegen_.refCounts().emitOperand(member->object, borrow);
  codegen_.throwLowerer().holdOperand(self, llvm::any_of(call->args, [&](Expr *arg) { return codegen_.mayThrow().exprThrows(arg); }));
  std::vector<llvm::Value *> values = codegen_.throwLowerer().emitHeldValues(call->args);
  codegen_.throwLowerer().takeOperand(self);
  std::vector<llvm::Value *> args{self.value};
  args.insert(args.end(), values.begin(), values.end());
  for (size_t i = call->args.size(); i < method->params.size(); ++i) {
    args.push_back(codegen_.optionalLowerer().undefinedValue(method->params[i]));
  }
  // A method that classes down the chain override: the one of the object's
  // class, from its method table.
  llvm::CallInst *result =
      call->virtualCall
          ? codegen_.builder_.CreateCall(codegen_.closureLowerer().closureFnType(ClosureLowerer::functionTypeOf(method)),
                                codegen_.inheritLowerer().vtableEntry(self.value, method->decl->slot, /*info=*/false), args)
          : codegen_.builder_.CreateCall(codegen_.classDescriptors().methodCode(method->decl), args);
  // The object goes before what the method threw does.
  codegen_.refCounts().dropOperand(self);
  codegen_.emitThrowCheckIf(codegen_.mayThrow().callMayThrow(method->decl, call->virtualCall) && call != codegen_.returnCall_);

  return result->getType()->isVoidTy() ? nullptr : result;
}

// The value holds a reference to the object, its environment.
llvm::Value *ClassLowerer::emitBoundMethod(MemberExpr *member) {
  llvm::Value *object = codegen_.exprLowerer().emitExpr(member->object);
  llvm::Value *info = member->virtualCall
                          ? codegen_.inheritLowerer().vtableEntry(object, member->method->decl->slot, /*info=*/true)
                          : codegen_.closureLowerer().functionInfo(member->method->decl);

  return codegen_.closureLowerer().makeFunctionValue(info, object);
}

// One comparison of class numbers, after the check for undefined.
llvm::Value *ClassLowerer::emitInstanceOf(BinaryExpr *bin) {
  const ClassDecl *cls = bin->rhs->type->classDecl();
  const Type *type = bin->lhs->type;
  CodeGen::Operand value = codegen_.refCounts().emitOperand(bin->lhs, /*canBorrow=*/true);
  // A class known only when the program runs: the numbers its object holds
  // (TzClass: { rc, layout, parent, i32 id, i32 last_id, ... }).
  llvm::Value *first = nullptr, *last = nullptr;
  if (bin->rhs->type->isConstructor()) {
    llvm::Value *rhs = codegen_.exprLowerer().emitExpr(bin->rhs); // a class is never counted
    first = codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(),
                                codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), rhs, 24),
                                "first");
    last = codegen_.builder_.CreateLoad(codegen_.builder_.getInt32Ty(),
                               codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), rhs, 28),
                               "last");
  }
  auto classMatches = [&](llvm::Value *object) {
    if (first) {
      llvm::Value *descriptor = codegen_.builder_.CreateLoad(
          codegen_.builder_.getPtrTy(), codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 8),
          "descriptor");
      llvm::Value *id = codegen_.builder_.CreateLoad(
          codegen_.builder_.getInt32Ty(), codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, descriptor, 6), "class");
      // A class of JavaScript (`String` of a `design:*` record) has no number:
      // nothing is one of its objects.
      return codegen_.builder_.CreateAnd(
          codegen_.builder_.CreateICmpNE(first, codegen_.builder_.getInt32(0)),
          codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateSub(id, first), codegen_.builder_.CreateSub(last, first)));
    }
    // `instanceof Date` (or Map, Set) of a union: the numbers of its members
    // that Sema gave the right side.
    if (!cls) {
      const Type *those = bin->rhs->type;
      return codegen_.unionLowerer().emitIsOneOf(codegen_.unionLowerer().objectClassId(object),
                         those->isUnion() ? those->members() : llvm::ArrayRef<const Type *>(those));
    }
    llvm::Value *descriptor = codegen_.builder_.CreateLoad(
        codegen_.builder_.getPtrTy(), codegen_.builder_.CreateConstInBoundsGEP1_64(codegen_.builder_.getInt8Ty(), object, 8),
        "descriptor");
    llvm::Value *id = codegen_.builder_.CreateLoad(
        codegen_.builder_.getInt32Ty(), codegen_.builder_.CreateStructGEP(codegen_.layoutDescTy_, descriptor, 6), "class");
    if (cls->lastId <= cls->id) {
      return codegen_.builder_.CreateICmpEQ(id, codegen_.builder_.getInt32(cls->id));
    }

    // The classes down its chain have the numbers that follow its own.
    return codegen_.builder_.CreateICmpULE(codegen_.builder_.CreateSub(id, codegen_.builder_.getInt32(cls->id)),
                                  codegen_.builder_.getInt32(cls->lastId - cls->id));
  };
  llvm::Value *result;
  if (type->nonOptional()->isTagged()) {
    // Only in the slot of objects: a check of the tag first.
    llvm::Value *tagged = value.value;
    const Type *u = type->nonOptional();
    llvm::BasicBlock *testBlock = codegen_.functionFrame().newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("instanceof.end");
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateCondBr(
        codegen_.builder_.CreateICmpEQ(codegen_.unionLayout().unionTag(tagged), codegen_.builder_.getInt32(unsigned(UnionSlot::Object))),
        testBlock, endBlock);
    codegen_.functionFrame().switchTo(testBlock);
    llvm::Value *matches = classMatches(codegen_.unionLayout().taggedPart(tagged, u, UnionSlot::Object));
    llvm::BasicBlock *testEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getInt1Ty(), 2);
    phi->addIncoming(codegen_.builder_.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  } else if (!type->isOptional() && !type->isUnknown()) {
    result = classMatches(value.value);
  } else {
    llvm::Value *object = codegen_.optionalLowerer().optionalValue(value.value, type);
    llvm::BasicBlock *testBlock = codegen_.functionFrame().newBlock("instanceof.test");
    llvm::BasicBlock *endBlock = codegen_.functionFrame().newBlock("instanceof.end");
    llvm::BasicBlock *entry = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateCondBr(codegen_.optionalLowerer().optionalHas(value.value, type), testBlock, endBlock);
    codegen_.functionFrame().switchTo(testBlock);
    llvm::Value *matches = classMatches(object);
    llvm::BasicBlock *testEnd = codegen_.builder_.GetInsertBlock();
    codegen_.builder_.CreateBr(endBlock);
    codegen_.functionFrame().switchTo(endBlock);
    llvm::PHINode *phi = codegen_.builder_.CreatePHI(codegen_.builder_.getInt1Ty(), 2);
    phi->addIncoming(codegen_.builder_.getFalse(), entry);
    phi->addIncoming(matches, testEnd);
    result = phi;
  }
  codegen_.refCounts().dropOperand(value);

  return result;
}

// The fields in the order written, then what the methods capture, as a
// closure's environment holds it.
llvm::Value *ClassLowerer::emitMethodLiteral(ObjectLitExpr *lit) {
  const ClassDecl *cls = lit->cls;
  const Type *type = cls->type;
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(layout.type);
  bool member = codegen_.makesMember(lit, type);
  llvm::Value *object;
  if (member) {
    object = codegen_.groupLowerer().allocMember(size, "object"); // counted by its group
  } else {
    object = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "object");
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(layout.type, object, 0));
  }
  codegen_.builder_.CreateStore(codegen_.classDescriptors().classDescriptor(cls), codegen_.builder_.CreateStructGEP(layout.type, object, 1));
  codegen_.groupLowerer().noteUnstored(object, lit);
  // Nothing can reach the new object while its values are evaluated.
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.method) {
      continue;
    }
    llvm::Value *value = codegen_.exprLowerer().emitExpr(prop.value);
    codegen_.builder_.CreateStore(value, codegen_.objectLayouts().fieldAddress(object, type, prop.name));
    if (member) {
      codegen_.storeLowerer().groupStore(object, value, type->field(prop.name)->type);
    }
  }
  const std::vector<VarSymbol *> &captures = cls->group->captures;
  for (size_t i = 0; i < captures.size(); ++i) {
    const VarSymbol *var = captures[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = codegen_.envLowerer().boxPointer(var);
      codegen_.builder_.CreateCall(codegen_.objRetainFn_, {value});
      if (member) {
        codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_store", codegen_.builder_.getVoidTy(),
                                          {codegen_.builder_.getPtrTy(), codegen_.builder_.getPtrTy()}),
                            {object, value});
      }
    } else {
      value = codegen_.envLowerer().emitVarValue(var, /*borrow=*/false);
      if (member) {
        codegen_.storeLowerer().groupStore(object, value, ClosureLowerer::typeOf(var));
      }
    }
    codegen_.builder_.CreateStore(
        value, codegen_.builder_.CreateStructGEP(layout.type, object, unsigned(layout.captures + i)));
  }
  for (FunctionDecl *fn : cls->methods) {
    codegen_.closureLowerer().closureCode(fn);
  }

  return object;
}

void ClassLowerer::emitStaticFields(ClassDecl *cls) {
  for (const ClassField &f : cls->fields) {
    if (f.isStatic && f.var && f.init) {
      llvm::Value *value = codegen_.exprLowerer().emitExpr(f.init);
      codegen_.storeLowerer().publishValue(value, f.var->type);
      codegen_.stmtLowerer().storeReplacing(codegen_.functionFrame().addressOf(f.var), value, f.var->type);
      if (codegen_.isEternal(f.var)) {
        codegen_.publishLowerer().pinValue(value, f.var->type);
      }
    }
  }
}

} // namespace tz
