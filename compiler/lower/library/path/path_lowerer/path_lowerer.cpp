#include "compiler/lower/library/path/path_lowerer/path_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

namespace tz {

const std::vector<MakerFields> &PathLowerer::pathObjects() {
  static const std::vector<MakerFields> table = {
      {nullptr, {"root", "dir", "base", "ext", "name"}},
  };

  return table;
}

PathLowerer::PathLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *PathLowerer::emitPathCall(CallExpr *call) {
  llvm::Type *voidTy = this->lowering.builder.getVoidTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  llvm::Type *i64 = this->lowering.builder.getInt64Ty();
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  std::vector<llvm::Value *> values = this->lowering.exprLowerer().emitExprs(call->args);
  auto obj = [&](size_t i) { return this->lowering.stringLowerer().stringObj(values[i]); };
  auto meta = [&](size_t i) { return this->lowering.stringLowerer().stringMeta(values[i]); };
  auto fn = [&](llvm::StringRef name, llvm::Type *ret, llvm::ArrayRef<llvm::Type *> params) {
    return this->lowering.processLowerer().processFunction(name, ret, params);
  };

  llvm::Value *result = nullptr;
  switch (call->builtin()) {
    case Builtin::PathJoin:
    case Builtin::PathResolve:
      result = this->lowering.stringLowerer().callReturningString(
          fn(call->builtin() == Builtin::PathJoin ? "tz_path_join" : "tz_path_resolve", voidTy,
             {ptr, ptr, i64}),
          {values[1], values[0]});
      break;
    case Builtin::PathNormalize:
    case Builtin::PathDirname:
    case Builtin::PathExtname:
    case Builtin::PathToNamespacedPath: {
      const char *name = call->builtin() == Builtin::PathNormalize ? "tz_path_normalize"
                         : call->builtin() == Builtin::PathDirname ? "tz_path_dirname"
                         : call->builtin() == Builtin::PathExtname ? "tz_path_extname"
                                                                   : "tz_path_to_namespaced";
      result = this->lowering.stringLowerer().callReturningString(
          fn(name, voidTy, {ptr, ptr, i64, i64}), {obj(1), meta(1), values[0]});
      break;
    }
    case Builtin::PathIsAbsolute:
      result = this->lowering.builder.CreateICmpNE(
          this->lowering.builder.CreateCall(fn("tz_path_is_absolute", i32, {ptr, i64, i64}),
                                            {obj(1), meta(1), values[0]}),
          this->lowering.builder.getInt32(0));
      break;
    case Builtin::PathBasename: {
      bool suffix = values.size() > 2;
      llvm::Value *null = llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
      result = this->lowering.stringLowerer().callReturningString(
          fn("tz_path_basename", voidTy, {ptr, ptr, i64, ptr, i64, i32, i64}),
          {obj(1), meta(1), suffix ? obj(2) : null,
           suffix ? meta(2) : this->lowering.builder.getInt64(0),
           this->lowering.builder.getInt32(suffix ? 1 : 0), values[0]});
      break;
    }
    case Builtin::PathRelative:
      result = this->lowering.stringLowerer().callReturningString(
          fn("tz_path_relative", voidTy, {ptr, ptr, i64, ptr, i64, i64}),
          {obj(1), meta(1), obj(2), meta(2), values[0]});
      break;
    case Builtin::PathParse:
      result = this->lowering.builder.CreateCall(
          fn("tz_path_parse", ptr, {ptr, i64, i64, ptr}),
          {obj(1), meta(1), values[0],
           this->lowering.objectMaker().maker(call->type, PathLowerer::pathObjects())});
      break;
    case Builtin::PathFormat: {
      const Type *object = call->args[1]->type;
      const Type *optional = Type::optional(Type::string());
      llvm::Type *part = this->lowering.moduleBuilder().lower(optional);
      auto *partsType = llvm::ArrayType::get(part, 5);
      llvm::AllocaInst *parts =
          this->lowering.functionFrame().createStackSlot(partsType, "format.parts");
      static const llvm::StringLiteral names[] = {"root", "dir", "base", "ext", "name"};
      for (unsigned i = 0; i < 5; ++i) {
        const Field *field = object->field(names[i]);
        llvm::Value *value;
        if (!field) {
          value = this->lowering.optionalLowerer().undefinedValue(optional);
        } else {
          value = this->lowering.builder.CreateLoad(
              this->lowering.moduleBuilder().lower(field->type),
              this->lowering.objectLayouts().fieldAddress(values[1], object, names[i]));
          if (!field->type->isOptional()) {
            value = this->lowering.optionalLowerer().makeOptional(
                value, this->lowering.builder.getTrue(), optional);
          }
        }
        this->lowering.builder.CreateStore(
            value, this->lowering.builder.CreateConstInBoundsGEP2_32(partsType, parts, 0, i));
      }
      result = this->lowering.stringLowerer().callReturningString(
          fn("tz_path_format", voidTy, {ptr, ptr, i64}), {parts, values[0]});
      break;
    }
    default:
      llvm_unreachable("not a built-in of node:path");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    this->lowering.refCounts().releaseValue(values[i], call->args[i]->type);
  }

  return result;
}

}
