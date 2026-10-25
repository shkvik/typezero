#include "compiler/lower/expr/call_lowerer/call_lowerer.h"
#include "compiler/library/builtin_modules/builtin_module_lowerer/builtin_module_lowerer.h"
#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/library/console/console_lowerer/console_lowerer.h"
#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/errors/error_lowerer/error_lowerer.h"
#include "compiler/lower/errors/throw_lowerer/throw_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
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
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

CallLowerer::CallLowerer(Lowering &lowering) : lowering_(lowering) {}

llvm::Value *CallLowerer::emitCall(CallExpr *call) {
  // A call of a built-in of a topic of the library: its code is the topic's.
  if (const Feature *feature = lowering_.features_->lowering(call->builtin)) {
    return feature->lowerCall(lowering_, call);
  }
  switch (call->builtin) {
  case Builtin::StringOf:
    // Checker already converted the argument to a string.
    return call->args.empty() ? lowering_.stringLowerer().stringLiteral(u"")
                              : lowering_.exprLowerer().emitExpr(call->args[0]);
  case Builtin::NumberOf:
  case Builtin::ParseInt:
  case Builtin::ParseFloat:
  case Builtin::IsNaN:
  case Builtin::IsFinite:
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
    return lowering_.numberLowerer().emitNumberBuiltin(call);
  case Builtin::None:
    break;
  case Builtin::TupleJoin:
    return lowering_.unknownLowerer().emitTupleJoin(call);
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
    return lowering_.callbackMethodLowerer().emitCallbackMethod(call);
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
    return lowering_.objectLowerer().emitObjectCall(call);
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
    return lowering_.iteratorLowerer().emitIteratorBuiltin(call);
  default:
    if (isAsyncBuiltin(call->builtin)) {
      return lowering_.asyncBuiltinLowerer().emitAsyncBuiltin(call);
    }
    if (isDictBuiltin(call->builtin)) {
      return lowering_.dictMethodLowerer().emitDictCall(call);
    }
    if (call->builtin == Builtin::ErrorStack || call->builtin == Builtin::ErrorToString) {
      return lowering_.errorLowerer().emitErrorBuiltin(call);
    }
    if (cast<MemberExpr>(call->callee)->object->type->isArray()) {
      return lowering_.arrayMethodLowerer().emitArrayMethod(call);
    }
    return lowering_.stringMethodLowerer().emitStringMethod(call);
  }

  if (call->superCall && !isa<MemberExpr>(call->callee)) {
    return lowering_.classLowerer().emitSuperCall(call);
  }
  if (call->method) {
    return lowering_.classLowerer().emitMethodCall(call);
  }
  if (!call->unionMethods.empty()) {
    return lowering_.unionMemberLowerer().emitUnionMethodCall(call);
  }
  if (!call->func) {
    return lowering_.closureLowerer().emitValueCall(call);
  }
  // The callee takes ownership of its arguments. Parameters left out get
  // undefined (and then their default values).
  const FuncSymbol *func = call->func;
  std::vector<llvm::Value *> args = lowering_.throwLowerer().emitHeldValues(call->args);
  for (size_t i = args.size(); i < func->params.size(); ++i) {
    args.push_back(lowering_.optionalLowerer().undefinedValue(func->params[i]));
  }
  llvm::CallInst *result;
  if (func->decl->isClosure()) {
    // A function declared in a block, or a constant arrow: its code is
    // known, its environment comes from where it is called.
    args.insert(args.begin(), lowering_.closureLowerer().closureEnv(call));
    result =
        lowering_.builder_.CreateCall(lowering_.closureLowerer().closureCode(func->decl), args);
  } else {
    result = lowering_.builder_.CreateCall(lowering_.functions_.lookup(func), args);
  }
  lowering_.emitThrowCheckIf(lowering_.mayThrow(func->decl) && call != lowering_.returnCall_);

  return result->getType()->isVoidTy() ? nullptr : result;
}

} // namespace tz
