#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// A literal environment on the stack: never freed, so its count only needs
// to stay far from zero.
constexpr uint64_t kStackObjectCount = uint64_t(1) << 62;

// The name of a closure's code in the IR: the functions it is nested in, then
// its own.
std::string qualifiedName(const FunctionDecl *fn) {
  std::string name = fn->name.empty() ? "lambda" : fn->name.str();
  for (const FunctionDecl *p = fn->parent; p; p = p->parent) {
    name = (p->name.empty() ? "lambda" : p->name.str()) + "." + name;
  }

  return "tz." + name;
}

} // namespace

EnvLowerer::EnvLowerer(CodeGen &codegen) : codegen_(codegen) {}

const CodeGen::EnvLayout &EnvLowerer::envLayout(const FunctionDecl *fn) {
  const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
  auto [it, inserted] = codegen_.envLayouts_.try_emplace(key);
  CodeGen::EnvLayout &layout = it->second;
  if (!inserted) {
    return layout;
  }
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
  layout.fields.assign(captures.begin(), captures.end());
  layout.key = key;
  layout.member = codegen_.isCyclicEnv(key);
  std::vector<llvm::Type *> types{codegen_.builder_.getInt64Ty(), codegen_.builder_.getPtrTy()};
  for (const VarSymbol *var : layout.fields) {
    types.push_back(var->boxed() ? codegen_.builder_.getPtrTy() : codegen_.moduleBuilder().lower(ClosureLowerer::typeOf(var)));
  }
  layout.type = llvm::StructType::get(codegen_.ctx_, types);

  // Releases what the environment holds, then frees it.
  auto *dropType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}, false);
  layout.drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                       qualifiedName(fn) + ".env.drop", codegen_.module_.get());
  layout.drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", layout.drop));
  llvm::Value *env = layout.drop->getArg(0);
  // A member releases what it holds as one (see runtime/core/group/group.c).
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    if (codegen_.uncounted_.count({key, var})) { // an owner it lives in (layer 2)
      continue;
    }
    llvm::Value *field = codegen_.builder_.CreateStructGEP(layout.type, env, unsigned(i + 2));
    llvm::Value *value =
        codegen_.builder_.CreateLoad(var->boxed() ? codegen_.builder_.getPtrTy() : codegen_.moduleBuilder().lower(ClosureLowerer::typeOf(var)), field);
    if (var->boxed()) {
      layout.member ? codegen_.storeLowerer().releaseObjectIn(env, value)
                    : (void)codegen_.builder_.CreateCall(codegen_.objReleaseFn_, {value});
    } else if (layout.member) {
      codegen_.storeLowerer().releaseIn(env, value, ClosureLowerer::typeOf(var));
    } else {
      codegen_.refCounts().releaseValue(value, ClosureLowerer::typeOf(var));
    }
  }
  if (layout.member) {
    codegen_.builder_.CreateCall(
        codegen_.groupLowerer().groupFunction("tz_group_obj_free", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}), {env});
    layout.descriptor = codegen_.groupLowerer().memberDescriptor(layout.drop, "closure " + qualifiedName(fn).substr(3));
  } else {
    codegen_.builder_.CreateCall(codegen_.rt_.objFree, {env});
    layout.descriptor = codegen_.classDescriptors().dropDescriptor(layout.drop);
  }
  codegen_.builder_.CreateRetVoid();
  codegen_.builder_.restoreIP(point);

  return layout;
}

llvm::StructType *EnvLowerer::boxType(const Type *type) {
  llvm::StructType *&box = boxTypes_[type];
  if (!box) {
    box = llvm::StructType::get(codegen_.ctx_, {codegen_.builder_.getInt64Ty(), codegen_.builder_.getPtrTy(), codegen_.moduleBuilder().lower(type)});
  }

  return box;
}

