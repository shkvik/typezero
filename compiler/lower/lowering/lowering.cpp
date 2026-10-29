#include "compiler/lower/lowering/lowering.h"
#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/library/console/console_lowerer/console_lowerer.h"
#include "compiler/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/library/date/date_lowerer/date_lowerer.h"
#include "compiler/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/library/json/json_lowerer/json_lowerer.h"
#include "compiler/library/math/math_lowerer/math_lowerer.h"
#include "compiler/library/os/os_lowerer/os_lowerer.h"
#include "compiler/library/path/path_lowerer/path_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/library/regex/regex_native_lowerer/regex_native_lowerer.h"
#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "compiler/library/result/result_lowerer/result_lowerer.h"
#include "compiler/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/expr/assign_lowerer/assign_lowerer.h"
#include "compiler/lower/expr/binary_lowerer/binary_lowerer.h"
#include "compiler/lower/expr/call_lowerer/call_lowerer.h"
#include "compiler/lower/expr/equality_lowerer/equality_lowerer.h"
#include "compiler/lower/expr/expr_lowerer/expr_lowerer.h"
#include "compiler/lower/expr/member_lowerer/member_lowerer.h"
#include "compiler/lower/expr/truth_lowerer/truth_lowerer.h"
#include "compiler/lower/expr/unary_lowerer/unary_lowerer.h"
#include "compiler/lower/function_builder/function_builder.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/functions/env_lowerer/env_lowerer.h"
#include "compiler/lower/functions/tail_call_lowerer/tail_call_lowerer.h"
#include "compiler/lower/iterators/async_generator_lowerer/async_generator_lowerer.h"
#include "compiler/lower/iterators/coroutine_lowerer/coroutine_lowerer.h"
#include "compiler/lower/iterators/generator_lowerer/generator_lowerer.h"
#include "compiler/lower/iterators/inline_generator_lowerer/inline_generator_lowerer.h"
#include "compiler/lower/iterators/iterator_lowerer/iterator_lowerer.h"
#include "compiler/lower/iterators/lazy_chain_lowerer/lazy_chain_lowerer.h"
#include "compiler/lower/layouts/object_layouts/object_layouts.h"
#include "compiler/lower/layouts/type_descriptors/type_descriptors.h"
#include "compiler/lower/memory/group_lowerer/group_lowerer.h"
#include "compiler/lower/memory/memory_debug/memory_debug.h"
#include "compiler/lower/memory/publish_lowerer/publish_lowerer.h"
#include "compiler/lower/memory/ref_counts/ref_counts.h"
#include "compiler/lower/memory/store_lowerer/store_lowerer.h"
#include "compiler/lower/module_builder/module_builder.h"
#include "compiler/lower/objects/dict_lowerer/dict_lowerer.h"
#include "compiler/lower/objects/dict_method_lowerer/dict_method_lowerer.h"
#include "compiler/lower/objects/object_lowerer/object_lowerer.h"
#include "compiler/lower/objects/object_maker/object_maker.h"
#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/runtime_functions/runtime_functions.h"
#include "compiler/lower/stmt/exit_lowerer/exit_lowerer.h"
#include "compiler/lower/stmt/loop_lowerer/loop_lowerer.h"
#include "compiler/lower/stmt/stmt_lowerer/stmt_lowerer.h"
#include "compiler/lower/stmt/switch_lowerer/switch_lowerer.h"
#include "compiler/lower/types/optional_lowerer/optional_lowerer.h"
#include "compiler/lower/types/union_layout/union_layout.h"
#include "compiler/lower/types/union_lowerer/union_lowerer.h"
#include "compiler/lower/types/union_member_lowerer/union_member_lowerer.h"
#include "compiler/lower/types/unknown_cast_lowerer/unknown_cast_lowerer.h"
#include "compiler/lower/types/unknown_lowerer/unknown_lowerer.h"
#include "compiler/lower/values/array_lowerer/array_lowerer.h"
#include "compiler/lower/values/array_method_lowerer/array_method_lowerer.h"
#include "compiler/lower/values/callback_method_lowerer/callback_method_lowerer.h"
#include "compiler/lower/values/number_lowerer/number_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/values/string_method_lowerer/string_method_lowerer.h"
#include "compiler/memory/memory_plan/memory_plan.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <array>

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

