#include "compiler/library/os/os_lowerer/os_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

namespace tz {

namespace {

const std::vector<MakerFields> &osObjects() {
  static const std::vector<MakerFields> table = {
      {nullptr, {"model", "speed", "times"}},
      {nullptr, {"user", "nice", "sys", "idle", "irq"}},
      {nullptr, {"uid", "gid", "username", "homedir", "shell"}},
  };

  return table;
}

}

OsLowerer::OsLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *OsLowerer::emitOsCall(CallExpr *call) {
  llvm::Type *voidTy = lowering_.builder_.getVoidTy(), *i32 = lowering_.builder_.getInt32Ty();
  llvm::Type *f64 = lowering_.builder_.getDoubleTy(), *ptr = lowering_.builder_.getPtrTy();
  std::vector<llvm::Value *> values = lowering_.exprLowerer().emitExprs(call->args);
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return lowering_.processLowerer().processFunction(name, ret, params);
  };
  auto maker = [&](const Type *type) { return lowering_.objectMaker().maker(type, osObjects()); };

  llvm::Value *result = nullptr;
  switch (call->builtin) {
  case Builtin::OsString:
    result = lowering_.stringLowerer().callReturningString(fn("tz_os_string", voidTy, {ptr, i32}),
                                                           {values[0]});
    break;
  case Builtin::OsNumber:
    result = lowering_.builder_.CreateCall(fn("tz_os_number", f64, {i32}), {values[0]});
    break;
  case Builtin::OsAvailableParallelism:
    result = lowering_.builder_.CreateCall(fn("tz_os_available_parallelism", i32, {}));
    break;
  case Builtin::OsCpus: {
    const Type *element = call->type->inner();
    result = lowering_.builder_.CreateCall(
        fn("tz_os_cpus", ptr, {ptr, ptr}),
        {lowering_.typeDescriptors().typeDescriptor(element), maker(element)});
    break;
  }
  case Builtin::OsLoadavg:
    result = lowering_.builder_.CreateCall(
        fn("tz_os_loadavg", ptr, {ptr}),
        {lowering_.typeDescriptors().typeDescriptor(Type::doubleTy())});
    break;
  case Builtin::OsUserInfo:
    result = lowering_.builder_.CreateCall(fn("tz_os_user_info", ptr, {ptr}), {maker(call->type)});
    break;
  default:
    llvm_unreachable("not a built-in of node:os");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    lowering_.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return result;
}

}