llvm::Value *EnvLowerer::newBox(const Type *type, bool member) {
  llvm::StructType *box = boxType(type);
  llvm::Function *&drop = codegen_.boxDrops_[type];
  // A box of a variable whose values may form cycles with it is a member of
  // a group (see runtime/core/group/group.c). Its drop lets go of the value as
  // from a member, which is right for a box outside the groups too (a box of
  // the same type may be either).
  bool memberDrop = (member || codegen_.isCyclicBox(type) || codegen_.memoryAnalysis().mayHoldMember(type)) && type->isManaged();
  member = (member || codegen_.isCyclicBox(type)) && type->isManaged();
  if (!drop) {
    // Releases the value, then frees the box; without a value to release,
    // just frees it.
    auto *dropType = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}, false);
    if (!type->isManaged()) {
      drop = cast<llvm::Function>(codegen_.rt_.objFree.getCallee());
    } else {
      drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage, "tz.box.drop",
                                    codegen_.module_.get());
      drop->addFnAttr(llvm::Attribute::NoUnwind);
      llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
      codegen_.builder_.SetInsertPoint(llvm::BasicBlock::Create(codegen_.ctx_, "entry", drop));
      llvm::Value *object = drop->getArg(0);
      llvm::Value *value =
          codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), codegen_.builder_.CreateStructGEP(box, object, 2));
      if (memberDrop) {
        codegen_.storeLowerer().releaseIn(object, value, type);
        codegen_.builder_.CreateCall(
            codegen_.groupLowerer().groupFunction("tz_group_obj_free", codegen_.builder_.getVoidTy(), {codegen_.builder_.getPtrTy()}),
            {object});
      } else {
        codegen_.refCounts().releaseValue(value, type);
        codegen_.builder_.CreateCall(codegen_.rt_.objFree, {object});
      }
      codegen_.builder_.CreateRetVoid();
      codegen_.builder_.restoreIP(point);
    }
  }
  uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(box);
  llvm::Value *object;
  if (member) {
    object = codegen_.groupLowerer().allocMember(size, "box");
    llvm::Constant *descriptor = codegen_.groupLowerer().memberDescriptor(drop, "box");
    if (!llvm::is_contained(codegen_.boxDescriptors_, std::make_pair(descriptor, type))) {
      codegen_.boxDescriptors_.push_back({descriptor, type});
    }
    codegen_.builder_.CreateStore(descriptor, codegen_.builder_.CreateStructGEP(box, object, 1));
  } else {
    object = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "box");
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(box, object, 0));
    codegen_.builder_.CreateStore(codegen_.classDescriptors().dropDescriptor(drop), codegen_.builder_.CreateStructGEP(box, object, 1));
  }
  codegen_.builder_.CreateStore(llvm::Constant::getNullValue(codegen_.moduleBuilder().lower(type)),
                       codegen_.builder_.CreateStructGEP(box, object, 2));

  return object;
}

llvm::Value *EnvLowerer::boxPointer(const VarSymbol *var) {
  if (llvm::Value *box = codegen_.envBoxes_.lookup(var)) {
    return box;
  }
  llvm::Value *slot = codegen_.boxSlots_.lookup(var);
  assert(slot && "a boxed variable used before its declaration was emitted");

  return codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), slot, var->name + ".box");
}

llvm::Value *EnvLowerer::createEnv(const CodeGen::EnvLayout &layout, bool onStack, bool member) {
  llvm::StructType *type = layout.type;
  llvm::Value *env;
  if (onStack) {
    env = codegen_.functionFrame().createStackSlot(type, "env");
    codegen_.builder_.CreateStore(codegen_.builder_.getInt64(kStackObjectCount),
                         codegen_.builder_.CreateStructGEP(type, env, 0));
    codegen_.builder_.CreateStore(llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy()),
                         codegen_.builder_.CreateStructGEP(type, env, 1));
  } else {
    uint64_t size = codegen_.module_->getDataLayout().getTypeAllocSize(type);
    member = member && layout.member;
    if (member) {
      env = codegen_.groupLowerer().allocMember(size, "env");
    } else {
      env = codegen_.builder_.CreateCall(codegen_.rt_.objAlloc, {codegen_.builder_.getInt64(size)}, "env");
      codegen_.builder_.CreateStore(codegen_.builder_.getInt64(1), codegen_.builder_.CreateStructGEP(type, env, 0));
    }
    codegen_.builder_.CreateStore(layout.descriptor, codegen_.builder_.CreateStructGEP(type, env, 1));
  }
  // An environment on the stack lives only while the variables do, so it
  // borrows their values.
  member = member && layout.member && !onStack;
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *value;
    if (var->boxed()) {
      value = boxPointer(var);
      if (!onStack) {
        codegen_.builder_.CreateCall(codegen_.objRetainFn_, {value});
      }
      if (member) {
        codegen_.builder_.CreateCall(codegen_.groupLowerer().groupFunction("tz_group_store", codegen_.builder_.getVoidTy(),
                                          {codegen_.builder_.getPtrTy(), codegen_.builder_.getPtrTy()}),
                            {env, value});
      }
    } else if (codegen_.uncounted_.count({layout.key, var})) {
      // The closure lives in an object that the object it refers to made
      // and holds alone (docs/objects.md, section 11, layer 2).
      value = emitVarValue(var, /*borrow=*/true);
    } else {
      value = emitVarValue(var, /*borrow=*/onStack);
      if (member) {
        codegen_.storeLowerer().groupStore(env, value, ClosureLowerer::typeOf(var));
      }
    }
    codegen_.builder_.CreateStore(value, codegen_.builder_.CreateStructGEP(type, env, unsigned(i + 2)));
  }

  return env;
}

