#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
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
#include "compiler/pipeline/check_program/topics/fs/fs_error_types/fs_error_types.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

using llvm::cast;

namespace tz {

const std::vector<MakerFields> &FsLowerer::fsObjects() {
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

FsLowerer::FsLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Function *FsLowerer::objectMaker(const Type *type) {
  return this->lowering.objectMaker().maker(type, FsLowerer::fsObjects());
}

llvm::Function *FsLowerer::nativeErrorMaker() {
  return this->objectMaker(this->lowering.nativeError_);
}

llvm::Function *FsLowerer::errorMaker(const Type *error) {
  if (error == this->lowering.nativeError_) {
    return this->nativeErrorMaker();
  }
  llvm::Function *&made = this->errorMakers[error];
  if (made) {
    return made;
  }
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  std::vector<const Type *> variants(error->members().begin(), error->members().end());
  std::vector<llvm::Function *> makers;
  for (const Type *variant : variants) {
    makers.push_back(this->objectMaker(variant));
  }
  made = llvm::Function::Create(llvm::FunctionType::get(ptr, {ptr}, false),
                                llvm::GlobalValue::InternalLinkage,
                                "tz.make." + error->name().str(), this->lowering.module_.get());
  llvm::Function *function = made;
  this->lowering.functionBuilder().setFunctionAttributes(function);
  llvm::IRBuilderBase::InsertPoint point = this->lowering.builder_.saveIP();
  llvm::Function *outer = std::exchange(this->lowering.function_, function);
  this->lowering.builder_.SetInsertPoint(
      llvm::BasicBlock::Create(this->lowering.ctx_, "entry", function));
  llvm::Value *parts = function->getArg(0);
  llvm::Value *code = this->lowering.builder_.CreateLoad(ptr, parts, "code");
  llvm::FunctionCallee compare = this->lowering.module_->getOrInsertFunction(
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
      llvm::Value *same = this->lowering.builder_.CreateICmpEQ(
          this->lowering.builder_.CreateCall(
              compare, {code, this->lowering.stringLowerer().utf8Constant(ascii)}),
          this->lowering.builder_.getInt32(0));
      index = this->lowering.builder_.CreateSelect(
          same, this->lowering.builder_.getInt32(unsigned(v)), index);
    }
  }
  llvm::Value *known = this->lowering.builder_.CreateICmpNE(index, none);
  this->lowering.builder_.CreateCall(
      this->lowering.processLowerer().processFunction(
          "tz_fs_compose", this->lowering.builder_.getVoidTy(), {ptr, ptr, i32}),
      {parts, this->lowering.stringLowerer().utf8Constant(FsErrorTypes::verbOf(error->name())),
       this->lowering.builder_.CreateZExt(known, i32)});
  llvm::Value *chosen =
      this->lowering.builder_.CreateSelect(known, index, this->lowering.builder_.getInt32(unknown));
  llvm::BasicBlock *fallback = llvm::BasicBlock::Create(this->lowering.ctx_, "unknown", function);
  llvm::SwitchInst *pick =
      this->lowering.builder_.CreateSwitch(chosen, fallback, unsigned(variants.size()));
  for (size_t v = 0; v < variants.size(); ++v) {
    llvm::BasicBlock *block = llvm::BasicBlock::Create(this->lowering.ctx_, "variant", function);
    pick->addCase(this->lowering.builder_.getInt32(unsigned(v)), block);
    this->lowering.builder_.SetInsertPoint(block);
    this->lowering.builder_.CreateRet(this->lowering.builder_.CreateCall(makers[v], {parts}));
  }
  this->lowering.builder_.SetInsertPoint(fallback);
  this->lowering.builder_.CreateRet(this->lowering.builder_.CreateCall(makers[unknown], {parts}));
  this->lowering.function_ = outer;
  this->lowering.builder_.restoreIP(point);

  return function;
}

llvm::Value *FsLowerer::emitFsCall(CallExpr *call) {
  if (isFspBuiltin(call->builtin())) {
    return this->lowering.fsPromisesLowerer().emitFsPromise(call);
  }
  if (call->builtin() >= Builtin::RlCreate && call->builtin() <= Builtin::RlClose) {
    return this->lowering.fsPromisesLowerer().emitReadlineBuiltin(call);
  }
  llvm::Type *voidTy = this->lowering.builder_.getVoidTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder_.getInt64Ty(),
             *f64 = this->lowering.builder_.getDoubleTy(),
             *ptr = this->lowering.builder_.getPtrTy();

  if (call->builtin() == Builtin::FsStatsDate) {
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand stats = this->lowering.refCounts().emitOperand(member->object, true);
    llvm::Value *ms = this->lowering.builder_.CreateLoad(
        f64,
        this->lowering.objectLayouts().fieldAddress(stats.value, stats.type,
                                                    (member->name + "Ms").str()),
        "ms");
    this->lowering.refCounts().dropOperand(stats);
    llvm::Value *rounded = this->lowering.builder_.CreateUnaryIntrinsic(
        llvm::Intrinsic::floor,
        this->lowering.builder_.CreateFAdd(ms, llvm::ConstantFP::get(f64, 0.5)));
    return this->lowering.dateLowerer().newDate(this->lowering.dateLowerer().wholeTime(rounded));
  }

  if (call->builtin() == Builtin::FsIsKind) {
    auto *member = cast<MemberExpr>(call->callee);
    Lowering::Operand object = this->lowering.refCounts().emitOperand(member->object, true);
    const Type *type = object.type;
    bool stats = type->field("mode") != nullptr;
    llvm::StringRef field = stats ? "mode" : "Symbol(type)";
    llvm::Value *bits = this->lowering.builder_.CreateLoad(
        i64, this->lowering.objectLayouts().fieldAddress(object.value, type, field));
    this->lowering.refCounts().dropOperand(object);
    llvm::Value *kind = this->lowering.exprLowerer().emitExpr(call->args[0]);
    return this->lowering.builder_.CreateICmpEQ(
        this->lowering.builder_.CreateAnd(bits, this->lowering.builder_.getInt64(0170000)), kind);
  }

  const Type *valueType = call->type->isResult() ? call->type->okType() : call->type;
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  auto release = [&] {
    for (size_t i = 0; i < values.size(); ++i) {
      this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
    }
  };
  auto obj = [&](size_t i) { return this->lowering.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return this->lowering.stringLowerer().stringMeta(values[i]); };
  auto flag = [&](size_t i) { return this->lowering.builder_.CreateZExt(values[i], i32); };
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return this->lowering.processLowerer().processFunction(name, ret, params);
  };
  auto fsCall = [&](llvm::StringRef name, llvm::Type *ret, std::vector<llvm::Type *> params,
                    std::vector<llvm::Value *> args) -> llvm::Value * {
    params.push_back(ptr);
    args.push_back(this->errorMaker(call->type->errType()));
    llvm::Value *result = this->lowering.builder_.CreateCall(fn(name, ret, params), args);
    release();

    return result;
  };
  auto fsString = [&](llvm::StringRef name, std::vector<llvm::Type *> params,
                      std::vector<llvm::Value *> args) {
    llvm::AllocaInst *out =
        this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "fs.out");
    params.insert(params.begin(), ptr);
    args.insert(args.begin(), out);
    fsCall(name, voidTy, params, args);

