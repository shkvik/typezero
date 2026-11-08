#include "compiler/lower/lowering/lowering.h"
#include "compiler/lower/async/async_builtin_lowerer/async_builtin_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/async/promise_combine_lowerer/promise_combine_lowerer.h"
#include "compiler/lower/async/promise_lowerer/promise_lowerer.h"
#include "compiler/lower/async/reaction_lowerer/reaction_lowerer.h"
#include "compiler/lower/classes/class_descriptors/class_descriptors.h"
#include "compiler/lower/classes/class_lowerer/class_lowerer.h"
#include "compiler/lower/classes/decorator_lowerer/decorator_lowerer.h"
#include "compiler/lower/classes/inherit_lowerer/inherit_lowerer.h"
#include "compiler/lower/debug_info/debug_info.h"
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
#include "compiler/lower/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/lower/library/bytes/bytes_callback_lowerer/bytes_callback_lowerer.h"
#include "compiler/lower/library/bytes/bytes_lowerer/bytes_lowerer.h"
#include "compiler/lower/library/child_process/child_lowerer/child_lowerer.h"
#include "compiler/lower/library/console/console_lowerer/console_lowerer.h"
#include "compiler/lower/library/crypto/crypto_lowerer/crypto_lowerer.h"
#include "compiler/lower/library/date/date_lowerer/date_lowerer.h"
#include "compiler/lower/library/decimal/decimal_lowerer/decimal_lowerer.h"
#include "compiler/lower/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/lower/library/fs/fs_promises_lowerer/fs_promises_lowerer.h"
#include "compiler/lower/library/intl/intl_call_lowerer/intl_call_lowerer.h"
#include "compiler/lower/library/intl/intl_lowerer/intl_lowerer.h"
#include "compiler/lower/library/json/json_lowerer/json_lowerer.h"
#include "compiler/lower/library/json/json_writers/json_writers.h"
#include "compiler/lower/library/math/math_lowerer/math_lowerer.h"
#include "compiler/lower/library/os/os_lowerer/os_lowerer.h"
#include "compiler/lower/library/path/path_lowerer/path_lowerer.h"
#include "compiler/lower/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/library/reflect/class_info_lowerer/class_info_lowerer.h"
#include "compiler/lower/library/reflect/reflect_args_lowerer/reflect_args_lowerer.h"
#include "compiler/lower/library/reflect/reflect_dispatch_lowerer/reflect_dispatch_lowerer.h"
#include "compiler/lower/library/reflect/reflect_lowerer/reflect_lowerer.h"
#include "compiler/lower/library/regex/regex_lowerer/regex_lowerer.h"
#include "compiler/lower/library/regex/regex_native_lowerer/regex_native_lowerer.h"
#include "compiler/lower/library/result/err_sites/err_sites.h"
#include "compiler/lower/library/result/result_combine_lowerer/result_combine_lowerer.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/library/stream/stream_lowerer/stream_lowerer.h"
#include "compiler/lower/library/tz_modules/library_call_lowerer/library_call_lowerer.h"
#include "compiler/lower/library/tz_modules/tz_modules_lowerer/tz_modules_lowerer.h"
#include "compiler/lower/library/weakref/weak_ref_lowerer/weak_ref_lowerer.h"
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
    : ctx(ctx),
      module_(std::make_unique<llvm::Module>(moduleName, ctx)),
      builder(ctx) {
  this->moduleBuilderPart = std::make_unique<ModuleBuilder>(*this);
  this->functionBuilderPart = std::make_unique<FunctionBuilder>(*this);
  this->runtimeFunctionsPart = std::make_unique<RuntimeFunctions>(*this);
  this->functionFramePart = std::make_unique<FunctionFrame>(*this);
  this->refCountsPart = std::make_unique<RefCounts>(*this);
  this->optionalLowererPart = std::make_unique<OptionalLowerer>(*this);
  this->stmtLowererPart = std::make_unique<StmtLowerer>(*this);
  this->loopLowererPart = std::make_unique<LoopLowerer>(*this);
  this->switchLowererPart = std::make_unique<SwitchLowerer>(*this);
  this->exprLowererPart = std::make_unique<ExprLowerer>(*this);
  this->unaryLowererPart = std::make_unique<UnaryLowerer>(*this);
  this->binaryLowererPart = std::make_unique<BinaryLowerer>(*this);
  this->equalityLowererPart = std::make_unique<EqualityLowerer>(*this);
  this->truthLowererPart = std::make_unique<TruthLowerer>(*this);
  this->assignLowererPart = std::make_unique<AssignLowerer>(*this);
  this->callLowererPart = std::make_unique<CallLowerer>(*this);
  this->consoleLowererPart = std::make_unique<ConsoleLowerer>(*this);
  this->stringLowererPart = std::make_unique<StringLowerer>(*this);
  this->stringMethodLowererPart = std::make_unique<StringMethodLowerer>(*this);
  this->memberLowererPart = std::make_unique<MemberLowerer>(*this);
  this->typeDescriptorsPart = std::make_unique<TypeDescriptors>(*this);
  this->arrayLowererPart = std::make_unique<ArrayLowerer>(*this);
  this->arrayMethodLowererPart = std::make_unique<ArrayMethodLowerer>(*this);
  this->callbackMethodLowererPart = std::make_unique<CallbackMethodLowerer>(*this);
  this->promiseLowererPart = std::make_unique<PromiseLowerer>(*this);
  this->asyncLowererPart = std::make_unique<AsyncLowerer>(*this);
  this->promiseCombineLowererPart = std::make_unique<PromiseCombineLowerer>(*this);
  this->reactionLowererPart = std::make_unique<ReactionLowerer>(*this);
  this->asyncBuiltinLowererPart = std::make_unique<AsyncBuiltinLowerer>(*this);
  this->asyncGeneratorLowererPart = std::make_unique<AsyncGeneratorLowerer>(*this);
  this->mathLowererPart = std::make_unique<MathLowerer>(*this);
  this->numberLowererPart = std::make_unique<NumberLowerer>(*this);
  this->classDescriptorsPart = std::make_unique<ClassDescriptors>(*this);
  this->classLowererPart = std::make_unique<ClassLowerer>(*this);
  this->closureLowererPart = std::make_unique<ClosureLowerer>(*this);
  this->envLowererPart = std::make_unique<EnvLowerer>(*this);
  this->groupLowererPart = std::make_unique<GroupLowerer>(*this);
  this->storeLowererPart = std::make_unique<StoreLowerer>(*this);
  this->weakRefLowererPart = std::make_unique<WeakRefLowerer>(*this);
  this->memoryDebugPart = std::make_unique<MemoryDebug>(*this);
  this->publishLowererPart = std::make_unique<PublishLowerer>(*this);
  this->decoratorLowererPart = std::make_unique<DecoratorLowerer>(*this);
  this->dictLowererPart = std::make_unique<DictLowerer>(*this);
  this->dictMethodLowererPart = std::make_unique<DictMethodLowerer>(*this);
  this->inheritLowererPart = std::make_unique<InheritLowerer>(*this);
  this->coroutineLowererPart = std::make_unique<CoroutineLowerer>(*this);
  this->generatorLowererPart = std::make_unique<GeneratorLowerer>(*this);
  this->lazyChainLowererPart = std::make_unique<LazyChainLowerer>(*this);
  this->inlineGeneratorLowererPart = std::make_unique<InlineGeneratorLowerer>(*this);
  this->iteratorLowererPart = std::make_unique<IteratorLowerer>(*this);
  this->objectLayoutsPart = std::make_unique<ObjectLayouts>(*this);
  this->objectLowererPart = std::make_unique<ObjectLowerer>(*this);
  this->tailCallLowererPart = std::make_unique<TailCallLowerer>(*this);
  this->placeLowererPart = std::make_unique<PlaceLowerer>(*this);
  this->exitLowererPart = std::make_unique<ExitLowerer>(*this);
  this->unionLayoutPart = std::make_unique<UnionLayout>(*this);
  this->unionLowererPart = std::make_unique<UnionLowerer>(*this);
  this->unionMemberLowererPart = std::make_unique<UnionMemberLowerer>(*this);
  this->unknownLowererPart = std::make_unique<UnknownLowerer>(*this);
  this->bytesLowererPart = std::make_unique<BytesLowerer>(*this);
  this->bytesCallbackLowererPart = std::make_unique<BytesCallbackLowerer>(*this);
  this->bytesCallLowererPart = std::make_unique<BytesCallLowerer>(*this);
  this->childLowererPart = std::make_unique<ChildLowerer>(*this);
  this->cryptoLowererPart = std::make_unique<CryptoLowerer>(*this);
  this->dateLowererPart = std::make_unique<DateLowerer>(*this);
  this->decimalLowererPart = std::make_unique<DecimalLowerer>(*this);
  this->libraryCallLowererPart = std::make_unique<LibraryCallLowerer>(*this);
  this->intlLowererPart = std::make_unique<IntlLowerer>(*this);
  this->intlCallLowererPart = std::make_unique<IntlCallLowerer>(*this);
  this->jsonLowererPart = std::make_unique<JsonLowerer>(*this);
  this->jsonWritersPart = std::make_unique<JsonWriters>(*this);
  this->fsPromisesLowererPart = std::make_unique<FsPromisesLowerer>(*this);
  this->fsLowererPart = std::make_unique<FsLowerer>(*this);
  this->pathLowererPart = std::make_unique<PathLowerer>(*this);
  this->osLowererPart = std::make_unique<OsLowerer>(*this);
  this->objectMakerPart = std::make_unique<ObjectMaker>(*this);
  this->processLowererPart = std::make_unique<ProcessLowerer>(*this);
  this->unknownCastLowererPart = std::make_unique<UnknownCastLowerer>(*this);
  this->reflectArgsLowererPart = std::make_unique<ReflectArgsLowerer>(*this);
  this->classInfoLowererPart = std::make_unique<ClassInfoLowerer>(*this);
  this->reflectDispatchLowererPart = std::make_unique<ReflectDispatchLowerer>(*this);
  this->reflectLowererPart = std::make_unique<ReflectLowerer>(*this);
  this->regexLowererPart = std::make_unique<RegexLowerer>(*this);
  this->regexNativeLowererPart = std::make_unique<RegexNativeLowerer>(*this);
  this->resultLowererPart = std::make_unique<ResultLowerer>(*this);
  this->resultCombineLowererPart = std::make_unique<ResultCombineLowerer>(*this);
  this->errSitesPart = std::make_unique<ErrSites>(*this);
  this->debugInfoPart = std::make_unique<DebugInfo>(*this);
  this->streamLowererPart = std::make_unique<StreamLowerer>(*this);
  this->tzModulesLowererPart = std::make_unique<TzModulesLowerer>(*this);
  module_->setDataLayout(layout);
  llvm::Type *ptr = this->builder.getPtrTy();
  llvm::Type *i32 = this->builder.getInt32Ty();
  llvm::Type *i64 = this->builder.getInt64Ty();
  this->strTy = llvm::StructType::create(ctx, {ptr, i64}, "tz.string");
  this->arrayTy = llvm::StructType::create(ctx, {i64, ptr, ptr, i32, i32, ptr}, "tz.array");
  this->typeDescTy = llvm::StructType::create(ctx, {i32, i32, ptr}, "tz.type");
  this->fnTy = llvm::StructType::create(ctx, {ptr, ptr}, "tz.fn");
  this->fnInfoTy = llvm::StructType::create(ctx, {ptr, this->strTy}, "tz.fninfo");
  this->decTy = llvm::StructType::create(ctx, {i64, i64}, "tz.decimal");
  this->fieldDescTy = llvm::StructType::create(ctx, {ptr, i32, i32, ptr}, "tz.field");
  this->layoutDescTy =
      llvm::StructType::create(ctx, {ptr, ptr, i32, i32, ptr, i32, i32}, "tz.layout");
  this->promiseTy = llvm::StructType::create(ctx, {ptr, ptr, ptr, i32, i32}, "tz.promise");
  this->promiseObjTy = llvm::StructType::create(ctx, {i64, ptr, ptr, ptr, i32, i32}, "tz.Promise");
}