llvm::Value *EnvLowerer::emitVarValue(const VarSymbol *var, bool borrow) {
  // A function's own value: it is the one being emitted (a constant arrow
  // calling itself), or one of the group being emitted or created here.
  const FunctionDecl *fn = nullptr;
  llvm::Value *env = nullptr;
  if (var->lambda && var->lambda == codegen_.currentFn_) {
    fn = var->lambda;
    env = codegen_.envParam_;
  } else if (const FunctionDecl *member = var->function) {
    NestedGroup *group = member->group;
    if (codegen_.currentFn_ && codegen_.currentFn_->group == group) {
      fn = member;
      env = codegen_.envParam_;
    } else if (group->owner == codegen_.currentFn_) {
      fn = member;
      env = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), codegen_.groupEnvs_.lookup(group));
    }
  }
  if (fn) {
    if (!env) {
      env = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
    }
    if (!borrow) {
      codegen_.builder_.CreateCall(codegen_.objRetainFn_, {env});
    }
    return codegen_.closureLowerer().makeFunctionValue(codegen_.closureLowerer().functionInfo(const_cast<FunctionDecl *>(fn)), env);
  }
  const Type *type = ClosureLowerer::typeOf(var);
  llvm::Value *value = codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(type), codegen_.functionFrame().addressOf(var), var->name);
  if (!borrow) {
    codegen_.refCounts().retainValue(value, type);
  }

  return value;
}

void EnvLowerer::emitGroupCreation(NestedGroup *group) {
  const CodeGen::EnvLayout &layout = envLayout(group->members.front());
  llvm::Value *slot = codegen_.functionFrame().createObjectSlot("group.env");
  codegen_.groupEnvs_[group] = slot;
  if (!layout.fields.empty() || codegen_.comparesFunctions_) {
    codegen_.builder_.CreateStore(createEnv(layout, /*onStack=*/false), slot);
  }
  for (FunctionDecl *member : group->members) {
    codegen_.closureLowerer().closureCode(member);
  }
}

void EnvLowerer::bindCaptures(const FunctionDecl *fn) {
  // A method of an object literal: the object holds what it captured.
  if (fn->cls && fn->cls->literal) {
    codegen_.envParam_->setName("this");
    const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(fn->cls->type);
    const std::vector<VarSymbol *> &captures = fn->cls->group->captures;
    for (size_t i = 0; i < captures.size(); ++i) {
      const VarSymbol *var = captures[i];
      llvm::Value *field = codegen_.builder_.CreateStructGEP(layout.type, codegen_.envParam_,
                                                    unsigned(layout.captures + i), var->name);
      if (var->boxed()) {
        codegen_.envBoxes_[var] = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), field, var->name + ".box");
      } else {
        codegen_.addresses_[var] = field;
      }
    }
    return;
  }
  // The captured variables: the values copied into the environment, and the
  // boxes it holds.
  codegen_.envParam_->setName("env");
  const CodeGen::EnvLayout &layout = envLayout(fn);
  for (size_t i = 0; i < layout.fields.size(); ++i) {
    const VarSymbol *var = layout.fields[i];
    llvm::Value *field =
        codegen_.builder_.CreateStructGEP(layout.type, codegen_.envParam_, unsigned(i + 2), var->name);
    if (var->boxed()) {
      codegen_.envBoxes_[var] = codegen_.builder_.CreateLoad(codegen_.builder_.getPtrTy(), field, var->name + ".box");
    } else {
      codegen_.addresses_[var] = field;
    }
  }
}

} // namespace tz
