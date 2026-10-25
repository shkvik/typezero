#pragma once

#include "compiler/memory/memory_plan/memory_plan.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace tz {

// Whether `expr` is a variable that a method can neither change nor release
// while it runs, so the object in it can be lent to the method.
bool staysDuringCall(const Expr *expr);
// What some code may run and throw (see codegen_throw.cpp).
struct ThrowSummary;
// Whether reading a key of the Json `object` may throw: unless `?.` checked
// it, it may be null or undefined.
bool jsonReadMayThrow(const Expr *object);
// Whether JSON.stringify of a value of `type` may throw: the value may hold
// itself, as a Json or the object of a class may.
bool stringifyMayThrow(const Type *type);
// Whether the locale and the options of toLocaleString or of
// Intl.DateTimeFormat (`args`, without the kind of toLocaleString) may be
// rejected when the program runs (see codegen_date.cpp).
bool dateOptionsMayThrow(llvm::ArrayRef<Expr *> args);
// Whether a built-in of binary data may throw (see codegen_bytes.cpp).
bool bytesMayThrow(const CallExpr *call);
// Whether a call of a built-in of Intl beyond dates may throw: what it is
// given may be rejected when the program runs (see codegen_intl.cpp).
bool intlCallMayThrow(const CallExpr *call);

// Lowers a checked AST to LLVM IR.
//
// Every local variable gets a stack slot (alloca) in the function's entry
// block and is accessed with loads and stores; LLVM's mem2reg pass turns them
// into registers. Top-level code becomes `main`.
//
// Representation of values:
//   int, long, double, boolean   i32, i64, double, i1
//   string                       { ptr obj, i64 meta } (runtime/core/string/string.c)
//   T[]                          ptr to an array header (runtime/core/array/array.c)
//   T | undefined                { T value, i1 has }; the value is zero when
//                                undefined, so it can always be released
//   function                     { ptr info, ptr env }: info is a constant
//                                { ptr code, string name }, env the closure's
//                                environment or null (see codegen_closure.cpp)
//   object                       ptr to { i64 rc, ptr layout, fields... }
//                                (see codegen_object.cpp); an object's
//                                `T | undefined` is that ptr, null when
//                                undefined. An instance of a class is one
//                                too (see codegen_class.cpp).
//   decimal                      { i64 coef, i64 meta } (runtime/library/decimal/decimal/decimal.c,
//                                codegen_decimal.cpp)
//   Result<T, E>                 { T value, E error, i1 ok } in some order
//                                (see codegen_result.cpp); the half not in
//                                use is zero, so both can always be released
//
// Strings, arrays, functions, objects and long decimals are reference
// counted ("managed", as are optionals of them). The ownership rules are
// simple for now and LLVM cleans up part of the overhead:
// - an expression of a managed type produces an owned (+1) value, which its
//   consumer either stores or releases;
// - a variable owns its value: assignment releases the old one, and leaving a
//   scope releases the variables declared in it;
// - an array owns its elements;
// - a call passes ownership of its arguments to the callee, which releases
//   its parameters when it returns;
// - an operand may be borrowed, without retain and release, when it is a
//   plain variable, or a field read through fields from one (`a.b.c`), and
//   nothing evaluated while it is in use can assign to anything.
class ModuleBuilder;
class FunctionBuilder;
class RuntimeFunctions;
class FunctionFrame;
class RefCounts;
class OptionalLowerer;
class StmtLowerer;
class LoopLowerer;
class SwitchLowerer;
class ExprLowerer;
class UnaryLowerer;
class BinaryLowerer;
class EqualityLowerer;
class TruthLowerer;
class AssignLowerer;
class CallLowerer;
class ConsoleLowerer;
class StringLowerer;
class StringMethodLowerer;
class MemberLowerer;
class TypeDescriptors;
class ArrayLowerer;
class ArrayMethodLowerer;
class CallbackMethodLowerer;
class PromiseLowerer;
class AsyncLowerer;
class PromiseCombineLowerer;
class ReactionLowerer;
class ResultAsyncHandler;
class ResultAsyncLowerer;
class AsyncBuiltinLowerer;
class AsyncGeneratorLowerer;
class MathLowerer;
class NumberLowerer;
class ClassDescriptors;
class ClassLowerer;
class ClosureLowerer;
class EnvLowerer;
class GroupLowerer;
class StoreLowerer;
class WeakRefLowerer;
class MemoryDebug;
class PublishLowerer;
class DecoratorLowerer;
class DictLowerer;
class DictMethodLowerer;
class InheritLowerer;
class CoroutineLowerer;
class GeneratorLowerer;
class LazyChainLowerer;
class InlineGeneratorLowerer;
class IteratorLowerer;
class ObjectLayouts;
class ObjectLowerer;
class TailCallLowerer;
class MayThrow;
class ThrowLowerer;
class UnwindLowerer;
class TryLowerer;
class ErrorLowerer;
class FromThrowableLowerer;
class UnionLayout;
class UnionLowerer;
class UnionMemberLowerer;
class UnknownLowerer;
class BytesLowerer;
class BytesCallbackLowerer;
class BytesCallLowerer;
class ChildLowerer;
class CryptoLowerer;
class DateLowerer;
class DecimalLowerer;
class LibraryCallLowerer;
class IntlLowerer;
class IntlCallLowerer;
class JsonLowerer;
class FromJsonLowerer;
class FsPromisesLowerer;
class BuiltinModuleLowerer;
class ProcessLowerer;
class UnknownCastLowerer;
class ReflectArgsLowerer;
class ClassInfoLowerer;
class ReflectDispatchLowerer;
class ReflectLowerer;
class RegexLowerer;
class RegexNativeLowerer;
class ResultLowerer;
class ResultCombineLowerer;
class StreamLowerer;
class TzModulesLowerer;

