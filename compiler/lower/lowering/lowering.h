#pragma once

#include "compiler/memory/memory_plan/memory_plan.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/pipeline/build_options/build_options.h"
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
class ExitLowerer;
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
class JsonWriters;
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
class DebugInfo;
class StreamLowerer;
class TzModulesLowerer;

class LowererRegistry;
class Lowering {
public:
  Lowering(llvm::LLVMContext &ctx, llvm::StringRef moduleName, const llvm::DataLayout &layout);
  void setSourceMgr(const llvm::SourceMgr *sourceMgr) { sourceMgr_ = sourceMgr; }
  const std::string &mismatch() const { return mismatch_; }
  void setLowerers(const LowererRegistry *registry) {
    this->lowerers = registry;
  }
  void setContexts(bool contexts) { contexts_ = contexts; }
  void setParallelPlan(const ParallelPlan *parallelPlan);
  void setMemoryPlan(const MemoryPlan *memory) { memory_ = memory; }
  void setDebug(DebugLevel level, bool optimizedBuild, bool withCodeView) {
    this->debugLevel = level;
    this->optimized = optimizedBuild;
    this->codeView = withCodeView;
  }
  bool parallel() const { return parallel_; }
  bool contexts() const { return contexts_; }
  void orderAt(llvm::SMLoc loc);

private:
  static const Expr *stripImplicit(const Expr *e);
  static const Expr *awaitedOperand(const Expr *e);
  struct JumpTarget;
  struct TailGroup;
  struct Scope {
    std::vector<std::pair<llvm::Value *, const Type *>> slots;
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
  bool hasSubtypes(const Type *type) const {
    return this->withSubtypes.contains(type);
  }
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
  llvm::DenseMap<const Type *, llvm::Function *> unionRetains, unionReleases;
  std::vector<const Type *> subtypes;
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
    const Type *yielded = nullptr;
    uint64_t header = 16;
  };
  struct DictLayout {
    llvm::StructType *entry = nullptr;
    uint64_t keyOffset = 0, valueOffset = 0, size = 0;
    llvm::Constant *descriptor = nullptr;
    llvm::Constant *plainDescriptor = nullptr;
  };
  struct AsyncState {
    llvm::Value *object = nullptr;
    llvm::Value *reactor = nullptr;
    const Type *value = nullptr;
    bool dropsPromise = false;
    bool generator = false;
  };
  bool cleanupNoAwait = false;
  AsyncState *async = nullptr;
  bool usesAsync = false;
  bool esModule = false;
  llvm::StructType *promiseObjTy = nullptr;
  llvm::DenseMap<const Type *, llvm::Constant *> promiseLayouts, resolverInfos;
  std::map<std::tuple<const Type *, const Type *, const Type *>, llvm::Function *>
      reactionHandlers_;
  std::vector<const ClassDecl *> emittedClasses;
  llvm::DenseMap<const ClassDecl *, llvm::GlobalVariable *> classInfos;
  llvm::SetVector<const Type *> unknownFunctionTypes;
  bool reflectsNew = false, reflectsGet = false;
  llvm::MDNode *tbaaHeader = nullptr, *tbaaData = nullptr;
  llvm::StructType *bytesTy = nullptr;
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
  bool ownerMayBeMember(const Type *type) const { return memory_->refMayCarryMember(type); }
  bool isEternal(const VarSymbol *var) const { return memory_->isEternal(var); }
  bool debugMemory = false;
  std::string storeField;
  llvm::SMLoc storeLoc;
  struct PublishedObject {
    llvm::Constant *descriptor;
    llvm::StructType *type;
    std::vector<std::pair<unsigned, const Type *>> values;
  };
  std::vector<PublishedObject> publishedObjects;
  llvm::DenseSet<const FunctionDecl *> aheadFunctions;
  llvm::DenseSet<const VarSymbol *> ownedParams;
  llvm::DenseSet<const CallExpr *> awaitedCalls;
  bool publishesFrame(const FunctionDecl *fn) const {
    return parallel_ && fn && fn->isAsync && !fn->isGenerator &&
           (this->aheadFunctions.contains(fn) || fn->awaitsAhead);
  }
  void noteAwaited(const Expr *operand);
  void noteAwaitedNext(const Stmt *declared, const Stmt *next);
  void markAwaitedCall(const CallExpr *call, const FunctionDecl *target);
  llvm::DenseMap<llvm::Function *, llvm::Constant *> memberDescriptors;
  std::vector<std::pair<llvm::Constant *, const Type *>> boxDescriptors;
  llvm::DenseSet<llvm::Value *> eternalSlots;
  const ParallelPlan *plan = nullptr;
  const MemoryPlan *memory_ = nullptr;
  const LowererRegistry *lowerers = nullptr;
  bool parallel_ = false;
  bool contexts_ = false;
  llvm::DenseMap<const Node *, uint32_t> aheadStarts;
  llvm::DenseSet<const Node *> freeAwaits;
  llvm::DenseSet<const Node *> movableAwaits;
  llvm::DenseSet<const char *> orderedLocs;
  llvm::DenseSet<const VarSymbol *> libraryState;
  llvm::Function *refRetainFn = nullptr;
  llvm::Function *plainInstanceReleaseFn = nullptr;
  llvm::Function *plainObjReleaseFn = nullptr;
  llvm::Function *plainObjRetainFn = nullptr;
  llvm::LLVMContext &ctx;
  std::unique_ptr<llvm::Module> module_;
  llvm::IRBuilder<> builder;
  llvm::StructType *strTy;
  llvm::StructType *arrayTy;
  llvm::StructType *typeDescTy;
  llvm::StructType *fnTy;
  llvm::StructType *fnInfoTy;
  llvm::StructType *decTy;
  llvm::Function *function = nullptr;
  const Type *returnType = nullptr;
  FunctionDecl *currentFn = nullptr;
  llvm::Value *envParam = nullptr;
  llvm::DenseMap<const VarSymbol *, llvm::Value *> addresses, globals;
  llvm::DenseMap<const VarSymbol *, llvm::Value *> boxSlots, envBoxes;
  llvm::DenseMap<const NestedGroup *, llvm::Value *> groupEnvs;
  llvm::DenseMap<const void *, EnvLayout> envLayouts;
  llvm::DenseMap<const Type *, llvm::Function *> boxDrops;
  std::vector<FunctionDecl *> pendingClosures;
  bool comparesFunctions = false;
  bool usesTls = false;
  const Type *nativeError = nullptr;
  llvm::DenseMap<const FunctionDecl *, TailGroup *> tailGroupOf;
  TailGroup *tailGroup = nullptr;
  llvm::DenseSet<const Stmt *> tailStatements;
  llvm::DenseMap<const FuncSymbol *, llvm::Function *> functions_;
  llvm::DenseMap<const Type *, llvm::Constant *> typeDescriptors_;
  std::map<std::pair<const Type *, std::vector<unsigned>>, llvm::Constant *> layoutDescriptors;
  llvm::StructType *fieldDescTy;
  llvm::StructType *layoutDescTy;
  llvm::DenseMap<const ClassDecl *, llvm::Constant *> classDescriptors_;
  llvm::DenseMap<llvm::Function *, llvm::Constant *> dropDescriptors;
  std::map<const Type *, InterfaceInfo> interfaces;
  llvm::DenseSet<const Type *> withSubtypes;
  llvm::DenseMap<const Stmt *, std::string> symbolPrefixes;
  std::vector<Scope> scopes;
  struct JumpTarget {
    llvm::SmallVector<llvm::StringRef, 1> labels;
    bool isLoop;
    bool isSwitch;
    llvm::BasicBlock *breakTarget;
    llvm::BasicBlock *continueTarget;
    size_t scopeDepth;
    size_t continueDepth = ~size_t(0);
  };
  std::vector<JumpTarget> targets;
  InlineGenerator *inline_ = nullptr;
  Coroutine *coro = nullptr;
  struct IteratorJob {
    llvm::Function *function;
    Builtin builtin;
    const Type *source, *operand, *result;
  };
  std::vector<IteratorJob> pendingIterators;
  llvm::StructType *promiseTy = nullptr;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels;
  std::vector<llvm::BasicBlock *> chainEnds;
  struct Runtime {
    llvm::FunctionCallee init;
    llvm::FunctionCallee shutdown;
    llvm::FunctionCallee mainDone;
    llvm::FunctionCallee printU64;
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
    llvm::FunctionCallee strFromU64;
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
  llvm::Function *retainFn = nullptr;
  llvm::Function *releaseFn = nullptr;
  llvm::Function *arrRetainFn = nullptr;
  llvm::Function *arrReleaseFn = nullptr;
  llvm::Function *objRetainFn = nullptr;
  llvm::Function *objReleaseFn = nullptr;
  llvm::Function *instanceReleaseFn = nullptr;
  llvm::DenseSet<const CallExpr *> tailJumps;
  llvm::SMLoc loc_;
  DebugLevel debugLevel = DebugLevel::None;
  bool optimized = true;
  bool codeView = true;
  const llvm::SourceMgr *sourceMgr_ = nullptr;
  std::string mismatch_;

public:
  ~Lowering();