const Expr *Lowering::stripImplicit(const Expr *e) {
  while (auto *c = llvm::dyn_cast_or_null<CastExpr>(e)) {
    if (!c->isImplicit()) {
      break;
    }
    e = c->operand;
  }

  return e;
}

const Expr *Lowering::awaitedOperand(const Expr *e) {
  auto *await = llvm::dyn_cast_or_null<AwaitExpr>(Lowering::stripImplicit(e));
  if (!await) {
    return nullptr;
  }
  const Expr *operand = Lowering::stripImplicit(await->value);
  if (auto *call = llvm::dyn_cast_or_null<CallExpr>(operand);
      call &&
      (call->builtin() == Builtin::LibAhead || call->builtin() == Builtin::LibAheadMovable) &&
      !call->args.empty()) {
    operand = Lowering::stripImplicit(call->args[0]);
  }

  return operand;
}

void Lowering::setParallelPlan(const ParallelPlan *parallelPlan) {
  this->plan = parallelPlan;
  parallel_ = parallelPlan && llvm::any_of(parallelPlan->pieces,
                                           [](const ParallelPlan::Piece &p) { return !p.alone; });
  if (!parallel_) {
    return;
  }
  this->ownedParams.insert(parallelPlan->ownedParams.begin(), parallelPlan->ownedParams.end());
  for (const ParallelPlan::Piece &piece : parallelPlan->pieces) {
    if (piece.alone) {
      continue;
    }
    if (piece.start) {
      this->aheadStarts.try_emplace(piece.start, uint32_t(this->aheadStarts.size() + 1));
      if (piece.fn) {
        this->aheadFunctions.insert(piece.fn);
      }
      if (piece.inOrderWith.empty() && piece.orderedAt.empty()) {
        this->freeAwaits.insert(piece.start);
      }
    }
    for (llvm::SMLoc at : piece.orderedAt) {
      this->orderedLocs.insert(at.getPointer());
    }
  }
}