class FeatureRegistry;
class Lowering {
public:
  Lowering(llvm::LLVMContext &ctx, llvm::StringRef moduleName, const llvm::DataLayout &layout);
  // Where the files of the program are, for the places in the stacks of
  // errors (file, line and column).
  void setSourceMgr(const llvm::SourceMgr *sourceMgr) { sourceMgr_ = sourceMgr; }
  // What of the program's async code may run in parallel (docs/async.md):
  // with a piece that may, the program runs on the threads of the parallel
  // runtime, tzrt_mt (see TZ_PARALLEL in runtime/core/tzrt/tzrt.h).
  void setFeatures(const FeatureRegistry *features) { features_ = features; }
  void setParallelPlan(const ParallelPlan *plan);
  void setMemoryPlan(const MemoryPlan *memory) { memory_ = memory; }
  bool parallel() const { return parallel_; }
  // Before a place where async code touches what keeps node's order, the
  // piece it is in waits for its turn on the thread that runs it ahead.
  void orderAt(llvm::SMLoc loc);

private:
  struct JumpTarget;
  struct TailGroup;
  // Scopes: managed variables to release when control leaves them. A slot
  // without a type holds an object of a closure (a box or an environment).
  // `unwind` is where an error leaves the scope: it releases them too and
  // goes on outward (see codegen_throw.cpp); made when first needed.
  struct Scope {
    std::vector<std::pair<llvm::Value *, const Type *>> slots;
    // Slots of iterators that are closed (see iterCloseFunction) when the
    // scope ends, before the slots are released.
    std::vector<llvm::Value *> iterators;
    // Slots of dictionaries being iterated, whose count of iterations goes
    // down when the scope ends (see beginDictIteration).
    std::vector<llvm::Value *> dicts;
    // Slots of the async iterators of `for await`, closed when the scope
    // ends while they hold one, which the code awaits (codegen_async_iter.cpp).
    std::vector<llvm::Value *> asyncIterators;
    llvm::BasicBlock *unwind = nullptr;
  };
  // Operands that are used and then dropped.
  struct Operand {
    llvm::Value *value;
    bool owned; // must be released after use
    const Type *type;
    // While held (see holdOperand): the slot it waits in.
    llvm::Value *heldSlot = nullptr;
  };
  // Closures (codegen_closure.cpp)
  struct EnvLayout {
    llvm::StructType *type = nullptr; // { i64 rc, ptr drop, fields... }
    llvm::Function *drop = nullptr;
    std::vector<const VarSymbol *> fields;
    const void *key = nullptr; // in envLayouts_
    // An environment that may form cycles is a member of a group, with a
    // layout of its own (see memberDescriptor).
    bool member = false;
    llvm::Constant *descriptor = nullptr;
  };
  // Objects (codegen_object.cpp). The fields of a layout lie in memory by
  // alignment, largest first, then by name: so no padding is needed between
  // them, and an object type and its read-only twin have the same layout.
  struct ObjectLayout {
    llvm::StructType *type = nullptr; // { i64 rc, ptr layout, fields... }
    // The struct element of each field, in the order of Type::fields().
    std::vector<unsigned> elements;
    // An object literal with methods holds the variables they capture after
    // its fields (see ClassDecl::literal), from this element on.
    unsigned captures = 0;
    // Releases what the fields hold and frees the object.
    llvm::Function *drop = nullptr;
    // Releases a reference, dropping the object with the last one.
    llvm::Function *release = nullptr;
  };
  // Inheritance and interfaces (codegen_inherit.cpp). The descriptor of an
  // instance of a class is { TzLayout, ptr itables, [slots x { code, info }] }:
  // the method table follows the layout, at a fixed offset. Code that knows
  // an object by an interface reads a field at the offset it has in the
  // interface's own layout when every type that implements the interface
  // has it there; otherwise, and for a method, through the table the
  // object's descriptor gives for the interface (itables, by the
  // interface's number, which no two interfaces of one type share).
  struct InterfaceMember {
    int entry = -1;        // in the table; -1 for a field read directly
    bool callable = false; // a method: { code, info } rather than an offset
  };
  struct InterfaceInfo {
    // The object types whose objects code may see as the interface, itself
    // included.
    std::vector<const Type *> implementers;
    std::vector<InterfaceMember> members; // by the fields of the interface
    // The methods with several signatures: by method, by signature.
    std::vector<std::vector<InterfaceMember>> overloads;
    llvm::StructType *table = nullptr;
    unsigned color = 0;
    bool dispatch = false; // some member goes through the table
  };
  // Where the method table of a class's descriptor starts, and the address of
  // the code (or with `info`, the info) at `slot` of the object's table.
  static constexpr uint64_t kVtableOffset = 48;
  // Whether objects of other types may be seen as `type`: their drop
  // functions differ, so the descriptor's is called.
  bool hasSubtypes(const Type *type) const { return withSubtypes_.contains(type); }
  // Results (codegen_result.cpp). A Result is a struct of its two halves and
  // a flag; a half of type void (`ok()`) or never is an empty struct.
  struct ResultLayout {
    llvm::StructType *type = nullptr;
    unsigned value = 0, error = 0, ok = 0; // the elements
  };
  // Tail calls (codegen_tail.cpp). A cycle of functions that call each
  // other (or one that calls itself) in tail position, emitted as one LLVM
  // function where those calls jump.
  struct TailGroup {
    std::vector<FunctionDecl *> members;
    bool emitted = false;
    // While emitting: the slots of each member's arguments, and where its
    // body starts.
    std::vector<std::vector<llvm::Value *>> slots;
    std::vector<llvm::BasicBlock *> starts;
  };
  llvm::DenseMap<const Type *, llvm::Function *> unionRetains_, unionReleases_;
  std::vector<const Type *> subtypes_;     // see SourceFile::subtypes
  llvm::StructType *jsonPathTy_ = nullptr; // { ptr parent, string key, i64 index }
  // Generators and iterators (codegen_iter.cpp). An iterator is an object of
  // the runtime, { i64 rc, ptr layout }, followed by the frame of an LLVM
  // coroutine that gives its values: a generator function, or one that
  // Lowering makes for the iterator of an array or a helper such as map.
  //
  // Where values are consumed right where the iterator is made (`for...of`,
  // a spread, toArray...), nothing of that is made: the chain of helpers
  // becomes one loop, and a generator called there is emitted inline, with
  // the consumer at each `yield` (see prepare).
  //
  // What gets the values of an iteration: `each` gets each value, owned,
  // and returns what a `yield` that gave it gets back (owned, of type
  // `sent`; null if `sent` is null); `stop` ends the whole iteration, and
  // leaves the builder in a block that is closed.
  struct Consumer {
    std::function<llvm::Value *(llvm::Value *)> each;
    std::function<void()> stop;
    const Type *sent = nullptr;
  };
  // An iteration whose operands are evaluated already: `run` emits it.
  struct Producer {
    std::function<void(const Consumer &)> run;
  };
  // What the code of a function sees: a generator emitted inline runs its
  // body in its own, and the consumer at each `yield` in the code's.
  struct InlineGenerator;
  struct EmitContext {
    FunctionDecl *fn;
    llvm::Value *env;
    const Type *returnType;
    TailGroup *tailGroup;
    std::vector<JumpTarget> targets;
    InlineGenerator *inlined;
  };
  struct InlineGenerator {
    FunctionDecl *decl;
    Consumer consumer;
    EmitContext consumerContext;
    llvm::BasicBlock *exit;  // where the generator finishes
    llvm::Value *returnSlot; // what it returns goes here, if anywhere
    size_t scopeDepth;       // the scopes its `return` leaves
  };
  // The coroutine being emitted: a generator, or the iterator of an array or
  // a helper.
  struct Coroutine {
    llvm::Value *promise; // { ptr out, ptr in, ptr ret, i32 running, i32 close }
    llvm::Value *id, *handle, *object;
    llvm::BasicBlock *finalBlock, *freeBlock, *returnBlock;
    const Type *sent; // what next(x) sends, or null
    // A generator with a `try`: closing it runs the `finally` blocks around
    // the `yield` it waits at (see emitCoroutineYield). What it yields.
    bool closes = false;
    const Type *yielded = nullptr;
    // The bytes of the object before the frame.
    uint64_t header = 16;
  };
  // Dictionaries (codegen_dict.cpp): a pointer to a TzMap of the runtime
  // (runtime/core/map/map.c), counted and released through its layout as an
  // object is. An entry is { i32 hash, i32 live, key, value }, laid out here.
  struct DictLayout {
    llvm::StructType *entry = nullptr;
    uint64_t keyOffset = 0, valueOffset = 0, size = 0;
    llvm::Constant *descriptor = nullptr; // TzMapLayout
    // Of a type that may form cycles: the same for maps that are no members
    // (see makesMember).
    llvm::Constant *plainDescriptor = nullptr;
  };
  bool mayThrow(const FunctionDecl *fn) const { return throwing_.contains(fn); }
  // A call through a value of the function type `fnType` may throw.
  bool typeThrows(const Type *fnType) const { return throwingTypes_.contains(fnType); }
  // After a call that may throw, if `throws`.
  void emitThrowCheckIf(bool throws);
  // The innermost `try` whose `finally` a jump to targets_[target] (or a
  // `return`, for ~0) passes, or null.
  struct TryContext;
  // Owned values held while code that may throw runs: a value that is
  // managed waits in a managed slot of the current scope, which the error
  // path releases with the variables (`held` false when not needed).
  struct Held {
    llvm::Value *value;
    llvm::Value *slot;
    const Type *type;
  };
  // Async (codegen_async.cpp, docs/async.md). A Promise is an object of the
  // runtime (TzPromise in runtime/core/async/async.h) with its value after the
  // header; an async function is a coroutine whose frame follows its promise
  // and the reactor it awaits with.
  struct AsyncState {
    llvm::Value *object = nullptr;  // the promise
    llvm::Value *reactor = nullptr; // what it awaits with
    const Type *value = nullptr;    // what the promise holds; void for nothing
    // A callback whose promise nobody takes: the ramp drops it.
    bool dropsPromise = false;
    // An async generator (codegen_async_iter.cpp): `object` is the generator.
    bool generator = false;
  };
  // While the code that destroys a coroutine where it waits is emitted:
  // nothing is awaited there.
  bool cleanupNoAwait_ = false;
  AsyncState *async_ = nullptr;
  bool usesAsync_ = false;
  // The program is an ES module for Node: its top-level code runs inside a
  // microtask.
  bool esModule_ = false;
  llvm::StructType *promiseObjTy_ = nullptr;
  llvm::DenseMap<const Type *, llvm::Constant *> promiseLayouts_, resolverInfos_;
  std::map<std::tuple<Builtin, const Type *, const Type *, const Type *, const Type *>,
           llvm::Function *>
      reactionHandlers_;
  std::vector<const ClassDecl *> emittedClasses_;
  llvm::DenseMap<const ClassDecl *, llvm::GlobalVariable *> classInfos_;
  llvm::SetVector<const Type *> unknownFunctionTypes_; // that reach an unknown
  // What the program looks up through the descriptions: `new` of a class
  // known only when it runs (and Reflect.construct), Reflect.get, and
  // fromJson(cls, j). A description that nothing looks up is not emitted.
  bool reflectsNew_ = false, reflectsGet_ = false, reflectsJson_ = false;
  llvm::MDNode *tbaaHeader_ = nullptr, *tbaaData_ = nullptr;
  llvm::StructType *bytesTy_ = nullptr; // TzBytes
  // A number for the runtime (TzIntlNumber): its kind and words, and the
  // operand to drop after the call.
  struct IntlNumber {
    Operand operand;
    int kind;
    llvm::Value *d, *integer, *meta;
  };
  // What MemoryPass found (see MemoryPlan).
  const MemoryPlan &memory() const { return *memory_; }
  bool isCyclic(const Type *type) const { return memory_->isCyclic(type); }
  bool isCyclicEnv(const void *key) const { return memory_->isCyclicEnv(key); }
  bool isCyclicBox(const Type *type) const { return memory_->isCyclicBox(type); }
  bool isWeakTarget(const Type *type) const { return memory_->isWeakTarget(type); }
  bool makesMember(const Expr *site, const Type *type) const {
    return memory_->makesMember(site, type);
  }
  // Whether objects of the static type `type` (an object, array or dictionary type, or a box or
  // environment) may be members whose slots need storeIn: refMayBeMember of it.
  bool ownerMayBeMember(const Type *type) const { return memory_->refMayBeMember(type); }
  bool isEternal(const VarSymbol *var) const { return memory_->isEternal(var); }
  // Debug mode (a program compiled with TZ_CHECK_LEAKS set, as the tests
  // are): the proofs of layers 1 and 2 are checked when the program runs,
  // and large groups are walked (runtime/core/group/group.c).
  bool debugMemory_ = false;
  // The field a store into a member being emitted writes, for the place
  // debug mode notes (tz_group_site).
  std::string storeField_;
  llvm::SMLoc storeLoc_;
  // Other objects of the program's own: their descriptor, struct and the
  // values in it, by element.
  struct PublishedObject {
    llvm::Constant *descriptor;
    llvm::StructType *type;
    std::vector<std::pair<unsigned, const Type *>> values;
  };
  std::vector<PublishedObject> publishedObjects_;
  // The async functions whose pieces may run ahead of their turn: what they
  // get is published as they start.
  llvm::DenseSet<const FunctionDecl *> aheadFunctions_;
  llvm::DenseMap<llvm::Function *, llvm::Constant *> memberDescriptors_;
  std::vector<std::pair<llvm::Constant *, const Type *>> boxDescriptors_;
  // The slots in main of those that no function uses.
  llvm::DenseSet<llvm::Value *> eternalSlots_;
  const ParallelPlan *plan_ = nullptr;
  const MemoryPlan *memory_ = nullptr;
  // The topics of the library, which the calls of built-ins go to (see compiler/library).
  const FeatureRegistry *features_ = nullptr;
  bool parallel_ = false; // runs on the threads of tzrt_mt
  // The awaits after which a piece may run ahead of its turn, and the
  // places where pieces touch what keeps node's order (see ParallelPlan).
  // Each by its number, from 1: the site of runtime/core/parallel/parallel.c.
  llvm::DenseMap<const Node *, uint32_t> aheadStarts_;
  llvm::DenseSet<const char *> orderedLocs_;
  llvm::DenseSet<const VarSymbol *> libraryState_; // see addressOf
  llvm::Function *refRetainFn_ = nullptr;          // tz.ref.retain
  llvm::Function *plainInstanceReleaseFn_ = nullptr;
  llvm::Function *plainObjReleaseFn_ = nullptr;
  llvm::Function *plainObjRetainFn_ = nullptr;
  llvm::LLVMContext &ctx_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  llvm::StructType *strTy_;   // { ptr obj, i64 meta }
  llvm::StructType *arrayTy_; // the array header, see runtime/core/array/array.c
  llvm::StructType *typeDescTy_;
  llvm::StructType *fnTy_;     // { ptr info, ptr env }
  llvm::StructType *fnInfoTy_; // { ptr code, string name }
  llvm::StructType *decTy_;    // { i64 coef, i64 meta }
  llvm::Function *function_ = nullptr;
  const Type *returnType_ = nullptr;
  FunctionDecl *currentFn_ = nullptr; // null in main
  llvm::Value *envParam_ = nullptr;   // the environment of the closure being emitted
  // The variables of the function being emitted, and the globals.
  llvm::DenseMap<const VarSymbol *, llvm::Value *> addresses_, globals_;
  // Boxed variables: the slot that holds the box of one the function
  // declares, or the box of one it captured.
  llvm::DenseMap<const VarSymbol *, llvm::Value *> boxSlots_, envBoxes_;
  // The slot that holds the environment of a group the function declares.
  llvm::DenseMap<const NestedGroup *, llvm::Value *> groupEnvs_;
  llvm::DenseMap<const void *, EnvLayout> envLayouts_;
  llvm::DenseMap<const Type *, llvm::Function *> boxDrops_;
  std::vector<FunctionDecl *> pendingClosures_;
  bool comparesFunctions_ = false;
  // The program has TLS (SourceFile::usesTls): the intrinsics `__tz_tls_*` call it.
  bool usesTls_ = false;
  llvm::DenseMap<const FunctionDecl *, TailGroup *> tailGroupOf_;
  TailGroup *tailGroup_ = nullptr; // being emitted
  // Statements that are calls in tail position of a function returning void.
  llvm::DenseSet<const Stmt *> tailStatements_;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::DenseMap<const Type *, llvm::Constant *> typeDescriptors_;
  std::map<std::pair<const Type *, std::vector<unsigned>>, llvm::Constant *> layoutDescriptors_;
  llvm::StructType *fieldDescTy_; // { ptr name, i32 length, i32 offset, ptr type }
  // { ptr drop, ptr fields, i32 count, i32 flags, ptr name, i32 name length,
  //   i32 class number }
  llvm::StructType *layoutDescTy_;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classDescriptors_;
  llvm::DenseMap<llvm::Function *, llvm::Constant *> dropDescriptors_;
  // Interfaces (codegen_inherit.cpp): those with implementers, and how many
  // numbers they take; the interfaces with tables that each type has; the
  // types with subtypes; and what was made for them.
  std::map<const Type *, InterfaceInfo> interfaces_;
  llvm::DenseSet<const Type *> withSubtypes_;
  // What the names of the top-level declarations of imported files start
  // with in the IR: their file.
  llvm::DenseMap<const Stmt *, std::string> symbolPrefixes_;
  std::vector<Scope> scopes_;
  // A loop, switch or labeled statement that `break` and `continue` jump out
  // of.
  struct JumpTarget {
    llvm::SmallVector<llvm::StringRef, 1> labels;
    bool isLoop;
    bool isSwitch;
    llvm::BasicBlock *breakTarget;
    llvm::BasicBlock *continueTarget; // loops only
    size_t scopeDepth;                // scopes to keep when jumping to either target
    // `continue` in a loop over a generator emitted inline goes on in the
    // generator, after the `yield`, inside its scopes; ~0 for scopeDepth.
    size_t continueDepth = ~size_t(0);
  };
  std::vector<JumpTarget> targets_;
  // Generators and iterators: the generator emitted inline whose code is
  // being emitted, the coroutine being emitted, and the functions Lowering
  // makes for the iterators of arrays and helpers, emitted after the others.
  InlineGenerator *inline_ = nullptr;
  Coroutine *coro_ = nullptr;
  struct IteratorJob {
    llvm::Function *function;
    Builtin builtin;
    const Type *source, *operand, *result;
  };
  std::vector<IteratorJob> pendingIterators_;
  llvm::StructType *promiseTy_ = nullptr;
  // The labels of the loop or switch being emitted next.
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  // Where the links of the optional chains being emitted go when their object
  // is undefined: the innermost chain's last.
  std::vector<llvm::BasicBlock *> chainEnds_;
  // Functions of the C runtime (runtime/).
  struct Runtime {
    llvm::FunctionCallee init;
    llvm::FunctionCallee shutdown;
    llvm::FunctionCallee mainDone;
    llvm::FunctionCallee printI32;
    llvm::FunctionCallee printI64;
    llvm::FunctionCallee printF64;
    llvm::FunctionCallee printBool;
    llvm::FunctionCallee printChar;
    llvm::FunctionCallee printString;
    llvm::FunctionCallee printValue;
    llvm::FunctionCallee debugger;
    llvm::FunctionCallee strFree;
    llvm::FunctionCallee strConcatN;
    llvm::FunctionCallee strEqual;
    llvm::FunctionCallee strCompare;
    llvm::FunctionCallee strCharAt;
    llvm::FunctionCallee strCharCodeAt;
    llvm::FunctionCallee strCodePointAt;
    llvm::FunctionCallee strSlice;
    llvm::FunctionCallee strSubstring;
    llvm::FunctionCallee strIndexOf;
    llvm::FunctionCallee strIncludes;
    llvm::FunctionCallee strStartsWith;
    llvm::FunctionCallee strEndsWith;
    llvm::FunctionCallee strRepeat;
    llvm::FunctionCallee strFromI64;
    llvm::FunctionCallee strFromF64;
    llvm::FunctionCallee strCharAtOrEmpty;
    llvm::FunctionCallee strCodePoint;
    llvm::FunctionCallee strLastIndexOf;
    llvm::FunctionCallee strSplit;
    llvm::FunctionCallee strTrim;
    llvm::FunctionCallee strPad;
    llvm::FunctionCallee strToUpper;
    llvm::FunctionCallee strToLower;
    llvm::FunctionCallee strReplace;
    llvm::FunctionCallee strFromCharCodes;
    llvm::FunctionCallee strFromCodePoints;
    llvm::FunctionCallee strToNumber;
    llvm::FunctionCallee parseInt;
    llvm::FunctionCallee parseFloat;
    llvm::FunctionCallee numToFixed;
    llvm::FunctionCallee numToRadix;
    llvm::FunctionCallee toInt32;
    llvm::FunctionCallee mathRound;
    llvm::FunctionCallee mathPow;
    llvm::FunctionCallee mathPowLong;
    llvm::FunctionCallee mathHypot;
    llvm::FunctionCallee mathRandom;
    llvm::FunctionCallee checkedInt32OfLong;
    llvm::FunctionCallee valueToString;
    llvm::FunctionCallee arrNew;
    llvm::FunctionCallee arrNewLength;
    llvm::FunctionCallee arrFree;
    llvm::FunctionCallee arrGrow;
    llvm::FunctionCallee arrShift;
    llvm::FunctionCallee arrUnshiftSlots;
    llvm::FunctionCallee arrSetLength;
    llvm::FunctionCallee arrSlice;
    llvm::FunctionCallee arrCopy;
    llvm::FunctionCallee arrAppend;
    llvm::FunctionCallee arrAppendCodePoints;
    llvm::FunctionCallee arrIndexOf;
    llvm::FunctionCallee arrJoin;
    llvm::FunctionCallee arrReverse;
    llvm::FunctionCallee arrFill;
    llvm::FunctionCallee arrSort;
    llvm::FunctionCallee arrSortWith;
    llvm::FunctionCallee arrSplice;
    llvm::FunctionCallee objAlloc;
    llvm::FunctionCallee objFree;
    llvm::FunctionCallee objKeys;
    llvm::FunctionCallee objValues;
    llvm::FunctionCallee objEntries;
  } rt_;
  llvm::Function *retainFn_ = nullptr;
  llvm::Function *releaseFn_ = nullptr;
  llvm::Function *arrRetainFn_ = nullptr;
  llvm::Function *arrReleaseFn_ = nullptr;
  llvm::Function *objRetainFn_ = nullptr;
  llvm::Function *objReleaseFn_ = nullptr;
  llvm::Function *instanceReleaseFn_ = nullptr;
  // Exceptions (codegen_throw.cpp): what analyzeThrows found, the functions
  // that check the stack at their start (those that may recurse), and the
  // tail calls that are jumps (findTailGroups), which do not recurse.
  llvm::DenseSet<const FunctionDecl *> throwing_, recursive_;
  llvm::DenseSet<const Type *> throwingTypes_;
  llvm::DenseSet<const CallExpr *> tailJumps_;
  // A `try` being emitted. Errors from its block go to its `catch`, or with
  // only a `finally`, to that; errors from the `catch` go to the `finally`.
  // A `finally` runs on every way out of the block and the `catch`: each
  // sets `completion` to say how to go on after it (0 on to the end, 1 throw
  // the pending error, 2 return the pending value, 3 + k take exits[k]).
  struct TryContext {
    enum class Phase { Block, Handler, Finally };
    TryStmt *stmt;
    size_t scopeDepth;   // scopes_.size() at its start
    size_t targetsDepth; // targets_.size() at its start
    Phase phase = Phase::Block;
    llvm::BasicBlock *handler = nullptr;    // the catch
    llvm::BasicBlock *finally = nullptr;    // the finally, with completion set
    llvm::BasicBlock *throwEntry = nullptr; // the finally for an error
    llvm::Value *completion = nullptr;
    llvm::Value *pendingError = nullptr, *pendingValue = nullptr;
    bool returns = false; // some `return` goes through the `finally`
    struct Exit {
      size_t target;
      bool isBreak;
    };
    std::vector<Exit> exits;
  };
  std::vector<TryContext> tries_;
  // Where an error leaves the function being emitted.
  llvm::BasicBlock *unwindExit_ = nullptr;
  // The call a `return` being emitted gives, left unchecked (see emitReturn).
  const CallExpr *returnCall_ = nullptr;
  // The expression being emitted: where a built-in that fails throws from.
  llvm::SMLoc loc_;
  const llvm::SourceMgr *sourceMgr_ = nullptr;
  ClassDecl *errorClass_ = nullptr;

public:
  ~Lowering();