    return this->lowering.builder_.CreateLoad(this->lowering.strTy_, out);
  };
  auto optionalString = [&](llvm::Value *out, llvm::Value *found) {
    return this->lowering.optionalLowerer().makeOptional(
        this->lowering.builder_.CreateLoad(this->lowering.strTy_, out),
        this->lowering.builder_.CreateICmpNE(found, this->lowering.builder_.getInt32(0)),
        valueType);
  };

  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::ReadLine: {
      bool question = !values.empty();
      llvm::AllocaInst *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "line");
      llvm::Value *found = this->lowering.builder_.CreateCall(
          fn("tz_io_read_line", i32, {ptr, ptr, i64, i32}),
          {out,
           question ? obj(0) : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy()),
           question ? meta(0) : this->lowering.builder_.getInt64(0),
           this->lowering.builder_.getInt32(question ? 1 : 0)});
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
      result = this->lowering.builder_.CreateICmpNE(
          this->lowering.builder_.CreateCall(fn("tz_fs_exists", i32, {ptr, i64}),
                                             {obj(0), meta(0)}),
          this->lowering.builder_.getInt32(0));
      break;
    case Builtin::FsAccess:
    case Builtin::FsChmod:
      fsCall(call->builtin() == Builtin::FsAccess ? "tz_fs_access" : "tz_fs_chmod", voidTy,
             {ptr, i64, i64}, {obj(0), meta(0), values[1]});
      return nullptr;
    case Builtin::FsTruncate:
      fsCall("tz_fs_truncate", voidTy, {ptr, i64, i64}, {obj(0), meta(0), values[1]});
      return nullptr;
    case Builtin::FsStat:
      return fsCall(
          "tz_fs_stat", ptr, {ptr, i64, i32, i32, ptr},
          {obj(0), meta(0), flag(1), flag(2), this->objectMaker(valueType->nonOptional())});
    case Builtin::FsReaddir: {
      const Type *element = valueType->inner();
      llvm::Value *dirent =
          element->isClass() ? static_cast<llvm::Value *>(this->objectMaker(element))
                             : llvm::ConstantPointerNull::get(this->lowering.builder_.getPtrTy());
      return fsCall("tz_fs_readdir", ptr, {ptr, i64, i32, ptr, ptr},
                    {obj(0), meta(0), flag(1),
                     this->lowering.typeDescriptors().typeDescriptor(element), dirent});
    }
    case Builtin::FsMkdir: {
      llvm::AllocaInst *out =
          this->lowering.functionFrame().createStackSlot(this->lowering.strTy_, "mkdir.first");
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
      fsCall(call->builtin() == Builtin::FsRmdir ? "tz_fs_rmdir" : "tz_fs_unlink", voidTy,
             {ptr, i64}, {obj(0), meta(0)});
      return nullptr;
    case Builtin::FsRm:
      fsCall("tz_fs_rm", voidTy, {ptr, i64, i32, i32}, {obj(0), meta(0), flag(1), flag(2)});
      return nullptr;
    case Builtin::FsRename:
    case Builtin::FsReplace:
    case Builtin::FsCopyFile:
      fsCall(call->builtin() == Builtin::FsRename    ? "tz_fs_rename"
             : call->builtin() == Builtin::FsReplace ? "tz_fs_replace"
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
  llvm::Value *value = this->emitFsCall(call);
  if (!call->type->isResult()) {
    return value;
  }

  return this->lowering.resultLowerer().catchFailure(value, call->type, call->loc());
}

}
