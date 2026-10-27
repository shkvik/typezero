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

bool staysDuringCall(const Expr *expr);

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
class PlaceLowerer;
class TryLowerer;
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
class FsLowerer;
class PathLowerer;
class OsLowerer;
class ObjectMaker;
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
class ErrSites;
class StreamLowerer;
class TzModulesLowerer;

class FeatureRegistry;
class Lowering {
public:
  Lowering(llvm::LLVMContext &ctx, llvm::StringRef moduleName, const llvm::DataLayout &layout);
  void setSourceMgr(const llvm::SourceMgr *sourceMgr) { sourceMgr_ = sourceMgr; }
  void setFeatures(const FeatureRegistry *features) { features_ = features; }
  void setParallelPlan(const ParallelPlan *plan);
  void setMemoryPlan(const MemoryPlan *memory) { memory_ = memory; }
  bool parallel() const { return parallel_; }
  void orderAt(llvm::SMLoc loc);

private:
  struct JumpTarget;
  struct TailGroup;
  struct Scope {
    std::vector<std::pair<llvm::Value *, const Type *>> slots;
    std::vector<llvm::Value *> iterators;
    std::vector<llvm::Value *> dicts;
    std::vector<llvm::Value *> asyncIterators;
  };
  struct Operand {
    llvm::Value *value;
    bool owned;
    const Type *type;
  };
  struct EnvLayout {
    llvm::StructType *type = nullptr;
    llvm::Function *drop = nullptr;
    std::vector<const VarSymbol *> fields;
    const void *key = nullptr;
    bool member = false;
    llvm::Constant *descriptor = nullptr;
  };
  struct ObjectLayout {
    llvm::StructType *type = nullptr;
    std::vector<unsigned> elements;
    unsigned captures = 0;
    llvm::Function *drop = nullptr;
    llvm::Function *release = nullptr;
  };
  struct InterfaceMember {
    int entry = -1;
    bool callable = false;
  };
  struct InterfaceInfo {
    std::vector<const Type *> implementers;
    std::vector<InterfaceMember> members;
    std::vector<std::vector<InterfaceMember>> overloads;
    llvm::StructType *table = nullptr;
    unsigned color = 0;
    bool dispatch = false;
  };
  static constexpr uint64_t kVtableOffset = 48;
  bool hasSubtypes(const Type *type) const { return withSubtypes_.contains(type); }
  struct ResultLayout {
    llvm::StructType *type = nullptr;
    unsigned value = 0, error = 0, ok = 0, site = 0;
  };
  struct TailGroup {
    std::vector<FunctionDecl *> members;
    bool emitted = false;
    std::vector<std::vector<llvm::Value *>> slots;
    std::vector<llvm::BasicBlock *> starts;
  };
  llvm::DenseMap<const Type *, llvm::Function *> unionRetains_, unionReleases_;
  std::vector<const Type *> subtypes_;
  llvm::StructType *jsonPathTy_ = nullptr;
  struct Consumer {
    std::function<llvm::Value *(llvm::Value *)> each;
    std::function<void()> stop;
    const Type *sent = nullptr;
  };
  struct Producer {
    std::function<void(const Consumer &)> run;
  };
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
    llvm::BasicBlock *exit;
    llvm::Value *returnSlot;
    size_t scopeDepth;
  };
  struct Coroutine {
    llvm::Value *promise;
    llvm::Value *id, *handle, *object;
    llvm::BasicBlock *finalBlock, *freeBlock, *returnBlock;
    const Type *sent;
    bool closes = false;
    const Type *yielded = nullptr;
    uint64_t header = 16;
  };
  struct DictLayout {
    llvm::StructType *entry = nullptr;
    uint64_t keyOffset = 0, valueOffset = 0, size = 0;
    llvm::Constant *descriptor = nullptr;
    llvm::Constant *plainDescriptor = nullptr;
  };
  struct TryContext;
  struct AsyncState {
    llvm::Value *object = nullptr;
    llvm::Value *reactor = nullptr;
    const Type *value = nullptr;
    bool dropsPromise = false;
    bool generator = false;
  };
  bool cleanupNoAwait_ = false;
  AsyncState *async_ = nullptr;
  bool usesAsync_ = false;
  bool esModule_ = false;
  llvm::StructType *promiseObjTy_ = nullptr;
  llvm::DenseMap<const Type *, llvm::Constant *> promiseLayouts_, resolverInfos_;
  std::map<std::tuple<const Type *, const Type *, const Type *>, llvm::Function *>
      reactionHandlers_;
  std::vector<const ClassDecl *> emittedClasses_;
  llvm::DenseMap<const ClassDecl *, llvm::GlobalVariable *> classInfos_;
  llvm::SetVector<const Type *> unknownFunctionTypes_;
  bool reflectsNew_ = false, reflectsGet_ = false, reflectsJson_ = false;
  llvm::MDNode *tbaaHeader_ = nullptr, *tbaaData_ = nullptr;
  llvm::StructType *bytesTy_ = nullptr;
  struct IntlNumber {
    Operand operand;
    int kind;
    llvm::Value *d, *integer, *meta;
  };
  const MemoryPlan &memory() const { return *memory_; }
  bool isCyclic(const Type *type) const { return memory_->isCyclic(type); }
  bool isCyclicEnv(const void *key) const { return memory_->isCyclicEnv(key); }
  bool isCyclicBox(const Type *type) const { return memory_->isCyclicBox(type); }
  bool isWeakTarget(const Type *type) const { return memory_->isWeakTarget(type); }
  bool makesMember(const Expr *site, const Type *type) const {
    return memory_->makesMember(site, type);
  }
  bool ownerMayBeMember(const Type *type) const { return memory_->refMayBeMember(type); }
  bool isEternal(const VarSymbol *var) const { return memory_->isEternal(var); }
  bool debugMemory_ = false;
  std::string storeField_;
  llvm::SMLoc storeLoc_;
  struct PublishedObject {
    llvm::Constant *descriptor;
    llvm::StructType *type;
    std::vector<std::pair<unsigned, const Type *>> values;
  };
  std::vector<PublishedObject> publishedObjects_;
  llvm::DenseSet<const FunctionDecl *> aheadFunctions_;
  llvm::DenseMap<llvm::Function *, llvm::Constant *> memberDescriptors_;
  std::vector<std::pair<llvm::Constant *, const Type *>> boxDescriptors_;
  llvm::DenseSet<llvm::Value *> eternalSlots_;
  const ParallelPlan *plan_ = nullptr;
  const MemoryPlan *memory_ = nullptr;
  const FeatureRegistry *features_ = nullptr;
  bool parallel_ = false;
  llvm::DenseMap<const Node *, uint32_t> aheadStarts_;
  llvm::DenseSet<const char *> orderedLocs_;
  llvm::DenseSet<const VarSymbol *> libraryState_;
  llvm::Function *refRetainFn_ = nullptr;
  llvm::Function *plainInstanceReleaseFn_ = nullptr;
  llvm::Function *plainObjReleaseFn_ = nullptr;
  llvm::Function *plainObjRetainFn_ = nullptr;
  llvm::LLVMContext &ctx_;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder_;
  llvm::StructType *strTy_;
  llvm::StructType *arrayTy_;
  llvm::StructType *typeDescTy_;
  llvm::StructType *fnTy_;
  llvm::StructType *fnInfoTy_;
  llvm::StructType *decTy_;
  llvm::Function *function_ = nullptr;
  const Type *returnType_ = nullptr;
  FunctionDecl *currentFn_ = nullptr;
  llvm::Value *envParam_ = nullptr;
  llvm::DenseMap<const VarSymbol *, llvm::Value *> addresses_, globals_;
  llvm::DenseMap<const VarSymbol *, llvm::Value *> boxSlots_, envBoxes_;
  llvm::DenseMap<const NestedGroup *, llvm::Value *> groupEnvs_;
  llvm::DenseMap<const void *, EnvLayout> envLayouts_;
  llvm::DenseMap<const Type *, llvm::Function *> boxDrops_;
  std::vector<FunctionDecl *> pendingClosures_;
  bool comparesFunctions_ = false;
  bool usesTls_ = false;
  const Type *systemError_ = nullptr;
  llvm::DenseMap<const FunctionDecl *, TailGroup *> tailGroupOf_;
  TailGroup *tailGroup_ = nullptr;
  llvm::DenseSet<const Stmt *> tailStatements_;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::DenseMap<const Type *, llvm::Constant *> typeDescriptors_;
  std::map<std::pair<const Type *, std::vector<unsigned>>, llvm::Constant *> layoutDescriptors_;
  llvm::StructType *fieldDescTy_;
  llvm::StructType *layoutDescTy_;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classDescriptors_;
  llvm::DenseMap<llvm::Function *, llvm::Constant *> dropDescriptors_;
  std::map<const Type *, InterfaceInfo> interfaces_;
  llvm::DenseSet<const Type *> withSubtypes_;
  llvm::DenseMap<const Stmt *, std::string> symbolPrefixes_;
  std::vector<Scope> scopes_;
  struct JumpTarget {
    llvm::SmallVector<llvm::StringRef, 1> labels;
    bool isLoop;
    bool isSwitch;
    llvm::BasicBlock *breakTarget;
    llvm::BasicBlock *continueTarget;
    size_t scopeDepth;
    size_t continueDepth = ~size_t(0);
  };
  std::vector<JumpTarget> targets_;
  InlineGenerator *inline_ = nullptr;
  Coroutine *coro_ = nullptr;
  struct IteratorJob {
    llvm::Function *function;
    Builtin builtin;
    const Type *source, *operand, *result;
  };
  std::vector<IteratorJob> pendingIterators_;
  llvm::StructType *promiseTy_ = nullptr;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  std::vector<llvm::BasicBlock *> chainEnds_;
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
  llvm::DenseSet<const CallExpr *> tailJumps_;
  struct TryContext {
    TryStmt *stmt;
    size_t scopeDepth;
    size_t targetsDepth;
    bool inFinally = false;
    llvm::BasicBlock *finally = nullptr;
    llvm::Value *completion = nullptr;
    llvm::Value *pendingValue = nullptr;
    bool returns = false;
    struct Exit {
      size_t target;
      bool isBreak;
    };
    std::vector<Exit> exits;
  };
  std::vector<TryContext> tries_;
  llvm::SMLoc loc_;
  const llvm::SourceMgr *sourceMgr_ = nullptr;

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
  PlaceLowerer &placeLowerer() const { return *placeLowererPart_; }
  TryLowerer &tryLowerer() const { return *tryLowererPart_; }
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
  FsLowerer &fsLowerer() const { return *fsLowererPart_; }
  PathLowerer &pathLowerer() const { return *pathLowererPart_; }
  OsLowerer &osLowerer() const { return *osLowererPart_; }
  ObjectMaker &objectMaker() const { return *objectMakerPart_; }
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
  ErrSites &errSites() const { return *errSitesPart_; }
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
  friend class PlaceLowerer;
  friend class TryLowerer;
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
  friend class FsLowerer;
  friend class PathLowerer;
  friend class OsLowerer;
  friend class ObjectMaker;
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
  friend class ErrSites;
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
  std::unique_ptr<PlaceLowerer> placeLowererPart_;
  std::unique_ptr<TryLowerer> tryLowererPart_;
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
  std::unique_ptr<FsLowerer> fsLowererPart_;
  std::unique_ptr<PathLowerer> pathLowererPart_;
  std::unique_ptr<OsLowerer> osLowererPart_;
  std::unique_ptr<ObjectMaker> objectMakerPart_;
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
  std::unique_ptr<ErrSites> errSitesPart_;
  std::unique_ptr<StreamLowerer> streamLowererPart_;
  std::unique_ptr<TzModulesLowerer> tzModulesLowererPart_;
};

}