  ModuleBuilder &moduleBuilder() const { return *moduleBuilderPart_; }
  FunctionBuilder &functionBuilder() const { return *functionBuilderPart_; }
  RuntimeFunctions &runtimeFunctions() const { return *runtimeFunctionsPart_; }
  FunctionFrame &functionFrame() const { return *functionFramePart_; }
  RefCounts &refCounts() const { return *refCountsPart_; }
  OptionalLowerer &optionalLowerer() const { return *optionalLowererPart_; }
  StmtLowerer &stmtLowerer() const { return *stmtLowererPart_; }
  LoopLowerer &loopLowerer() const { return *loopLowererPart_; }
  SwitchLowerer &switchLowerer() const { return *switchLowererPart_; }
  ExprLowerer &exprLowerer() const { return *exprLowererPart_; }
  UnaryLowerer &unaryLowerer() const { return *unaryLowererPart_; }
  BinaryLowerer &binaryLowerer() const { return *binaryLowererPart_; }
  EqualityLowerer &equalityLowerer() const { return *equalityLowererPart_; }
  TruthLowerer &truthLowerer() const { return *truthLowererPart_; }
  AssignLowerer &assignLowerer() const { return *assignLowererPart_; }
  CallLowerer &callLowerer() const { return *callLowererPart_; }
  ConsoleLowerer &consoleLowerer() const { return *consoleLowererPart_; }
  StringLowerer &stringLowerer() const { return *stringLowererPart_; }
  StringMethodLowerer &stringMethodLowerer() const { return *stringMethodLowererPart_; }
  MemberLowerer &memberLowerer() const { return *memberLowererPart_; }
  TypeDescriptors &typeDescriptors() const { return *typeDescriptorsPart_; }
  ArrayLowerer &arrayLowerer() const { return *arrayLowererPart_; }
  ArrayMethodLowerer &arrayMethodLowerer() const { return *arrayMethodLowererPart_; }
  CallbackMethodLowerer &callbackMethodLowerer() const { return *callbackMethodLowererPart_; }
  PromiseLowerer &promiseLowerer() const { return *promiseLowererPart_; }
  AsyncLowerer &asyncLowerer() const { return *asyncLowererPart_; }
  PromiseCombineLowerer &promiseCombineLowerer() const { return *promiseCombineLowererPart_; }
  ReactionLowerer &reactionLowerer() const { return *reactionLowererPart_; }
  ResultAsyncHandler &resultAsyncHandler() const { return *resultAsyncHandlerPart_; }
  ResultAsyncLowerer &resultAsyncLowerer() const { return *resultAsyncLowererPart_; }
  AsyncBuiltinLowerer &asyncBuiltinLowerer() const { return *asyncBuiltinLowererPart_; }
  AsyncGeneratorLowerer &asyncGeneratorLowerer() const { return *asyncGeneratorLowererPart_; }
  MathLowerer &mathLowerer() const { return *mathLowererPart_; }
  NumberLowerer &numberLowerer() const { return *numberLowererPart_; }
  ClassDescriptors &classDescriptors() const { return *classDescriptorsPart_; }
  ClassLowerer &classLowerer() const { return *classLowererPart_; }
  ClosureLowerer &closureLowerer() const { return *closureLowererPart_; }
  EnvLowerer &envLowerer() const { return *envLowererPart_; }
  GroupLowerer &groupLowerer() const { return *groupLowererPart_; }
  StoreLowerer &storeLowerer() const { return *storeLowererPart_; }
  WeakRefLowerer &weakRefLowerer() const { return *weakRefLowererPart_; }
  MemoryDebug &memoryDebug() const { return *memoryDebugPart_; }
  PublishLowerer &publishLowerer() const { return *publishLowererPart_; }
  DecoratorLowerer &decoratorLowerer() const { return *decoratorLowererPart_; }
  DictLowerer &dictLowerer() const { return *dictLowererPart_; }
  DictMethodLowerer &dictMethodLowerer() const { return *dictMethodLowererPart_; }
  InheritLowerer &inheritLowerer() const { return *inheritLowererPart_; }
  CoroutineLowerer &coroutineLowerer() const { return *coroutineLowererPart_; }
  GeneratorLowerer &generatorLowerer() const { return *generatorLowererPart_; }
  LazyChainLowerer &lazyChainLowerer() const { return *lazyChainLowererPart_; }
  InlineGeneratorLowerer &inlineGeneratorLowerer() const { return *inlineGeneratorLowererPart_; }
  IteratorLowerer &iteratorLowerer() const { return *iteratorLowererPart_; }
  ObjectLayouts &objectLayouts() const { return *objectLayoutsPart_; }
  ObjectLowerer &objectLowerer() const { return *objectLowererPart_; }
  TailCallLowerer &tailCallLowerer() const { return *tailCallLowererPart_; }
  MayThrow &mayThrow() const { return *mayThrowPart_; }
  ThrowLowerer &throwLowerer() const { return *throwLowererPart_; }
  UnwindLowerer &unwindLowerer() const { return *unwindLowererPart_; }
  TryLowerer &tryLowerer() const { return *tryLowererPart_; }
  ErrorLowerer &errorLowerer() const { return *errorLowererPart_; }
  FromThrowableLowerer &fromThrowableLowerer() const { return *fromThrowableLowererPart_; }
  UnionLayout &unionLayout() const { return *unionLayoutPart_; }
  UnionLowerer &unionLowerer() const { return *unionLowererPart_; }
  UnionMemberLowerer &unionMemberLowerer() const { return *unionMemberLowererPart_; }
  UnknownLowerer &unknownLowerer() const { return *unknownLowererPart_; }
  BytesLowerer &bytesLowerer() const { return *bytesLowererPart_; }
  BytesCallbackLowerer &bytesCallbackLowerer() const { return *bytesCallbackLowererPart_; }
  BytesCallLowerer &bytesCallLowerer() const { return *bytesCallLowererPart_; }
  ChildLowerer &childLowerer() const { return *childLowererPart_; }
  CryptoLowerer &cryptoLowerer() const { return *cryptoLowererPart_; }
  DateLowerer &dateLowerer() const { return *dateLowererPart_; }
  DecimalLowerer &decimalLowerer() const { return *decimalLowererPart_; }
  LibraryCallLowerer &libraryCallLowerer() const { return *libraryCallLowererPart_; }
  IntlLowerer &intlLowerer() const { return *intlLowererPart_; }
  IntlCallLowerer &intlCallLowerer() const { return *intlCallLowererPart_; }
  JsonLowerer &jsonLowerer() const { return *jsonLowererPart_; }
  FromJsonLowerer &fromJsonLowerer() const { return *fromJsonLowererPart_; }
  FsPromisesLowerer &fsPromisesLowerer() const { return *fsPromisesLowererPart_; }
  BuiltinModuleLowerer &builtinModuleLowerer() const { return *builtinModuleLowererPart_; }
  ProcessLowerer &processLowerer() const { return *processLowererPart_; }
  UnknownCastLowerer &unknownCastLowerer() const { return *unknownCastLowererPart_; }
  ReflectArgsLowerer &reflectArgsLowerer() const { return *reflectArgsLowererPart_; }
  ClassInfoLowerer &classInfoLowerer() const { return *classInfoLowererPart_; }
  ReflectDispatchLowerer &reflectDispatchLowerer() const { return *reflectDispatchLowererPart_; }
  ReflectLowerer &reflectLowerer() const { return *reflectLowererPart_; }
  RegexLowerer &regexLowerer() const { return *regexLowererPart_; }
  RegexNativeLowerer &regexNativeLowerer() const { return *regexNativeLowererPart_; }
  ResultLowerer &resultLowerer() const { return *resultLowererPart_; }
  ResultCombineLowerer &resultCombineLowerer() const { return *resultCombineLowererPart_; }
  StreamLowerer &streamLowerer() const { return *streamLowererPart_; }
  TzModulesLowerer &tzModulesLowerer() const { return *tzModulesLowererPart_; }

private:
  friend class ModuleBuilder;
  friend class FunctionBuilder;
  friend class RuntimeFunctions;
  friend class FunctionFrame;
  friend class RefCounts;
  friend class OptionalLowerer;
  friend class StmtLowerer;
  friend class LoopLowerer;
  friend class SwitchLowerer;
  friend class ExprLowerer;
  friend class UnaryLowerer;
  friend class BinaryLowerer;
  friend class EqualityLowerer;
  friend class TruthLowerer;
  friend class AssignLowerer;
  friend class CallLowerer;
  friend class ConsoleLowerer;
  friend class StringLowerer;
  friend class StringMethodLowerer;
  friend class MemberLowerer;
  friend class TypeDescriptors;
  friend class ArrayLowerer;
  friend class ArrayMethodLowerer;
  friend class CallbackMethodLowerer;
  friend class PromiseLowerer;
  friend class AsyncLowerer;
  friend class PromiseCombineLowerer;
  friend class ReactionLowerer;
  friend class ResultAsyncHandler;
  friend class ResultAsyncLowerer;
  friend class AsyncBuiltinLowerer;
  friend class AsyncGeneratorLowerer;
  friend class MathLowerer;
  friend class NumberLowerer;
  friend class ClassDescriptors;
  friend class ClassLowerer;
  friend class ClosureLowerer;
  friend class EnvLowerer;
  friend class GroupLowerer;
  friend class StoreLowerer;
  friend class WeakRefLowerer;
  friend class MemoryDebug;
  friend class PublishLowerer;
  friend class DecoratorLowerer;
  friend class DictLowerer;
  friend class DictMethodLowerer;
  friend class InheritLowerer;
  friend class CoroutineLowerer;
  friend class GeneratorLowerer;
  friend class LazyChainLowerer;
  friend class InlineGeneratorLowerer;
  friend class IteratorLowerer;
  friend class ObjectLayouts;
  friend class ObjectLowerer;
  friend class TailCallLowerer;
  friend class MayThrow;
  friend class ThrowLowerer;
  friend class UnwindLowerer;
  friend class TryLowerer;
  friend class ErrorLowerer;
  friend class FromThrowableLowerer;
  friend class UnionLayout;
  friend class UnionLowerer;
  friend class UnionMemberLowerer;
  friend class UnknownLowerer;
  friend class BytesLowerer;
  friend class BytesCallbackLowerer;
  friend class BytesCallLowerer;
  friend class BytesCallSite;
  friend class BytesMakeCalls;
  friend class TypedArrayCalls;
  friend class NumberAccessCalls;
  friend class BufferCalls;
  friend class TextCodecCalls;
  friend class ChildLowerer;
  friend class CryptoLowerer;
  friend class DateLowerer;
  friend class DecimalLowerer;
  friend class LibraryCallLowerer;
  friend class IntlLowerer;
  friend class IntlCallLowerer;
  friend class JsonLowerer;
  friend class FromJsonLowerer;
  friend class FsPromisesLowerer;
  friend class BuiltinModuleLowerer;
  friend class ProcessLowerer;
  friend class UnknownCastLowerer;
  friend class ReflectArgsLowerer;
  friend class ClassInfoLowerer;
  friend class ReflectDispatchLowerer;
  friend class ReflectLowerer;
  friend class RegexLowerer;
  friend class RegexNativeLowerer;
  friend class ResultLowerer;
  friend class ResultCombineLowerer;
  friend class StreamLowerer;
  friend class TzModulesLowerer;
  std::unique_ptr<ModuleBuilder> moduleBuilderPart_;
  std::unique_ptr<FunctionBuilder> functionBuilderPart_;
  std::unique_ptr<RuntimeFunctions> runtimeFunctionsPart_;
  std::unique_ptr<FunctionFrame> functionFramePart_;
  std::unique_ptr<RefCounts> refCountsPart_;
  std::unique_ptr<OptionalLowerer> optionalLowererPart_;
  std::unique_ptr<StmtLowerer> stmtLowererPart_;
  std::unique_ptr<LoopLowerer> loopLowererPart_;
  std::unique_ptr<SwitchLowerer> switchLowererPart_;
  std::unique_ptr<ExprLowerer> exprLowererPart_;
  std::unique_ptr<UnaryLowerer> unaryLowererPart_;
  std::unique_ptr<BinaryLowerer> binaryLowererPart_;
  std::unique_ptr<EqualityLowerer> equalityLowererPart_;
  std::unique_ptr<TruthLowerer> truthLowererPart_;
  std::unique_ptr<AssignLowerer> assignLowererPart_;
  std::unique_ptr<CallLowerer> callLowererPart_;
  std::unique_ptr<ConsoleLowerer> consoleLowererPart_;
  std::unique_ptr<StringLowerer> stringLowererPart_;
  std::unique_ptr<StringMethodLowerer> stringMethodLowererPart_;
  std::unique_ptr<MemberLowerer> memberLowererPart_;
  std::unique_ptr<TypeDescriptors> typeDescriptorsPart_;
  std::unique_ptr<ArrayLowerer> arrayLowererPart_;
  std::unique_ptr<ArrayMethodLowerer> arrayMethodLowererPart_;
  std::unique_ptr<CallbackMethodLowerer> callbackMethodLowererPart_;
  std::unique_ptr<PromiseLowerer> promiseLowererPart_;
  std::unique_ptr<AsyncLowerer> asyncLowererPart_;
  std::unique_ptr<PromiseCombineLowerer> promiseCombineLowererPart_;
  std::unique_ptr<ReactionLowerer> reactionLowererPart_;
  std::unique_ptr<ResultAsyncHandler> resultAsyncHandlerPart_;
  std::unique_ptr<ResultAsyncLowerer> resultAsyncLowererPart_;
  std::unique_ptr<AsyncBuiltinLowerer> asyncBuiltinLowererPart_;
  std::unique_ptr<AsyncGeneratorLowerer> asyncGeneratorLowererPart_;
  std::unique_ptr<MathLowerer> mathLowererPart_;
  std::unique_ptr<NumberLowerer> numberLowererPart_;
  std::unique_ptr<ClassDescriptors> classDescriptorsPart_;
  std::unique_ptr<ClassLowerer> classLowererPart_;
  std::unique_ptr<ClosureLowerer> closureLowererPart_;
  std::unique_ptr<EnvLowerer> envLowererPart_;
  std::unique_ptr<GroupLowerer> groupLowererPart_;
  std::unique_ptr<StoreLowerer> storeLowererPart_;
  std::unique_ptr<WeakRefLowerer> weakRefLowererPart_;
  std::unique_ptr<MemoryDebug> memoryDebugPart_;
  std::unique_ptr<PublishLowerer> publishLowererPart_;
  std::unique_ptr<DecoratorLowerer> decoratorLowererPart_;
  std::unique_ptr<DictLowerer> dictLowererPart_;
  std::unique_ptr<DictMethodLowerer> dictMethodLowererPart_;
  std::unique_ptr<InheritLowerer> inheritLowererPart_;
  std::unique_ptr<CoroutineLowerer> coroutineLowererPart_;
  std::unique_ptr<GeneratorLowerer> generatorLowererPart_;
  std::unique_ptr<LazyChainLowerer> lazyChainLowererPart_;
  std::unique_ptr<InlineGeneratorLowerer> inlineGeneratorLowererPart_;
  std::unique_ptr<IteratorLowerer> iteratorLowererPart_;
  std::unique_ptr<ObjectLayouts> objectLayoutsPart_;
  std::unique_ptr<ObjectLowerer> objectLowererPart_;
  std::unique_ptr<TailCallLowerer> tailCallLowererPart_;
  std::unique_ptr<MayThrow> mayThrowPart_;
  std::unique_ptr<ThrowLowerer> throwLowererPart_;
  std::unique_ptr<UnwindLowerer> unwindLowererPart_;
  std::unique_ptr<TryLowerer> tryLowererPart_;
  std::unique_ptr<ErrorLowerer> errorLowererPart_;
  std::unique_ptr<FromThrowableLowerer> fromThrowableLowererPart_;
  std::unique_ptr<UnionLayout> unionLayoutPart_;
  std::unique_ptr<UnionLowerer> unionLowererPart_;
  std::unique_ptr<UnionMemberLowerer> unionMemberLowererPart_;
  std::unique_ptr<UnknownLowerer> unknownLowererPart_;
  std::unique_ptr<BytesLowerer> bytesLowererPart_;
  std::unique_ptr<BytesCallbackLowerer> bytesCallbackLowererPart_;
  std::unique_ptr<BytesCallLowerer> bytesCallLowererPart_;
  std::unique_ptr<ChildLowerer> childLowererPart_;
  std::unique_ptr<CryptoLowerer> cryptoLowererPart_;
  std::unique_ptr<DateLowerer> dateLowererPart_;
  std::unique_ptr<DecimalLowerer> decimalLowererPart_;
  std::unique_ptr<LibraryCallLowerer> libraryCallLowererPart_;
  std::unique_ptr<IntlLowerer> intlLowererPart_;
  std::unique_ptr<IntlCallLowerer> intlCallLowererPart_;
  std::unique_ptr<JsonLowerer> jsonLowererPart_;
  std::unique_ptr<FromJsonLowerer> fromJsonLowererPart_;
  std::unique_ptr<FsPromisesLowerer> fsPromisesLowererPart_;
  std::unique_ptr<BuiltinModuleLowerer> builtinModuleLowererPart_;
  std::unique_ptr<ProcessLowerer> processLowererPart_;
  std::unique_ptr<UnknownCastLowerer> unknownCastLowererPart_;
  std::unique_ptr<ReflectArgsLowerer> reflectArgsLowererPart_;
  std::unique_ptr<ClassInfoLowerer> classInfoLowererPart_;
  std::unique_ptr<ReflectDispatchLowerer> reflectDispatchLowererPart_;
  std::unique_ptr<ReflectLowerer> reflectLowererPart_;
  std::unique_ptr<RegexLowerer> regexLowererPart_;
  std::unique_ptr<RegexNativeLowerer> regexNativeLowererPart_;
  std::unique_ptr<ResultLowerer> resultLowererPart_;
  std::unique_ptr<ResultCombineLowerer> resultCombineLowererPart_;
  std::unique_ptr<StreamLowerer> streamLowererPart_;
  std::unique_ptr<TzModulesLowerer> tzModulesLowererPart_;
};

} // namespace tz
