#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// How CodeGen walks the references in a value for the group functions: the
// reference itself, or the environment of a function; anything else goes to
// the runtime by its descriptor.
enum class RefShape { None, Pointer, Function, Optional, Other };

} // namespace

GroupLowerer::GroupLowerer(CodeGen &codegen) : codegen_(codegen) {}

llvm::FunctionCallee GroupLowerer::groupFunction(llvm::StringRef name, llvm::Type *ret,
                                            llvm::ArrayRef<llvm::Type *> params) {
  llvm::FunctionCallee callee =
      codegen_.module_->getOrInsertFunction(name, llvm::FunctionType::get(ret, params, false));
  cast<llvm::Function>(callee.getCallee())->addFnAttr(llvm::Attribute::NoUnwind);

  return callee;
}

llvm::Value *GroupLowerer::allocMember(uint64_t size, const llvm::Twine &name) {
  llvm::FunctionCallee alloc =
      groupFunction("tz_group_alloc", codegen_.builder_.getPtrTy(), {codegen_.builder_.getInt64Ty()});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  return codegen_.builder_.CreateCall(alloc, {codegen_.builder_.getInt64(size)}, name);
}

void GroupLowerer::adoptFields(llvm::Value *object, const Type *type) {
  if (!codegen_.isCyclic(type)) {
    return;
  }
  const CodeGen::ObjectLayout &layout = codegen_.objectLayouts().objectLayout(type);
  llvm::ArrayRef<Field> fields = type->fields();
  for (unsigned i = 0; i < fields.size(); ++i) {
    if (codegen_.memoryAnalysis().mayHoldMember(fields[i].type)) {
      codegen_.storeLowerer().groupStore(
          object,
          codegen_.builder_.CreateLoad(codegen_.moduleBuilder().lower(fields[i].type),
                              codegen_.builder_.CreateStructGEP(layout.type, object, layout.elements[i])),
          fields[i].type);
    }
  }
}

llvm::Constant *GroupLowerer::memberDescriptor(llvm::Function *drop, llvm::StringRef name) {
  llvm::Constant *&descriptor = codegen_.memberDescriptors_[drop];
  if (descriptor) {
    return descriptor;
  }
  llvm::Constant *null = llvm::ConstantPointerNull::get(codegen_.builder_.getPtrTy());
  llvm::Constant *init = llvm::ConstantStruct::get(
      codegen_.layoutDescTy_, {drop, null, codegen_.builder_.getInt32(0), codegen_.builder_.getInt32(0), codegen_.stringLowerer().utf8Constant(name),
                      codegen_.builder_.getInt32(uint32_t(name.size())), codegen_.builder_.getInt32(0)});
  auto *global = new llvm::GlobalVariable(*codegen_.module_, codegen_.layoutDescTy_, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init,
                                          drop->getName() + ".layout");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  descriptor = global;

  return global;
}

llvm::Value *GroupLowerer::newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                               const llvm::Twine &name) {
  return newArray(arrayType, length, capacity, codegen_.isCyclic(arrayType), name);
}

