#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
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
      {"SystemError", {"message", "name", "errno", "code", "syscall", "path", "dest"}},
      {"Stats",
       {"dev", "mode", "nlink", "uid", "gid", "rdev", "blksize", "ino", "size", "blocks", "atimeMs",
        "mtimeMs", "ctimeMs", "birthtimeMs"}},
      {"Dirent", {"name", "parentPath", "#type"}},
  };

  return table;
}

}

FsLowerer::FsLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Function *FsLowerer::objectMaker(const Type *type) {
  return lowering_.objectMaker().maker(type, fsObjects());
}

llvm::Function *FsLowerer::systemErrorMaker() {
  for (const ClassDecl *child : lowering_.errorClass_->children) {
    if (child->builtin && child->name == "SystemError") {
      return objectMaker(child->type);
    }
  }
  llvm_unreachable("Checker declares SystemError");
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
    return lowering_.dateLowerer().newDate(lowering_.builder_.CreateCall(
        lowering_.processLowerer().processFunction("tz_date_clip", f64, {f64}), {rounded}));
  }

  if (call->builtin == Builtin::FsIsKind) {
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand object = lowering_.refCounts().emitOperand(member->object, true);
    const Type *type = object.type;
    bool stats = type->field("mode") != nullptr;
    llvm::StringRef field = stats ? "mode" : "#type";
    llvm::Value *bits = lowering_.builder_.CreateLoad(
        stats ? f64 : i32, lowering_.objectLayouts().fieldAddress(object.value, type, field));
    if (stats) {
      bits = lowering_.builder_.CreateFPToSI(bits, i32);
    }
    lowering_.refCounts().dropOperand(object);
    llvm::Value *kind = lowering_.exprLowerer().emitExpr(call->args[0]);
    return lowering_.builder_.CreateICmpEQ(
        lowering_.builder_.CreateAnd(bits, lowering_.builder_.getInt32(0170000)), kind);
  }

  std::vector<llvm::Value *> values = lowering_.throwLowerer().emitHeldValues(call->args);
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
    args.push_back(systemErrorMaker());
    llvm::Value *result = lowering_.builder_.CreateCall(fn(name, ret, params), args);
    release();
    lowering_.throwLowerer().emitThrowCheck();

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
        lowering_.builder_.CreateICmpNE(found, lowering_.builder_.getInt32(0)), call->type);
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
    if (call->type->isBytes()) {
      if (call->args[0]->type->isInt()) {
        return fsCall("tz_fs_read_fd_bytes", ptr, {i32}, {values[0]});
      }
      return fsCall("tz_fs_read_file_bytes", ptr, {ptr, i64, ptr, i64},
                    {obj(0), meta(0), obj(1), meta(1)});
    }
    if (call->args[0]->type->isInt()) {
      return fsString("tz_fs_read_fd", {i32, i32}, {values[0], values[2]});
    }
    return fsString("tz_fs_read_file", {ptr, i64, ptr, i64, i32},
                    {obj(0), meta(0), obj(1), meta(1), values[2]});
  case Builtin::FsWriteFile:
    if (!call->args[1]->type->isString()) {
      if (call->args[0]->type->isInt()) {
        fsCall("tz_fs_write_fd_bytes", voidTy, {i32, ptr}, {values[0], values[1]});
      } else {
        fsCall("tz_fs_write_file_bytes", voidTy, {ptr, i64, ptr, ptr, i64, i32},
               {obj(0), meta(0), values[1], obj(2), meta(2), values[3]});
      }
    } else if (call->args[0]->type->isInt()) {
      fsCall("tz_fs_write_fd", voidTy, {i32, ptr, i64, i32},
             {values[0], obj(1), meta(1), values[4]});
    } else {
      fsCall("tz_fs_write_file", voidTy, {ptr, i64, ptr, i64, ptr, i64, i32, i32},
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
           {ptr, i64, i32}, {obj(0), meta(0), values[1]});
    return nullptr;
  case Builtin::FsTruncate:
    fsCall("tz_fs_truncate", voidTy, {ptr, i64, i64}, {obj(0), meta(0), values[1]});
    return nullptr;
  case Builtin::FsStat:
    return fsCall("tz_fs_stat", ptr, {ptr, i64, i32, i32, ptr},
                  {obj(0), meta(0), flag(1), flag(2), objectMaker(call->type->nonOptional())});
  case Builtin::FsReaddir: {
    const Type *element = call->type->inner();
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
    llvm::Value *made = fsCall("tz_fs_mkdir", i32, {ptr, ptr, i64, i32, i32},
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
    fsCall("tz_fs_rename", voidTy, {ptr, i64, ptr, i64}, {obj(0), meta(0), obj(1), meta(1)});
    return nullptr;
  case Builtin::FsCopyFile:
    fsCall("tz_fs_copy_file", voidTy, {ptr, i64, ptr, i64, i32},
           {obj(0), meta(0), obj(1), meta(1), values[2]});
    return nullptr;
  default:
    llvm_unreachable("not a built-in of node:fs");
  }
  release();

  return result;
}

}
