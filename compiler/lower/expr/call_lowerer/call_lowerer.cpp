#include "compiler/lower/expr/call_lowerer/call_lowerer.h"
#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/lower/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/library/console/console_lowerer/console_lowerer.h"
#include "compiler/lower/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/lower/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/lower/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/library/lowerer_registry/lowerer_registry.h"
#include "compiler/lower/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/values/string_method_lowerer/string_method_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"

using llvm::cast;
using llvm::isa;

namespace tz {

CallLowerer::CallLowerer(Lowering &owner)
    : lowering(owner) {}

llvm::Value *CallLowerer::emitCall(CallExpr *call) {
  const LibraryLowerer *lowerer = this->lowering.lowerers->find(call->builtin());
  if (lowerer != nullptr) {
    return lowerer->lowerCall(this->lowering, call);
  }
  switch (call->builtin()) {
    case Builtin::StringOf:
      return call->args.empty() ? this->lowering.stringLowerer().stringLiteral(u"")
                                : this->lowering.exprLowerer().emitExpr(call->args[0]);
    case Builtin::NumberOf:
    case Builtin::ParseInt:
    case Builtin::ParseFloat:
    case Builtin::IsInteger:
    case Builtin::IsSafeInteger:
    case Builtin::FromCharCode:
    case Builtin::FromCodePoint:
    case Builtin::UriEncodeComponent:
    case Builtin::UriEncode:
    case Builtin::UriDecodeComponent:
    case Builtin::UriDecode:
    case Builtin::NumToFixed:
    case Builtin::NumToRadix:
    case Builtin::NumConvert:
      return this->lowering.numberLowerer().emitNumberBuiltin(call);
    case Builtin::None:
      break;
    case Builtin::TupleJoin:
      return this->lowering.unknownLowerer().emitTupleJoin(call);
    case Builtin::ArrForEach:
    case Builtin::ArrMap:
    case Builtin::ArrFilter:
    case Builtin::ArrFlatMap:
    case Builtin::ArrReduce:
    case Builtin::ArrReduceRight:
    case Builtin::ArrFind:
    case Builtin::ArrFindIndex:
    case Builtin::ArrFindLast:
    case Builtin::ArrFindLastIndex:
    case Builtin::ArrSome:
    case Builtin::ArrEvery:
    case Builtin::ArrSortWith:
      return this->lowering.callbackMethodLowerer().emitCallbackMethod(call);
    case Builtin::ObjectKeys:
    case Builtin::ObjectValues:
    case Builtin::ObjectEntries:
      return this->lowering.objectLowerer().emitObjectCall(call);
    case Builtin::ArrKeys:
    case Builtin::ArrValues:
    case Builtin::ArrEntries:
    case Builtin::IterFrom:
    case Builtin::ArrayFrom:
    case Builtin::IterNext:
    case Builtin::IterMap:
    case Builtin::IterFilter:
    case Builtin::IterTake:
    case Builtin::IterDrop:
    case Builtin::IterFlatMap:
    case Builtin::IterToArray:
    case Builtin::IterForEach:
    case Builtin::IterReduce:
    case Builtin::IterSome:
    case Builtin::IterEvery:
    case Builtin::IterFind:
      return this->lowering.iteratorLowerer().emitIteratorBuiltin(call);
    default:
      if (BuiltinGroups::isAsyncBuiltin(call->builtin())) {
        return this->lowering.asyncBuiltinLowerer().emitAsyncBuiltin(call);
      }
      if (BuiltinGroups::isDictBuiltin(call->builtin())) {
        return this->lowering.dictMethodLowerer().emitDictCall(call);
      }
      if (cast<MemberExpr>(call->callee)->object->type->isArray()) {
        return this->lowering.arrayMethodLowerer().emitArrayMethod(call);
      }
      return this->lowering.stringMethodLowerer().emitStringMethod(call);
  }

  if (call->superCall() && !isa<MemberExpr>(call->callee)) {
    return this->lowering.classLowerer().emitSuperCall(call);
  }
  if (call->method()) {
    return this->lowering.classLowerer().emitMethodCall(call);
  }
  if (!call->unionMethods().empty()) {
    return this->lowering.unionMemberLowerer().emitUnionMethodCall(call);
  }
  if (!call->func()) {
    return this->lowering.closureLowerer().emitValueCall(call);
  }
  const FuncSymbol *func = call->func();
  std::vector<llvm::Value *> args = this->lowering.exprLowerer().emitExprs(call->args);
  for (size_t i = args.size(); i < func->params.size(); ++i) {
    args.push_back(this->lowering.optionalLowerer().undefinedValue(func->params[i]));
  }
  llvm::CallInst *result;
  this->lowering.markAwaitedCall(call, func->decl);
  if (func->decl->isClosure()) {
    args.insert(args.begin(), this->lowering.closureLowerer().closureEnv(call));
    result = this->lowering.builder.CreateCall(
        this->lowering.closureLowerer().closureCode(func->decl), args);
  } else {
    result = this->lowering.builder.CreateCall(this->lowering.functions_.lookup(func), args);
  }

  return result->getType()->isVoidTy() ? nullptr : result;
}

}
