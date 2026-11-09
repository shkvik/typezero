#include "compiler/lower/library/os/os_lowerer/os_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

namespace tz {

OsLowerer::OsLowerer(Lowering &owner)
    : lowering(owner) {}

const std::vector<MakerFields> &OsLowerer::osObjects() {
  static const std::vector<MakerFields> table = {
      {nullptr, {"model", "speed", "times"}},
      {nullptr, {"user", "nice", "sys", "idle", "irq"}},
      {nullptr, {"uid", "gid", "username", "homedir", "shell"}},
  };

  return table;
}

llvm::Value *OsLowerer::emitOsCall(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *f64 = this->lowering.builder.getDoubleTy();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return this->lowering.processLowerer().processFunction(name, ret, params);
  };
  auto maker = [&](const Type *type) {
    return this->lowering.objectMaker().maker(type, OsLowerer::osObjects());
  };

  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::OsString:
      result = this->lowering.stringLowerer().callReturningString(
          fn("tz_os_string", voidTy, {ptr, this->lowering.builder.getInt64Ty()}), {values[0]});
      break;
    case Builtin::OsNumber:
      result = this->lowering.builder.CreateCall(
          fn("tz_os_number", f64, {this->lowering.builder.getInt64Ty()}), {values[0]});
      break;
    case Builtin::OsAvailableParallelism:
      result = this->lowering.builder.CreateSExt(
          this->lowering.builder.CreateCall(fn("tz_os_available_parallelism", i32, {})),
          this->lowering.builder.getInt64Ty());
      break;
    case Builtin::OsCpus: {
      const Type *element = call->type->inner();
      result = this->lowering.builder.CreateCall(
          fn("tz_os_cpus", ptr, {ptr, ptr}),
          {this->lowering.typeDescriptors().typeDescriptor(element), maker(element)});
      break;
    }
    case Builtin::OsLoadavg:
      result = this->lowering.builder.CreateCall(
          fn("tz_os_loadavg", ptr, {ptr}),
          {this->lowering.typeDescriptors().typeDescriptor(Type::doubleTy())});
      break;
    case Builtin::OsUserInfo:
      result =
          this->lowering.builder.CreateCall(fn("tz_os_user_info", ptr, {ptr}), {maker(call->type)});
      break;
    default:
      llvm_unreachable("not a built-in of node:os");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return result;
}

}