llvm::Value *GroupLowerer::newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                               bool member, const llvm::Twine &name) {
  llvm::Constant *element = codegen_.typeDescriptors().typeDescriptor(arrayType->inner());
  if (!member) {
    return codegen_.builder_.CreateCall(codegen_.rt_.arrNew, {element, length, capacity}, name);
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i64 = codegen_.builder_.getInt64Ty();
  llvm::FunctionCallee alloc = groupFunction("tz_arr_new_group", ptr, {ptr, i64, i64});
  cast<llvm::Function>(alloc.getCallee())->addRetAttr(llvm::Attribute::NoAlias);

  return codegen_.builder_.CreateCall(alloc, {element, length, capacity}, name);
}

llvm::Function *GroupLowerer::groupAwareRelease(llvm::Function *plain) {
  llvm::Function *&fn = groupReleases_[plain];
  if (fn) {
    return fn;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  auto *type = llvm::FunctionType::get(codegen_.builder_.getVoidTy(), {ptr}, false);
  fn = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, plain->getName() + ".g",
                              codegen_.module_.get());
  fn->addFnAttr(llvm::Attribute::AlwaysInline);
  fn->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> b(llvm::BasicBlock::Create(codegen_.ctx_, "entry", fn));
  auto *countedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "counted", fn);
  auto *memberBlock = llvm::BasicBlock::Create(codegen_.ctx_, "member", fn);
  auto *plainBlock = llvm::BasicBlock::Create(codegen_.ctx_, "plain", fn);
  auto *doneBlock = llvm::BasicBlock::Create(codegen_.ctx_, "done", fn);
  llvm::Value *object = fn->getArg(0);
  b.CreateCondBr(b.CreateIsNotNull(object), countedBlock, doneBlock);
  b.SetInsertPoint(countedBlock);
  llvm::Value *count = b.CreateLoad(b.getInt64Ty(), object, "rc");
  if (codegen_.parallel_) {
    // A shared object, no member: counted atomically; the last release
    // hands it to the plain release with a count of 1, which frees it.
    auto *notMemberBlock = llvm::BasicBlock::Create(codegen_.ctx_, "not.member", fn);
    auto *sharedBlock = llvm::BasicBlock::Create(codegen_.ctx_, "shared", fn);
    auto *lastBlock = llvm::BasicBlock::Create(codegen_.ctx_, "shared.last", fn);
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(uint64_t(-(int64_t(1) << 60)))), memberBlock,
                   notMemberBlock);
    b.SetInsertPoint(notMemberBlock);
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), sharedBlock, plainBlock);
    b.SetInsertPoint(sharedBlock);
    llvm::Value *old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, object, b.getInt64(1),
                                         llvm::MaybeAlign(8), llvm::AtomicOrdering::AcquireRelease);
    b.CreateCondBr(b.CreateICmpEQ(old, b.getInt64(uint64_t(-(int64_t(1) << 60) + 1))), lastBlock,
                   doneBlock);
    b.SetInsertPoint(lastBlock);
    b.CreateStore(b.getInt64(1), object);
    b.CreateBr(plainBlock);
  } else {
    b.CreateCondBr(b.CreateICmpSLT(count, b.getInt64(0)), memberBlock, plainBlock);
  }
  b.SetInsertPoint(memberBlock);
  b.CreateCall(groupFunction("tz_group_release", b.getVoidTy(), {ptr}), {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(plainBlock);
  b.CreateCall(plain, {object});
  b.CreateBr(doneBlock);
  b.SetInsertPoint(doneBlock);
  b.CreateRetVoid();

  return fn;
}

void GroupLowerer::retainObject(llvm::Value *object) {
  codegen_.builder_.CreateCall(codegen_.hasCycles_ ? codegen_.refRetainFn_ : codegen_.arrRetainFn_, {object});
}

llvm::Constant *GroupLowerer::groupSite() {
  llvm::Constant *frame = codegen_.unwindLowerer().frameDescriptor(codegen_.storeLoc_.isValid() ? codegen_.storeLoc_ : codegen_.loc_);
  llvm::Constant *&site = groupSites_[{frame, codegen_.storeField_}];
  if (site) {
    return site;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  auto *type = llvm::StructType::get(codegen_.ctx_, {ptr, ptr, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type, {frame, codegen_.stringLowerer().utf8Constant(codegen_.storeField_), codegen_.builder_.getInt32(uint32_t(codegen_.storeField_.size()))});
  auto *global = new llvm::GlobalVariable(*codegen_.module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.site");
  site = global;

  return site;
}

void GroupLowerer::noteUnstored(llvm::Value *object, const Expr *site) {
  if (!codegen_.debugMemory_ || !codegen_.unstoredSites_.contains(site)) {
    return;
  }
  llvm::Type *ptr = codegen_.builder_.getPtrTy();
  codegen_.builder_.CreateCall(groupFunction("tz_group_note_unstored", codegen_.builder_.getVoidTy(), {ptr, ptr}),
                      {object, codegen_.unwindLowerer().frameDescriptor(site->loc())});
}

} // namespace tz
