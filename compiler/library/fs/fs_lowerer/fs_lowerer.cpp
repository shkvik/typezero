#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/fs/fs_error_types/fs_error_types.h"
#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

using llvm::cast;

namespace tz {

namespace {

const std::vector<MakerFields> &fsObjects() {
  static const std::vector<MakerFields> table = {
      {nullptr, {"message", "errno", "code", "syscall", "path", "dest"}},
      {nullptr, {"code", "message", "syscall", "path"}},
      {nullptr, {"code", "message", "syscall", "path", "dest"}},
      {nullptr, {"code", "message", "path"}},
      {nullptr, {"code", "message"}},
      {"Stats",
       {"dev", "mode", "nlink", "uid", "gid", "rdev", "blksize", "ino", "size", "blocks", "atimeMs",
        "mtimeMs", "ctimeMs", "birthtimeMs"}},
      {"Dirent", {"name", "parentPath", "Symbol(type)"}},
  };

  return table;
}

}

FsLowerer::FsLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Function *FsLowerer::objectMaker(const Type *type) {
  return lowering_.objectMaker().maker(type, fsObjects());
}

llvm::Function *FsLowerer::nativeErrorMaker() { return objectMaker(lowering_.nativeError_); }

llvm::Function *FsLowerer::errorMaker(const Type *error) {
  if (error == lowering_.nativeError_) {
    return nativeErrorMaker();
  }
  llvm::Function *&made = errorMakers_[error];
  if (made) {
    return made;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  std::vector<const Type *> variants(error->members().begin(), error->members().end());
  std::vector<llvm::Function *> makers;
  for (const Type *variant : variants) {
    makers.push_back(objectMaker(variant));
  }
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage,
                                "tz.make." + error->name().str(), lowering_.module_.get());
  llvm::Function *function = made;
  lowering_.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  llvm::Function *outer = std::exchange(lowering_.function_, function);
  lowering_.builder_.SetInsertPoint(llvm::BasicBlock::Create(lowering_.ctx_, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::Value *code = lowering_.builder_.CreateLoad(ptr, parts, "code");
  llvm::FunctionCallee compare = lowering_.module_->getOrInsertFunction(
      "strcmp", llvm::FunctionType::get(i32, {ptr, ptr}, false));
  llvm::Value *none = llvm::ConstantInt::getSigned(i32, -1);
  llvm::Value *index = none;
  unsigned unknown = 0;
  for (size_t v = 0; v < variants.size(); ++v) {
    const Type *codes = variants[v]->field("code")->type;
    std::vector<const Type *> literals =
        codes->isStringLiteral()
            ? std::vector<const Type *>{codes}
            : std::vector<const Type *>(codes->members().begin(), codes->members().end());
    for (const Type *literal : literals) {
      std::string ascii;
      for (char16_t unit : literal->literalText()) {
        ascii.push_back(char(unit));
      }
      if (ascii == "UNKNOWN") {
        unknown = unsigned(v);
      }
      llvm::Value *same = lowering_.builder_.CreateICmpEQ(
          lowering_.builder_.CreateCall(compare,
                                        {code, lowering_.stringLowerer().utf8Constant(ascii)}),
          lowering_.builder_.getInt32(0));
      index =
          lowering_.builder_.CreateSelect(same, lowering_.builder_.getInt32(unsigned(v)), index);
    }
  }
  llvm::Value *known = lowering_.builder_.CreateICmpNE(index, none);
  lowering_.builder_.CreateCall(
      lowering_.processLowerer().processFunction("tz_fs_compose", lowering_.builder_.getVoidTy(),
                                                 {ptr, ptr, i32}),
      {parts, lowering_.stringLowerer().utf8Constant(FsErrorTypes::verbOf(error->name())),
       lowering_.builder_.CreateZExt(known, i32)});
  llvm::Value *chosen =
      lowering_.builder_.CreateSelect(known, index, lowering_.builder_.getInt32(unknown));
  llvm::BasicBlock *fallback = llvm::BasicBlock::Create(lowering_.ctx_, "unknown", function);
  llvm::SwitchInst *pick =
      lowering_.builder_.CreateSwitch(chosen, fallback, unsigned(variants.size()));
  for (size_t v = 0; v < variants.size(); ++v) {
    llvm::BasicBlock *block = llvm::BasicBlock::Create(lowering_.ctx_, "variant", function);
    pick->addCase(lowering_.builder_.getInt32(unsigned(v)), block);
    lowering_.builder_.SetInsertPoint(block);
    lowering_.builder_.CreateRet(lowering_.builder_.CreateCall(makers[v], {parts}));
  }
  lowering_.builder_.SetInsertPoint(fallback);
  lowering_.builder_.CreateRet(lowering_.builder_.CreateCall(makers[unknown], {parts}));
  lowering_.function_ = outer;
  lowering_.builder_.restoreIP(point);

  return function;
}

llvm::Value *FsLowerer::emitFsCall(CallExpr *call) {
  if (isFspBuiltin(call->builtin)) {
    return lowering_.fsPromisesLowerer().emitFsPromise(call);
  }
  if (call->builtin >= Builtin::RlCreate && call->builtin <= Builtin::RlClose) {
    return lowering_.fsPromisesLowerer().emitReadlineBuiltin(call);
  }
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *i64 = lowering_.builder_.getInt64Ty(), *f64 = lowering_.builder_.getDoubleTy(),
             *ptr = lowering_.builder_.getPtrTy();

  if (call->builtin == Builtin::FsStatsDate) {
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand stats = lowering_.refCounts().emitOperand(member->object, true);
    llvm::Value *ms =
        lowering_.builder_.CreateLoad(f64,
                                      lowering_.objectLayouts().fieldAddress(
                                          stats.value, stats.type, (member->name + "Ms").str()),
                                      "ms");
    lowering_.refCounts().dropOperand(stats);
    llvm::Value *rounded = lowering_.builder_.CreateUnaryIntrinsic(
        llvm::Intrinsic::floor, lowering_.builder_.CreateFAdd(ms, llvm::ConstantFP::get(f64, 0.5)));
    return lowering_.dateLowerer().newDate(lowering_.dateLowerer().wholeTime(rounded));
  }

  if (call->builtin == Builtin::FsIsKind) {
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, true);
    const Type *type = object.type;
    bool stats = type->field("mode") != nullptr;
    llvm::StringRef field = stats ? "mode" : "Symbol(type)";
    llvm::Value *bits = lowering_.builder_.CreateLoad(
        i64, lowering_.objectLayouts().fieldAddress(object.value, type, field));
    lowering_.refCounts().dropOperand(object);
    llvm::Value *kind = lowering_.exprLowerer().emitExpr(call->args[0]);
    return lowering_.builder_.CreateICmpEQ(
        lowering_.builder_.CreateAnd(bits, lowering_.builder_.getInt64(0170000)), kind);
  }

  const Type *valueType = call->type->isResult() ? call->type->okType() : call->type;
  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
  auto release = [&] {
    for (size_t i = 0; i < values.size(); ++i) {
      lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
    }
  };
  auto obj = [&](size_t i) { return lowering_.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return lowering_.stringLowerer().stringMeta(values[i]); };
  auto flag = [&](size_t i) { return lowering_.builder_.CreateZExt(values[i], i32); };
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return lowering_.processLowerer().processFunction(name, ret, params);
  };
  auto fsCall = [&](llvm::StringRef name, llvm::Type *ret, std::vector<llvm::Type *> params,
                    std::vector<llvm::Value *> args) -> llvm::Value * {
    params.push_back(ptr);
    args.push_back(errorMaker(call->type->errType()));
    llvm::Value *result = lowering_.builder_.CreateCall(fn(name, ret, params), args);
    release();

    return result;
  };
  auto fsString = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                      std::vector<llvm::Value *> args) {
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "fs.out");
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), out);
    fsCall(name, voidTy, params, args);

    return lowering_.builder_.CreateLoad(lowering_.strTy_, out);
  };
  auto optionalString = [&](llvm::Value *out, llvm::Value *found) {
    return lowering_.optionalLowerer().makeOptional(
        lowering_.builder_.CreateLoad(lowering_.strTy_, out),
        lowering_.builder_.CreateICmpNE(found, lowering_.builder_.getInt32(0)), valueType);
  };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::ReadLine: {
    bool question = !values.empty();
    llvm::AllocaInst *out = lowering_.functionFrame().createStackSlot(lowering_.strTy_, "line");
    llvm::Value *found = lowering_.builder_.CreateCall(
        fn("tz_io_read_line", i32, {ptr, ptr, i64, i32}),
        {out, question ? obj(0) : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy()),
         question ? meta(0) : lowering_.builder_.getInt64(0),
         lowering_.builder_.getInt32(question ? 1 : 0)});
    release();
    return optionalString(out, found);
  }
  case Builtin::FsReadFile:
    if (valueType->isBytes()) {
      if (call->args[0]->type->isInteger()) {
        return fsCall("tz_fs_read_fd_bytes", ptr, {i64}, {values[0]});
      }
      return fsCall("tz_fs_read_file_bytes", ptr, {ptr, i64, ptr, i64},
                    {obj(0), meta(0), obj(1), meta(1)});
    }
    if (call->args[0]->type->isInteger()) {
      return fsString("tz_fs_read_fd", {i64, i64}, {values[0], values[2]});
    }
    return fsString("tz_fs_read_file", {ptr, i64, ptr, i64, i64},
                    {obj(0), meta(0), obj(1), meta(1), values[2]});
  case Builtin::FsWriteFile:
    if (!call->args[1]->type->isString()) {
      if (call->args[0]->type->isInteger()) {
        fsCall("tz_fs_write_fd_bytes", voidTy, {i64, ptr}, {values[0], values[1]});
      } else {
        fsCall("tz_fs_write_file_bytes", voidTy, {ptr, i64, ptr, ptr, i64, i64},
               {obj(0), meta(0), values[1], obj(2), meta(2), values[3]});
      }
    } else if (call->args[0]->type->isInteger()) {
      fsCall("tz_fs_write_fd", voidTy, {i64, ptr, i64, i64},
             {values[0], obj(1), meta(1), values[4]});
    } else {
      fsCall("tz_fs_write_file", voidTy, {ptr, i64, ptr, i64, ptr, i64, i64, i64},
             {obj(0), meta(0), obj(1), meta(1), obj(2), meta(2), values[3], values[4]});
    }
    return nullptr;
  case Builtin::FsExists:
    result = lowering_.builder_.CreateICmpNE(
        lowering_.builder_.CreateCall(fn("tz_fs_exists", i32, {ptr, i64}), {obj(0), meta(0)}),
        lowering_.builder_.getInt32(0));
    break;
  case Builtin::FsAccess:
  case Builtin::FsChmod:
    fsCall(call->builtin == Builtin::FsAccess ? "tz_fs_access" : "tz_fs_chmod", voidTy,
           {ptr, i64, i64}, {obj(0), meta(0), values[1]});
    return nullptr;
  case Builtin::FsTruncate:
    fsCall("tz_fs_truncate", voidTy, {ptr, i64, i64}, {obj(0), meta(0), values[1]});
    return nullptr;
  case Builtin::FsStat:
    return fsCall("tz_fs_stat", ptr, {ptr, i64, i32, i32, ptr},
                  {obj(0), meta(0), flag(1), flag(2), objectMaker(valueType->nonOptional())});
  case Builtin::FsReaddir: {
    const Type *element = valueType->inner();
    llvm::Value *dirent = element->isClass()
                              ? static_cast<llvm::Value *>(objectMaker(element))
                              : llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
    return fsCall(
        "tz_fs_readdir", ptr, {ptr, i64, i32, ptr, ptr},
        {obj(0), meta(0), flag(1), lowering_.typeDescriptors().typeDescriptor(element), dirent});
  }
  case Builtin::FsMkdir: {
    llvm::AllocaInst *out =
        lowering_.functionFrame().createStackSlot(lowering_.strTy_, "mkdir.first");
    llvm::Value *made = fsCall("tz_fs_mkdir", i32, {ptr, ptr, i64, i32, i64},
                               {out, obj(0), meta(0), flag(1), values[2]});
    return optionalString(out, made);
  }
  case Builtin::FsMkdtemp:
    return fsString("tz_fs_mkdtemp", {ptr, i64}, {obj(0), meta(0)});
  case Builtin::FsRealpath:
    return fsString("tz_fs_realpath", {ptr, i64}, {obj(0), meta(0)});
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
    fsCall(call->builtin == Builtin::FsRmdir ? "tz_fs_rmdir" : "tz_fs_unlink", voidTy, {ptr, i64},
           {obj(0), meta(0)});
    return nullptr;
  case Builtin::FsRm:
    fsCall("tz_fs_rm", voidTy, {ptr, i64, i32, i32}, {obj(0), meta(0), flag(1), flag(2)});
    return nullptr;
  case Builtin::FsRename:
  case Builtin::FsReplace:
  case Builtin::FsCopyFile:
    fsCall(call->builtin == Builtin::FsRename    ? "tz_fs_rename"
           : call->builtin == Builtin::FsReplace ? "tz_fs_replace"
                                                 : "tz_fs_copy_file",
           voidTy, {ptr, i64, ptr, i64}, {obj(0), meta(0), obj(1), meta(1)});
    return nullptr;
  default:
    llvm_unreachable("not a built-in of node:fs");
  }
  release();

  return result;
}

llvm::Value *FsLowerer::emitFsResult(CallExpr *call) {
  llvm::Value *value = emitFsCall(call);
  if (!call->type->isResult()) {
    return value;
  }

  return lowering_.resultLowerer().catchFailure(value, call->type, call->loc());
}

}