  ModuleBuilder &moduleBuilder() const {
    return *this->moduleBuilderPart;
  }
  FunctionBuilder &functionBuilder() const {
    return *this->functionBuilderPart;
  }
  RuntimeFunctions &runtimeFunctions() const {
    return *this->runtimeFunctionsPart;
  }
  FunctionFrame &functionFrame() const {
    return *this->functionFramePart;
  }
  RefCounts &refCounts() const {
    return *this->refCountsPart;
  }
  OptionalLowerer &optionalLowerer() const {
    return *this->optionalLowererPart;
  }
  StmtLowerer &stmtLowerer() const {
    return *this->stmtLowererPart;
  }
  LoopLowerer &loopLowerer() const {
    return *this->loopLowererPart;
  }
  SwitchLowerer &switchLowerer() const {
    return *this->switchLowererPart;
  }
  ExprLowerer &exprLowerer() const {
    return *this->exprLowererPart;
  }
  UnaryLowerer &unaryLowerer() const {
    return *this->unaryLowererPart;
  }
  BinaryLowerer &binaryLowerer() const {
    return *this->binaryLowererPart;
  }
  EqualityLowerer &equalityLowerer() const {
    return *this->equalityLowererPart;
  }
  TruthLowerer &truthLowerer() const {
    return *this->truthLowererPart;
  }
  AssignLowerer &assignLowerer() const {
    return *this->assignLowererPart;
  }
  CallLowerer &callLowerer() const {
    return *this->callLowererPart;
  }
  ConsoleLowerer &consoleLowerer() const {
    return *this->consoleLowererPart;
  }
  StringLowerer &stringLowerer() const {
    return *this->stringLowererPart;
  }
  StringMethodLowerer &stringMethodLowerer() const {
    return *this->stringMethodLowererPart;
  }
  MemberLowerer &memberLowerer() const {
    return *this->memberLowererPart;
  }
  TypeDescriptors &typeDescriptors() const {
    return *this->typeDescriptorsPart;
  }
  ArrayLowerer &arrayLowerer() const {
    return *this->arrayLowererPart;
  }
  ArrayMethodLowerer &arrayMethodLowerer() const {
    return *this->arrayMethodLowererPart;
  }
  CallbackMethodLowerer &callbackMethodLowerer() const {
    return *this->callbackMethodLowererPart;
  }
  PromiseLowerer &promiseLowerer() const {
    return *this->promiseLowererPart;
  }
  AsyncLowerer &asyncLowerer() const {
    return *this->asyncLowererPart;
  }
  PromiseCombineLowerer &promiseCombineLowerer() const {
    return *this->promiseCombineLowererPart;
  }
  ReactionLowerer &reactionLowerer() const {
    return *this->reactionLowererPart;
  }
  AsyncBuiltinLowerer &asyncBuiltinLowerer() const {
    return *this->asyncBuiltinLowererPart;
  }
  AsyncGeneratorLowerer &asyncGeneratorLowerer() const {
    return *this->asyncGeneratorLowererPart;
  }
  MathLowerer &mathLowerer() const {
    return *this->mathLowererPart;
  }
  NumberLowerer &numberLowerer() const {
    return *this->numberLowererPart;
  }
  ClassDescriptors &classDescriptors() const {
    return *this->classDescriptorsPart;
  }
  ClassLowerer &classLowerer() const {
    return *this->classLowererPart;
  }
  ClosureLowerer &closureLowerer() const {
    return *this->closureLowererPart;
  }
  EnvLowerer &envLowerer() const {
    return *this->envLowererPart;
  }
  GroupLowerer &groupLowerer() const {
    return *this->groupLowererPart;
  }
  StoreLowerer &storeLowerer() const {
    return *this->storeLowererPart;
  }
  WeakRefLowerer &weakRefLowerer() const {
    return *this->weakRefLowererPart;
  }
  MemoryDebug &memoryDebug() const {
    return *this->memoryDebugPart;
  }
  PublishLowerer &publishLowerer() const {
    return *this->publishLowererPart;
  }
  DecoratorLowerer &decoratorLowerer() const {
    return *this->decoratorLowererPart;
  }
  DictLowerer &dictLowerer() const {
    return *this->dictLowererPart;
  }
  DictMethodLowerer &dictMethodLowerer() const {
    return *this->dictMethodLowererPart;
  }
  InheritLowerer &inheritLowerer() const {
    return *this->inheritLowererPart;
  }
  CoroutineLowerer &coroutineLowerer() const {
    return *this->coroutineLowererPart;
  }
  GeneratorLowerer &generatorLowerer() const {
    return *this->generatorLowererPart;
  }
  LazyChainLowerer &lazyChainLowerer() const {
    return *this->lazyChainLowererPart;
  }
  InlineGeneratorLowerer &inlineGeneratorLowerer() const {
    return *this->inlineGeneratorLowererPart;
  }
  IteratorLowerer &iteratorLowerer() const {
    return *this->iteratorLowererPart;
  }
  ObjectLayouts &objectLayouts() const {
    return *this->objectLayoutsPart;
  }
  ObjectLowerer &objectLowerer() const {
    return *this->objectLowererPart;
  }
  TailCallLowerer &tailCallLowerer() const {
    return *this->tailCallLowererPart;
  }
  PlaceLowerer &placeLowerer() const {
    return *this->placeLowererPart;
  }
  ExitLowerer &exitLowerer() const {
    return *this->exitLowererPart;
  }
  UnionLayout &unionLayout() const {
    return *this->unionLayoutPart;
  }
  UnionLowerer &unionLowerer() const {
    return *this->unionLowererPart;
  }
  UnionMemberLowerer &unionMemberLowerer() const {
    return *this->unionMemberLowererPart;
  }
  UnknownLowerer &unknownLowerer() const {
    return *this->unknownLowererPart;
  }
  BytesLowerer &bytesLowerer() const {
    return *this->bytesLowererPart;
  }
  BytesCallbackLowerer &bytesCallbackLowerer() const {
    return *this->bytesCallbackLowererPart;
  }
  BytesCallLowerer &bytesCallLowerer() const {
    return *this->bytesCallLowererPart;
  }
  ChildLowerer &childLowerer() const {
    return *this->childLowererPart;
  }
  CryptoLowerer &cryptoLowerer() const {
    return *this->cryptoLowererPart;
  }
  DateLowerer &dateLowerer() const {
    return *this->dateLowererPart;
  }
  DecimalLowerer &decimalLowerer() const {
    return *this->decimalLowererPart;
  }
  LibraryCallLowerer &libraryCallLowerer() const {
    return *this->libraryCallLowererPart;
  }
  IntlLowerer &intlLowerer() const {
    return *this->intlLowererPart;
  }
  IntlCallLowerer &intlCallLowerer() const {
    return *this->intlCallLowererPart;
  }
  JsonLowerer &jsonLowerer() const {
    return *this->jsonLowererPart;
  }
  JsonWriters &jsonWriters() const {
    return *this->jsonWritersPart;
  }
  FsPromisesLowerer &fsPromisesLowerer() const {
    return *this->fsPromisesLowererPart;
  }
  FsLowerer &fsLowerer() const {
    return *this->fsLowererPart;
  }
  PathLowerer &pathLowerer() const {
    return *this->pathLowererPart;
  }
  OsLowerer &osLowerer() const {
    return *this->osLowererPart;
  }
  ObjectMaker &objectMaker() const {
    return *this->objectMakerPart;
  }
  ProcessLowerer &processLowerer() const {
    return *this->processLowererPart;
  }
  UnknownCastLowerer &unknownCastLowerer() const {
    return *this->unknownCastLowererPart;
  }
  ReflectArgsLowerer &reflectArgsLowerer() const {
    return *this->reflectArgsLowererPart;
  }
  ClassInfoLowerer &classInfoLowerer() const {
    return *this->classInfoLowererPart;
  }
  ReflectDispatchLowerer &reflectDispatchLowerer() const {
    return *this->reflectDispatchLowererPart;
  }
  ReflectLowerer &reflectLowerer() const {
    return *this->reflectLowererPart;
  }
  RegexLowerer &regexLowerer() const {
    return *this->regexLowererPart;
  }
  RegexNativeLowerer &regexNativeLowerer() const {
    return *this->regexNativeLowererPart;
  }
  ResultLowerer &resultLowerer() const {
    return *this->resultLowererPart;
  }
  ResultCombineLowerer &resultCombineLowerer() const {
    return *this->resultCombineLowererPart;
  }
  ErrSites &errSites() const {
    return *this->errSitesPart;
  }
  DebugInfo &debugInfo() const {
    return *this->debugInfoPart;
  }
  StreamLowerer &streamLowerer() const {
    return *this->streamLowererPart;
  }
  TzModulesLowerer &tzModulesLowerer() const {
    return *this->tzModulesLowererPart;
  }

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
  friend class ExitLowerer;
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
  friend class JsonWriters;
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
  friend class DebugInfo;
  friend class DebugTypes;
  friend class StreamLowerer;
  friend class TzModulesLowerer;
  std::unique_ptr<ModuleBuilder> moduleBuilderPart;
  std::unique_ptr<FunctionBuilder> functionBuilderPart;
  std::unique_ptr<RuntimeFunctions> runtimeFunctionsPart;
  std::unique_ptr<FunctionFrame> functionFramePart;
  std::unique_ptr<RefCounts> refCountsPart;
  std::unique_ptr<OptionalLowerer> optionalLowererPart;
  std::unique_ptr<StmtLowerer> stmtLowererPart;
  std::unique_ptr<LoopLowerer> loopLowererPart;
  std::unique_ptr<SwitchLowerer> switchLowererPart;
  std::unique_ptr<ExprLowerer> exprLowererPart;
  std::unique_ptr<UnaryLowerer> unaryLowererPart;
  std::unique_ptr<BinaryLowerer> binaryLowererPart;
  std::unique_ptr<EqualityLowerer> equalityLowererPart;
  std::unique_ptr<TruthLowerer> truthLowererPart;
  std::unique_ptr<AssignLowerer> assignLowererPart;
  std::unique_ptr<CallLowerer> callLowererPart;
  std::unique_ptr<ConsoleLowerer> consoleLowererPart;
  std::unique_ptr<StringLowerer> stringLowererPart;
  std::unique_ptr<StringMethodLowerer> stringMethodLowererPart;
  std::unique_ptr<MemberLowerer> memberLowererPart;
  std::unique_ptr<TypeDescriptors> typeDescriptorsPart;
  std::unique_ptr<ArrayLowerer> arrayLowererPart;
  std::unique_ptr<ArrayMethodLowerer> arrayMethodLowererPart;
  std::unique_ptr<CallbackMethodLowerer> callbackMethodLowererPart;
  std::unique_ptr<PromiseLowerer> promiseLowererPart;
  std::unique_ptr<AsyncLowerer> asyncLowererPart;
  std::unique_ptr<PromiseCombineLowerer> promiseCombineLowererPart;
  std::unique_ptr<ReactionLowerer> reactionLowererPart;
  std::unique_ptr<AsyncBuiltinLowerer> asyncBuiltinLowererPart;
  std::unique_ptr<AsyncGeneratorLowerer> asyncGeneratorLowererPart;
  std::unique_ptr<MathLowerer> mathLowererPart;
  std::unique_ptr<NumberLowerer> numberLowererPart;
  std::unique_ptr<ClassDescriptors> classDescriptorsPart;
  std::unique_ptr<ClassLowerer> classLowererPart;
  std::unique_ptr<ClosureLowerer> closureLowererPart;
  std::unique_ptr<EnvLowerer> envLowererPart;
  std::unique_ptr<GroupLowerer> groupLowererPart;
  std::unique_ptr<StoreLowerer> storeLowererPart;
  std::unique_ptr<WeakRefLowerer> weakRefLowererPart;
  std::unique_ptr<MemoryDebug> memoryDebugPart;
  std::unique_ptr<PublishLowerer> publishLowererPart;
  std::unique_ptr<DecoratorLowerer> decoratorLowererPart;
  std::unique_ptr<DictLowerer> dictLowererPart;
  std::unique_ptr<DictMethodLowerer> dictMethodLowererPart;
  std::unique_ptr<InheritLowerer> inheritLowererPart;
  std::unique_ptr<CoroutineLowerer> coroutineLowererPart;
  std::unique_ptr<GeneratorLowerer> generatorLowererPart;
  std::unique_ptr<LazyChainLowerer> lazyChainLowererPart;
  std::unique_ptr<InlineGeneratorLowerer> inlineGeneratorLowererPart;
  std::unique_ptr<IteratorLowerer> iteratorLowererPart;
  std::unique_ptr<ObjectLayouts> objectLayoutsPart;
  std::unique_ptr<ObjectLowerer> objectLowererPart;
  std::unique_ptr<TailCallLowerer> tailCallLowererPart;
  std::unique_ptr<PlaceLowerer> placeLowererPart;
  std::unique_ptr<ExitLowerer> exitLowererPart;
  std::unique_ptr<UnionLayout> unionLayoutPart;
  std::unique_ptr<UnionLowerer> unionLowererPart;
  std::unique_ptr<UnionMemberLowerer> unionMemberLowererPart;
  std::unique_ptr<UnknownLowerer> unknownLowererPart;
  std::unique_ptr<BytesLowerer> bytesLowererPart;
  std::unique_ptr<BytesCallbackLowerer> bytesCallbackLowererPart;
  std::unique_ptr<BytesCallLowerer> bytesCallLowererPart;
  std::unique_ptr<ChildLowerer> childLowererPart;
  std::unique_ptr<CryptoLowerer> cryptoLowererPart;
  std::unique_ptr<DateLowerer> dateLowererPart;
  std::unique_ptr<DecimalLowerer> decimalLowererPart;
  std::unique_ptr<LibraryCallLowerer> libraryCallLowererPart;
  std::unique_ptr<IntlLowerer> intlLowererPart;
  std::unique_ptr<IntlCallLowerer> intlCallLowererPart;
  std::unique_ptr<JsonLowerer> jsonLowererPart;
  std::unique_ptr<JsonWriters> jsonWritersPart;
  std::unique_ptr<FsPromisesLowerer> fsPromisesLowererPart;
  std::unique_ptr<FsLowerer> fsLowererPart;
  std::unique_ptr<PathLowerer> pathLowererPart;
  std::unique_ptr<OsLowerer> osLowererPart;
  std::unique_ptr<ObjectMaker> objectMakerPart;
  std::unique_ptr<ProcessLowerer> processLowererPart;
  std::unique_ptr<UnknownCastLowerer> unknownCastLowererPart;
  std::unique_ptr<ReflectArgsLowerer> reflectArgsLowererPart;
  std::unique_ptr<ClassInfoLowerer> classInfoLowererPart;
  std::unique_ptr<ReflectDispatchLowerer> reflectDispatchLowererPart;
  std::unique_ptr<ReflectLowerer> reflectLowererPart;
  std::unique_ptr<RegexLowerer> regexLowererPart;
  std::unique_ptr<RegexNativeLowerer> regexNativeLowererPart;
  std::unique_ptr<ResultLowerer> resultLowererPart;
  std::unique_ptr<ResultCombineLowerer> resultCombineLowererPart;
  std::unique_ptr<ErrSites> errSitesPart;
  std::unique_ptr<DebugInfo> debugInfoPart;
  std::unique_ptr<StreamLowerer> streamLowererPart;
  std::unique_ptr<TzModulesLowerer> tzModulesLowererPart;
};

}