Lowering::Lowering(llvm::LLVMContext &ctx, llvm::StringRef moduleName,
                   const llvm::DataLayout &layout)
    : ctx_(ctx), module_(std::make_unique<llvm::Module>(moduleName, ctx)), builder_(ctx) {
  moduleBuilderPart_ = std::make_unique<ModuleBuilder>(*this);
  functionBuilderPart_ = std::make_unique<FunctionBuilder>(*this);
  runtimeFunctionsPart_ = std::make_unique<RuntimeFunctions>(*this);
  functionFramePart_ = std::make_unique<FunctionFrame>(*this);
  refCountsPart_ = std::make_unique<RefCounts>(*this);
  optionalLowererPart_ = std::make_unique<OptionalLowerer>(*this);
  stmtLowererPart_ = std::make_unique<StmtLowerer>(*this);
  loopLowererPart_ = std::make_unique<LoopLowerer>(*this);
  switchLowererPart_ = std::make_unique<SwitchLowerer>(*this);
  exprLowererPart_ = std::make_unique<ExprLowerer>(*this);
  unaryLowererPart_ = std::make_unique<UnaryLowerer>(*this);
  binaryLowererPart_ = std::make_unique<BinaryLowerer>(*this);
  equalityLowererPart_ = std::make_unique<EqualityLowerer>(*this);
  truthLowererPart_ = std::make_unique<TruthLowerer>(*this);
  assignLowererPart_ = std::make_unique<AssignLowerer>(*this);
  callLowererPart_ = std::make_unique<CallLowerer>(*this);
  consoleLowererPart_ = std::make_unique<ConsoleLowerer>(*this);
  stringLowererPart_ = std::make_unique<StringLowerer>(*this);
  stringMethodLowererPart_ = std::make_unique<StringMethodLowerer>(*this);
  memberLowererPart_ = std::make_unique<MemberLowerer>(*this);
  typeDescriptorsPart_ = std::make_unique<TypeDescriptors>(*this);
  arrayLowererPart_ = std::make_unique<ArrayLowerer>(*this);
  arrayMethodLowererPart_ = std::make_unique<ArrayMethodLowerer>(*this);
  callbackMethodLowererPart_ = std::make_unique<CallbackMethodLowerer>(*this);
  promiseLowererPart_ = std::make_unique<PromiseLowerer>(*this);
  asyncLowererPart_ = std::make_unique<AsyncLowerer>(*this);
  promiseCombineLowererPart_ = std::make_unique<PromiseCombineLowerer>(*this);
  reactionLowererPart_ = std::make_unique<ReactionLowerer>(*this);
  asyncBuiltinLowererPart_ = std::make_unique<AsyncBuiltinLowerer>(*this);
  asyncGeneratorLowererPart_ = std::make_unique<AsyncGeneratorLowerer>(*this);
  mathLowererPart_ = std::make_unique<MathLowerer>(*this);
  numberLowererPart_ = std::make_unique<NumberLowerer>(*this);
  classDescriptorsPart_ = std::make_unique<ClassDescriptors>(*this);
  classLowererPart_ = std::make_unique<ClassLowerer>(*this);
  closureLowererPart_ = std::make_unique<ClosureLowerer>(*this);
  envLowererPart_ = std::make_unique<EnvLowerer>(*this);
  groupLowererPart_ = std::make_unique<GroupLowerer>(*this);
  storeLowererPart_ = std::make_unique<StoreLowerer>(*this);
  weakRefLowererPart_ = std::make_unique<WeakRefLowerer>(*this);
  memoryDebugPart_ = std::make_unique<MemoryDebug>(*this);
  publishLowererPart_ = std::make_unique<PublishLowerer>(*this);
  decoratorLowererPart_ = std::make_unique<DecoratorLowerer>(*this);
  dictLowererPart_ = std::make_unique<DictLowerer>(*this);
  dictMethodLowererPart_ = std::make_unique<DictMethodLowerer>(*this);
  inheritLowererPart_ = std::make_unique<InheritLowerer>(*this);
  coroutineLowererPart_ = std::make_unique<CoroutineLowerer>(*this);
  generatorLowererPart_ = std::make_unique<GeneratorLowerer>(*this);
  lazyChainLowererPart_ = std::make_unique<LazyChainLowerer>(*this);
  inlineGeneratorLowererPart_ = std::make_unique<InlineGeneratorLowerer>(*this);
  iteratorLowererPart_ = std::make_unique<IteratorLowerer>(*this);
  objectLayoutsPart_ = std::make_unique<ObjectLayouts>(*this);
  objectLowererPart_ = std::make_unique<ObjectLowerer>(*this);
  tailCallLowererPart_ = std::make_unique<TailCallLowerer>(*this);
  placeLowererPart_ = std::make_unique<PlaceLowerer>(*this);
  exitLowererPart_ = std::make_unique<ExitLowerer>(*this);
  unionLayoutPart_ = std::make_unique<UnionLayout>(*this);
  unionLowererPart_ = std::make_unique<UnionLowerer>(*this);
  unionMemberLowererPart_ = std::make_unique<UnionMemberLowerer>(*this);
  unknownLowererPart_ = std::make_unique<UnknownLowerer>(*this);
  bytesLowererPart_ = std::make_unique<BytesLowerer>(*this);
  bytesCallbackLowererPart_ = std::make_unique<BytesCallbackLowerer>(*this);
  bytesCallLowererPart_ = std::make_unique<BytesCallLowerer>(*this);
  childLowererPart_ = std::make_unique<ChildLowerer>(*this);
  cryptoLowererPart_ = std::make_unique<CryptoLowerer>(*this);
  dateLowererPart_ = std::make_unique<DateLowerer>(*this);
  decimalLowererPart_ = std::make_unique<DecimalLowerer>(*this);
  libraryCallLowererPart_ = std::make_unique<LibraryCallLowerer>(*this);
  intlLowererPart_ = std::make_unique<IntlLowerer>(*this);
  intlCallLowererPart_ = std::make_unique<IntlCallLowerer>(*this);
  jsonLowererPart_ = std::make_unique<JsonLowerer>(*this);
  fsPromisesLowererPart_ = std::make_unique<FsPromisesLowerer>(*this);
  fsLowererPart_ = std::make_unique<FsLowerer>(*this);
  pathLowererPart_ = std::make_unique<PathLowerer>(*this);
  osLowererPart_ = std::make_unique<OsLowerer>(*this);
  objectMakerPart_ = std::make_unique<ObjectMaker>(*this);
  processLowererPart_ = std::make_unique<ProcessLowerer>(*this);
  unknownCastLowererPart_ = std::make_unique<UnknownCastLowerer>(*this);
  reflectArgsLowererPart_ = std::make_unique<ReflectArgsLowerer>(*this);
  classInfoLowererPart_ = std::make_unique<ClassInfoLowerer>(*this);
  reflectDispatchLowererPart_ = std::make_unique<ReflectDispatchLowerer>(*this);
  reflectLowererPart_ = std::make_unique<ReflectLowerer>(*this);
  regexLowererPart_ = std::make_unique<RegexLowerer>(*this);
  regexNativeLowererPart_ = std::make_unique<RegexNativeLowerer>(*this);
  resultLowererPart_ = std::make_unique<ResultLowerer>(*this);
  resultCombineLowererPart_ = std::make_unique<ResultCombineLowerer>(*this);
  errSitesPart_ = std::make_unique<ErrSites>(*this);
  streamLowererPart_ = std::make_unique<StreamLowerer>(*this);
  tzModulesLowererPart_ = std::make_unique<TzModulesLowerer>(*this);
  module_->setDataLayout(layout);
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Type *i32 = builder_.getInt32Ty();
  llvm::Type *i64 = builder_.getInt64Ty();
  strTy_ = llvm::StructType::create(ctx, {ptr, i64}, "tz.string");
  arrayTy_ = llvm::StructType::create(ctx, {i64, ptr, ptr, i32, i32, ptr}, "tz.array");
  typeDescTy_ = llvm::StructType::create(ctx, {i32, i32, ptr}, "tz.type");
  fnTy_ = llvm::StructType::create(ctx, {ptr, ptr}, "tz.fn");
  fnInfoTy_ = llvm::StructType::create(ctx, {ptr, strTy_}, "tz.fninfo");
  decTy_ = llvm::StructType::create(ctx, {i64, i64}, "tz.decimal");
  fieldDescTy_ = llvm::StructType::create(ctx, {ptr, i32, i32, ptr}, "tz.field");
  layoutDescTy_ = llvm::StructType::create(ctx, {ptr, ptr, i32, i32, ptr, i32, i32}, "tz.layout");
  promiseTy_ = llvm::StructType::create(ctx, {ptr, ptr, ptr, i32, i32}, "tz.promise");
  promiseObjTy_ = llvm::StructType::create(ctx, {i64, ptr, ptr, ptr, i32, i32}, "tz.Promise");
}

void Lowering::setParallelPlan(const ParallelPlan *plan) {
  plan_ = plan;
  parallel_ =
      plan && llvm::any_of(plan->pieces, [](const ParallelPlan::Piece &p) { return !p.alone; });
  if (!parallel_) {
    return;
  }
  for (const ParallelPlan::Piece &piece : plan->pieces) {
    if (piece.alone) {
      continue;
    }
    if (piece.start) {
      aheadStarts_.try_emplace(piece.start, uint32_t(aheadStarts_.size() + 1));
      if (piece.fn) {
        aheadFunctions_.insert(piece.fn);
      }
      if (piece.inOrderWith.empty() && piece.orderedAt.empty()) {
        freeAwaits_.insert(piece.start);
      }
    }
    for (llvm::SMLoc at : piece.orderedAt) {
      orderedLocs_.insert(at.getPointer());
    }
  }
}

void Lowering::orderAt(llvm::SMLoc loc) {
  if (!parallel_ || !function_ || !orderedLocs_.contains(loc.getPointer())) {
    return;
  }
  builder_.CreateCall(processLowerer().processFunction("tz_order_enter", builder_.getVoidTy(), {}));
}

Lowering::~Lowering() = default;

}