void Lowering::noteAwaited(const Expr *operand) {
  if (!parallel_) {
    return;
  }
  if (auto *call = llvm::dyn_cast_or_null<CallExpr>(Lowering::stripImplicit(operand))) {
    this->awaitedCalls.insert(call);
  }
}

void Lowering::noteAwaitedNext(const Stmt *declared, const Stmt *next) {
  auto *decl = llvm::dyn_cast_or_null<VarDeclStmt>(declared);
  if (!parallel_ || !decl || decl->decls.size() != 1 || !next) {
    return;
  }
  const VarDeclarator &d = decl->decls[0];
  auto *call = llvm::dyn_cast_or_null<CallExpr>(Lowering::stripImplicit(d.init));
  if (!call || !d.sym || d.destructured() || d.alias) {
    return;
  }
  const Expr *first = nullptr;
  if (auto *s = llvm::dyn_cast<ExprStmt>(next)) {
    first = s->expr;
  } else if (auto *v = llvm::dyn_cast<VarDeclStmt>(next); v && !v->decls.empty()) {
    first = v->decls[0].init;
  } else if (auto *r = llvm::dyn_cast<ReturnStmt>(next)) {
    first = r->value;
  }
  auto *name = llvm::dyn_cast_or_null<NameExpr>(Lowering::awaitedOperand(first));
  if (name && name->var() == d.sym) {
    this->awaitedCalls.insert(call);
  }
}

void Lowering::markAwaitedCall(const CallExpr *call, const FunctionDecl *target) {
  if (!publishesFrame(target) || !this->awaitedCalls.contains(call)) {
    return;
  }
  this->builder.CreateCall(
      processLowerer().processFunction("tz_call_awaited", this->builder.getVoidTy(), {}));
}

void Lowering::orderAt(llvm::SMLoc loc) {
  if (!parallel_ || !this->function || !this->orderedLocs.contains(loc.getPointer())) {
    return;
  }
  this->builder.CreateCall(
      processLowerer().processFunction("tz_order_enter", this->builder.getVoidTy(), {}));
}

Lowering::~Lowering() = default;

}
