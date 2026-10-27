#pragma once

#include "compiler/check/facts/facts.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include <deque>
#include <map>
#include <memory>
#include <optional>

namespace tz {

enum class NodeModule {
  Fs,
  FsPromises,
  Path,
  PathPosix,
  PathWin32,
  Os,
  Process,
  Buffer,
  TzIo,
  TimersPromises,
  Readline,
  ReadlinePromises,
  ChildProcess,
  Crypto,
};

std::string cryptoExportProblem(llvm::StringRef name, llvm::StringRef path);

inline constexpr llvm::StringLiteral kSubtleName = "__tz_subtle";
inline constexpr llvm::StringLiteral kIntlLocaleName = "__tz_IntlLocale";

std::string builtinModuleProblem(llvm::StringRef path);

struct ValueRange {
  std::optional<int64_t> lo, hi;
};

struct EventInference {
  struct Portable {
    enum class Kind {
      Same,
      Class,
      ClassRef,
      Optional,
      Array,
      Tuple,
      Function,
      Union,
      Object,
      Promise,
      Dict,
      Iterator,
      Result,
      Bad,
    };
    Kind kind = Kind::Same;
    const Type *same = nullptr;
    std::string file, name;
    std::vector<Portable> parts;
    std::vector<std::string> names;
    std::vector<bool> readonly;
    size_t min = 0;
    bool rest = false;
    int dict = 0;
  };
  struct Event {
    std::string name;
    Portable args;
    std::string where;
  };
  std::map<std::string, std::vector<Event>> maps;
  bool open = false;
  bool changed = false;
  bool final = false;
};

class DeclarePass;
class FeatureRegistry;
class InferPass;
class CheckPass;
class InstancesPass;
class WholeProgramPass;
class Scopes;
class TypeResolver;
class TypeRelations;
class CallCycles;
class FunctionChecker;
class ReturnChecker;
class StmtChecker;
class JumpChecker;
class LoopChecker;
class SwitchChecker;
class VarChecker;
class ExprChecker;
class NameChecker;
class LiteralChecker;
class UnaryChecker;
class BinaryChecker;
class ConcatChecker;
class AssignChecker;
class TargetChecker;
class CallChecker;
class ArgumentChecker;
class ConsoleChecker;
class MemberChecker;
class ArgListChecker;
class AsyncChecker;
class PromiseChecker;
class PromiseStaticChecker;
class TimerChecker;
class MathChecker;
class GlobalFunctionChecker;
class NumberMethods;
class StringMethods;
class ArrayMethods;
class NewChecker;
class ArrayLiteralChecker;
class ClassDeclarer;
class ClassChecker;
class ClassMemberChecker;
class StaticChecker;
class MethodCallChecker;
class InstanceOfChecker;
class OverloadResolver;
class MethodLiteralChecker;
class ClassValueChecker;
class LateClasses;
class ClosureChecker;
class NestedFunctions;
class FunctionValueChecker;
class CallbackChecker;
class ConditionalTypes;
class ExtendsRelation;
class TemplateTypes;
class TypeDeclarer;
class ConstantFolder;
class FunctionMethodChecker;
class DecoratorChecker;
class MetadataChecker;
class Destination;
class DestinationScanner;
class DictChecker;
class DictMethods;
class DictNewChecker;
class RecordLiteralChecker;

class FlowChecker;
class IndexProofs;
class ConditionFacts;
class LoopScanner;
class LoopBounds;
class TypeParams;
class GenericChecker;
class TypeArgInferrer;
class GenericInstantiator;
class AliasInstantiator;
class GenericClassChecker;
class GenericClassInstantiator;
class ReturnInferrer;
class ClassBases;
class ClassLinks;
class InheritChecker;
class ConstructorCalls;
class GeneratorChecker;
class IteratorMethods;
class IterableChecker;
class ModuleDeclarer;
class ImportBinder;
class TopLevelChecker;
class LoadOrder;
class Unused;
class ObjectLiteralChecker;
class KeyedFieldChecker;
class TupleChecker;
class ObjectChecker;
class FieldFacts;
class PredicateChecker;
class TruthChecker;
class TypeOperators;
class UtilityTypes;
class Prelude;
class LiteralTypes;
class UnionNarrowing;
class UnionChecker;
class UnknownChecker;
class WideWrites;
class ByteProofs;
class BytesChecker;
class BytesNewChecker;
class TypedArrayChecker;
class BufferChecker;
class BufferStaticChecker;
class ChildChecker;
class CryptoChecker;
class CryptoArgs;
class CryptoFunctionChecker;
class KeyPairChecker;
class CryptoMethodChecker;
class WebCryptoChecker;
class SubtleCallChecker;
class DateChecker;
class DateFormatChecker;
class DecimalChecker;
class LibraryCalls;
class EventMaps;
class EventMapInference;
class EmitterChecker;
class EventTargetChecker;
class IntlChecker;
class IntlOptionsChecker;
class IntlMethodChecker;
class JsonChecker;
class JsonTypeChecker;
class JsonStaticChecker;
class BuiltinModuleChecker;
class FsChecker;
class FsDeclarer;
class PathChecker;
class OsChecker;
class RuntimeObjectChecker;
class FsPromisesChecker;
class ReadlineChecker;
class ProcessChecker;
class ReflectChecker;
class ReflectCallChecker;
class RegexChecker;
class StringRegexChecker;
class MatchChecker;
class ResultTypes;
class ResultChecker;
class ResultMethodChecker;
class ChunkTypes;
class StreamNewChecker;
class StreamChecker;
class SetEncodingChecker;
class StreamSources;
class StreamPipeline;
class TzModulesChecker;
class UrlChecker;
class HttpChecker;
class WeakRefChecker;

class Checker {
public:
  Checker(AstContext &ctx, Diagnostics &diags);
  static const Type *awaitedTypeOf(const Type *type);
  void setEventInference(EventInference *inference) { inference_ = inference; }

private:
  struct ScopeEntry {
    VarSymbol *var = nullptr;
    FuncSymbol *func = nullptr;
    EnumDecl *enumDecl = nullptr;
    ClassDecl *cls = nullptr;
  };
  struct FieldPath {
    const VarSymbol *var = nullptr;
    llvm::SmallVector<llvm::StringRef, 2> names;
    bool operator==(const FieldPath &other) const {
      return var == other.var && names == other.names;
    }
  };
  struct EmitterKey {
    FieldPath path;
    std::string text;
    const Expr *root = nullptr;
    bool operator==(const EmitterKey &other) const {
      return path == other.path && text == other.text && root == other.root;
    }
  };
  struct Flow {
    Facts facts;
    llvm::SmallPtrSet<const VarSymbol *, 4> defined;
    std::vector<FieldPath> definedFields;
    std::vector<std::pair<FieldPath, const Type *>> narrowedFields;
    std::vector<std::pair<const VarSymbol *, const Type *>> narrowed;
    llvm::SmallPtrSet<const VarSymbol *, 2> okResults, errResults;
    std::vector<llvm::StringRef> setFields;
    std::vector<EmitterKey> errorListened;
    std::vector<std::pair<EmitterKey, llvm::SMLoc>> errorPending;
    bool beforeSuper = false;
    bool unreachable = false;
  };
  struct LoopScan {
    struct Change {
      unsigned count = 0;
      int64_t delta = 0;
      bool nested = false;
    };
    llvm::StringMap<Change> changes;
    bool callsFunctions = false;
    bool growsArrays = false;
    bool shrinksArrays = false;
    llvm::StringSet<> fieldWrites;
    bool releasesElements = false;
    bool breaks = false;
    unsigned switches = 0;
    llvm::SmallVector<llvm::StringRef, 2> innerLabels;
  };
  struct JumpTarget {
    enum class Kind { Loop, Switch, Block };
    Kind kind;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    Flow breaks;
  };
  void pushScope() {
    scopes_.emplace_back();
    typeScopes_.emplace_back();
    scopeBranches_.push_back(branchDepth_);
  }
  void popScope();
  int branchDepth_ = 0;
  std::vector<int> scopeBranches_;
  struct Signature;
  struct Instance {
    FunctionDecl *decl = nullptr;
    FuncSymbol *generic = nullptr;
    ClassDecl *cls = nullptr;
    std::vector<const Type *> args;
    std::vector<const Type *> concrete;
    std::vector<const Type *> exact;
    llvm::SMLoc callLoc;
    const Instance *requester = nullptr;
    bool checking = false, checked = false, failed = false;
    std::string message;
    llvm::SMLoc errorLoc;
  };
  static bool sameClass(const ClassDecl *a, const ClassDecl *b) {
    return a && b && a->staticOwner() == b->staticOwner();
  }
  struct Constant {
    bool isString = false;
    double number = 0;
    std::u16string text;
  };
  struct BlockNames {
    size_t functions;
    std::vector<llvm::StringRef> names;
  };
  struct BodyState {
    Flow flow;
    std::vector<llvm::StringMap<ScopeEntry>> scopes;
    std::vector<llvm::StringMap<Stmt *>> typeScopes;
    std::vector<FunctionDecl *> functions;
    std::vector<NestedGroup *> groups;
    std::vector<BlockNames> blockNames;
    FuncSymbol *function = nullptr;
    std::vector<JumpTarget> targets;
    llvm::SmallVector<llvm::StringRef, 1> pendingLabels;
    llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars;
    bool chainShortCircuits = false;
    std::vector<ReturnStmt *> *returns = nullptr;
    std::vector<YieldExpr *> *yields = nullptr;
    bool trial = false;
    const FuncSymbol *early = nullptr;
    Module *module = nullptr;
    Instance *instance = nullptr;
    std::vector<Diagnostics::Captured> *capture = nullptr;
    std::vector<int> scopeBranches;
    int branchDepth = 0;
  };
  struct Signature {
    std::vector<const Type *> params;
    size_t minArgs = 0;
    const Type *ret = nullptr;
    bool voidResult = false;
    bool pad = true;
    bool asyncVoid = false;
    bool hasThis = false;
  };
  FunctionDecl *currentDecl() const { return functions_.empty() ? nullptr : functions_.back(); }
  static bool holdsFunctions(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isFunction();
  }
  static bool holdsObjects(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isObject() || type->isDict();
  }
  static bool holdsDates(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isDate();
  }
  enum class TruthUse { Condition, Not, Or, And, OrAssign, AndAssign };
  struct NodeName {
    NodeModule module;
    llvm::StringRef name;
  };
  bool fsAsync_ = false;
  struct DestinationScan;
  static bool holdsResults(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isResult();
  }
  using ClassProblem = llvm::function_ref<std::string(const ClassDecl *)>;
  struct NarrowTarget {
    const VarSymbol *var = nullptr;
    std::optional<FieldPath> path;
    const Type *declared = nullptr;
  };
  struct PredicateSide {
    bool unreachable = false;
    std::vector<std::pair<int, const Type *>> narrowed;
    std::vector<int> defined;
    std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>> fields;
    std::vector<Facts::Fact> facts;
  };
  struct Predicate {
    PredicateSide whenTrue, whenFalse;
    std::vector<const VarSymbol *> params;
    const VarSymbol *self = nullptr;
  };
  struct PredicateBuild {
    const FuncSymbol *fn;
    const FunctionDecl *decl;
    Flow whenTrue, whenFalse;
  };
  std::vector<PredicateBuild> predicateBuilds_;
  static bool holdsPromises(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isPromise();
  }
  struct ByteCallback {
    unsigned param = 0;
    int64_t lo = 0, hi = 0;
    const Type *target = nullptr;
  };
  llvm::SmallPtrSet<const CallExpr *, 4> severalVariants_;
  llvm::DenseMap<const CastExpr *, ClassDecl *> constructorCasts_;
  std::vector<std::pair<const NewExpr *, std::vector<const Type *>>> reflectNews_;
  llvm::DenseMap<const Type *, llvm::SmallPtrSet<const Type *, 4>> constructorWidenings_;
  ClassDecl *declaringClass_ = nullptr;
  std::vector<ClassDecl *> lateClasses_;
  size_t lateChecked_ = 0;
  llvm::DenseMap<const ClassDecl *, Instance *> lateOwners_;
  std::map<Instance *, std::vector<Diagnostics::Captured>> lateErrors_;
  bool reflectionUsed_ = false;
  llvm::DenseMap<const Type *, std::pair<const Type *, const Type *>> descriptorOrigins_;
  struct DecoratorUse {
    const Decorator *decorator;
    int param = -1;
    const FunctionDecl *owner = nullptr;
  };
  struct MetaKeyType {
    const Type *type;
    llvm::SMLoc loc;
  };
  llvm::DenseMap<const void *, MetaKeyType> metaTypes_;
  ClassDecl *libraryClass(llvm::StringRef name) const { return libClasses_.lookup(name); }
  enum class ListenerKind { Plain, Async, This };
  struct StreamChunks {
    const Type *readable = nullptr;
    const Type *writable = nullptr;
  };
  struct StreamView {
    FieldPath path;
    llvm::SMLoc from;
    const Type *type;
  };
  std::vector<StreamView> streamViews_;
  struct StreamCallbackSite {
    const FunctionDecl *fn;
    llvm::StringRef param;
    const Type *chunk;
  };
  std::vector<StreamCallbackSite> streamCallbacks_;
  EventInference *inference_ = nullptr;
  llvm::DenseMap<const Type *, std::string> openMaps_, inferredMaps_;
  llvm::StringMap<ClassDecl *> libClasses_;
  llvm::StringMap<FunctionDecl *> libFunctions_;
  llvm::StringMap<TypeAliasDecl *> libTypes_;
  llvm::DenseMap<const VarSymbol *, const Expr *> constLiterals_;
  bool isDefined(const VarSymbol *var) const { return flow_.defined.contains(var); }
  struct Binding {
    Module *module = nullptr;
    llvm::StringRef name;
    bool operator==(const Binding &other) const {
      return module == other.module && name == other.name;
    }
  };
  struct Export {
    Binding target;
    llvm::SMLoc loc;
    const ExportDecl *all = nullptr;
  };
  struct Import {
    Binding target;
    llvm::SMLoc loc;
  };
  struct ModuleState {
    llvm::StringMap<ScopeEntry> values;
    llvm::StringMap<Stmt *> types;
    llvm::StringMap<llvm::SMLoc> declared;
    llvm::StringSet<> topLevelNames;
    llvm::StringMap<Import> imports;
    llvm::StringSet<> badImports;
    llvm::SmallPtrSet<const VarSymbol *, 4> importedVars;
    llvm::StringMap<NodeName> nodeNames;
    llvm::StringMap<Export> exports;
    bool exportsDone = false, exportsBusy = false;
    size_t next = 0;
    std::optional<Flow> flow;
    bool busy = false;
    bool loaded = false;
  };
  ModuleState &state(const Module *module) { return moduleStates_[module->order]; }
  AstContext &ctx_;
  Diagnostics &diags_;
  std::vector<llvm::StringMap<ScopeEntry>> scopes_;
  std::vector<llvm::StringMap<Stmt *>> typeScopes_;
  FuncSymbol *currentFunction_ = nullptr;
  std::vector<JumpTarget> targets_;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars_;
  Flow flow_;
  std::vector<std::pair<const Type *, llvm::SMLoc>> errorEmits_;
  llvm::SmallPtrSet<const Type *, 4> errorListenedMaps_;
  llvm::DenseSet<const void *> reportedErrorSubscriptions_;
  llvm::StringSet<> namesInFunctions_;
  llvm::StringSet<> assignedFields_;
  llvm::ArrayRef<Stmt *> block_;
  size_t blockIndex_ = 0;
  std::vector<const Stmt *> following_;
  llvm::DenseMap<const VarSymbol *, std::vector<std::pair<const Type *, llvm::SMLoc>>>
      destinations_;
  std::vector<std::pair<const Type *, llvm::SMLoc>> destinationConflict_;
  llvm::DenseMap<const VarSymbol *, std::string> constKeys_;
  llvm::SmallVector<llvm::StringRef, 2> initializing_;
  bool chainShortCircuits_ = false;
  BinaryExpr *concatChain_ = nullptr;
  const BinaryExpr *mixedConcat_ = nullptr;
  std::vector<ReturnStmt *> *returns_ = nullptr;
  std::vector<YieldExpr *> *yields_ = nullptr;
  bool trial_ = false;
  bool topLevelDone_ = false;
  const FuncSymbol *early_ = nullptr;
  std::vector<std::vector<FuncSymbol *>> cycles_;
  llvm::DenseMap<const FuncSymbol *, unsigned> cycleOf_;
  struct Untyped {
    VarSymbol *var;
    bool initUndefined;
    std::vector<const Type *> assigned;
  };
  std::vector<Untyped> untyped_;
  llvm::DenseMap<const VarSymbol *, int64_t> constVars_;
  llvm::DenseMap<const VarSymbol *, int64_t> fixedLengths_;
  std::vector<FunctionDecl *> functions_;
  std::vector<NestedGroup *> groups_;
  unsigned seq_ = 0;
  SourceFile *file_ = nullptr;
  const FeatureRegistry *features_ = nullptr;
  std::vector<FunctionDecl *> declaredFunctions_;
  std::vector<FunctionDecl *> lifted_;
  std::deque<Instance> instanceStore_;
  std::vector<Instance *> pendingInstances_;
  Instance *checkingInstance_ = nullptr;
  llvm::SmallPtrSet<const FuncSymbol *, 4> brokenGenerics_;
  llvm::DenseMap<const FuncSymbol *, std::vector<bool>> exactParams_;
  std::map<std::pair<const ClassDecl *, std::vector<const Type *>>, ClassDecl *> classInstances_;
  llvm::DenseMap<const ClassDecl *, std::vector<const Type *>> classDefaults_;
  std::vector<ClassDecl *> unfilled_;
  std::vector<const ClassDecl *> filling_;
  llvm::StringMap<TypeAliasDecl *> builtinAliases_;
  struct InferMatch {
    llvm::ArrayRef<const Type *> params;
    std::vector<const Type *> bound;
  };
  struct ConditionalInfo {
    const TypeRef *ref = nullptr;
    const Type *tag = nullptr;
    std::vector<llvm::StringRef> bound;
    std::vector<std::pair<llvm::StringRef, Stmt *>> decls;
  };
  unsigned extendsClause_ = 0;
  struct TailCall {
    TypeAliasDecl *alias = nullptr;
    std::vector<const Type *> args;
  };
  bool tailAllowed_ = false;
  std::optional<TailCall> tailCall_;
  llvm::DenseSet<const CastExpr *> widenings_;
  bool narrowedFields_ = false;
  bool listContext_ = false;
  llvm::SMLoc instanceLoc_;
  bool methodsDeclared_ = false;
  bool classesLinked_ = false;
  llvm::SmallPtrSet<const ClassDecl *, 2> brokenClasses_;
  llvm::DenseMap<const char *, const Instance *> failedCalls_;
  llvm::DenseMap<std::pair<const char *, unsigned>, const Type *> forced_;
  struct AliasInstance {
    const Type *type = nullptr;
    Type *self = nullptr;
    bool resolving = false;
    bool recursive = false;
  };
  llvm::DenseMap<const Type *, std::pair<TypeAliasDecl *, std::vector<const Type *>>> aliasOrigins_;
  std::vector<BlockNames> blockNames_;
  const BlockNames *declaredLater(llvm::StringRef name) const {
    for (const BlockNames &block : llvm::reverse(blockNames_)) {
      if (llvm::is_contained(block.names, name)) {
        return &block;
      }
    }

    return nullptr;
  }
  std::vector<ModuleState> moduleStates_;
  Module *module_ = nullptr;
  Module *loading_ = nullptr;
  llvm::DenseMap<const Stmt *, Module *> declModules_;
  ClassDecl *currentClass_ = nullptr;
  FunctionDecl *initFn_ = nullptr;
  const VarSymbol *initThis_ = nullptr;
  bool closureThisReady_ = true;
  bool staticInit_ = false;
  llvm::StringRef thisMember_;
  const Signature *callback_ = nullptr;
  llvm::SmallPtrSet<const VarSymbol *, 4> staticsSet_;
  unsigned classCount_ = 0;
  std::vector<ClassDecl *> classes_;
  llvm::SmallPtrSet<const CallExpr *, 4> rootSupers_;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> returnSources_;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> inheritsReturn_;
  std::vector<const FuncSymbol *> inferring_;
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended_;
  llvm::DenseMap<const FunctionDecl *, ByteCallback> byteCallbacks_;
  llvm::DenseMap<const FunctionDecl *, unsigned> indexCallbacks_;
  llvm::DenseMap<const FunctionDecl *, std::tuple<unsigned, int64_t, int64_t>> elementCallbacks_;
  ClassDecl *statsClass_ = nullptr;
  ClassDecl *direntClass_ = nullptr;
  llvm::DenseMap<const VarSymbol *, std::string> constPrefixes_;

public:
  ~Checker();

  Scopes &scopes() const { return *scopesPart_; }
  TypeResolver &typeResolver() const { return *typeResolverPart_; }
  TypeRelations &typeRelations() const { return *typeRelationsPart_; }
  CallCycles &callCycles() const { return *callCyclesPart_; }
  FunctionChecker &functionChecker() const { return *functionCheckerPart_; }
  ReturnChecker &returnChecker() const { return *returnCheckerPart_; }
  StmtChecker &stmtChecker() const { return *stmtCheckerPart_; }
  JumpChecker &jumpChecker() const { return *jumpCheckerPart_; }
  LoopChecker &loopChecker() const { return *loopCheckerPart_; }
  SwitchChecker &switchChecker() const { return *switchCheckerPart_; }
  VarChecker &varChecker() const { return *varCheckerPart_; }
  ExprChecker &exprChecker() const { return *exprCheckerPart_; }
  NameChecker &nameChecker() const { return *nameCheckerPart_; }
  LiteralChecker &literalChecker() const { return *literalCheckerPart_; }
  UnaryChecker &unaryChecker() const { return *unaryCheckerPart_; }
  BinaryChecker &binaryChecker() const { return *binaryCheckerPart_; }
  ConcatChecker &concatChecker() const { return *concatCheckerPart_; }
  AssignChecker &assignChecker() const { return *assignCheckerPart_; }
  TargetChecker &targetChecker() const { return *targetCheckerPart_; }
  CallChecker &callChecker() const { return *callCheckerPart_; }
  ArgumentChecker &argumentChecker() const { return *argumentCheckerPart_; }
  ConsoleChecker &consoleChecker() const { return *consoleCheckerPart_; }
  MemberChecker &memberChecker() const { return *memberCheckerPart_; }
  ArgListChecker &argListChecker() const { return *argListCheckerPart_; }
  AsyncChecker &asyncChecker() const { return *asyncCheckerPart_; }
  PromiseChecker &promiseChecker() const { return *promiseCheckerPart_; }
  PromiseStaticChecker &promiseStaticChecker() const { return *promiseStaticCheckerPart_; }
  TimerChecker &timerChecker() const { return *timerCheckerPart_; }
  MathChecker &mathChecker() const { return *mathCheckerPart_; }
  GlobalFunctionChecker &globalFunctionChecker() const { return *globalFunctionCheckerPart_; }
  NumberMethods &numberMethods() const { return *numberMethodsPart_; }
  StringMethods &stringMethods() const { return *stringMethodsPart_; }
  ArrayMethods &arrayMethods() const { return *arrayMethodsPart_; }
  NewChecker &newChecker() const { return *newCheckerPart_; }
  ArrayLiteralChecker &arrayLiteralChecker() const { return *arrayLiteralCheckerPart_; }
  ClassDeclarer &classDeclarer() const { return *classDeclarerPart_; }
  ClassChecker &classChecker() const { return *classCheckerPart_; }
  ClassMemberChecker &classMemberChecker() const { return *classMemberCheckerPart_; }
  StaticChecker &staticChecker() const { return *staticCheckerPart_; }
  MethodCallChecker &methodCallChecker() const { return *methodCallCheckerPart_; }
  InstanceOfChecker &instanceOfChecker() const { return *instanceOfCheckerPart_; }
  OverloadResolver &overloadResolver() const { return *overloadResolverPart_; }
  MethodLiteralChecker &methodLiteralChecker() const { return *methodLiteralCheckerPart_; }
  ClassValueChecker &classValueChecker() const { return *classValueCheckerPart_; }
  LateClasses &lateClasses() const { return *lateClassesPart_; }
  ClosureChecker &closureChecker() const { return *closureCheckerPart_; }
  NestedFunctions &nestedFunctions() const { return *nestedFunctionsPart_; }
  FunctionValueChecker &functionValueChecker() const { return *functionValueCheckerPart_; }
  CallbackChecker &callbackChecker() const { return *callbackCheckerPart_; }
  ConditionalTypes &conditionalTypes() const { return *conditionalTypesPart_; }
  ExtendsRelation &extendsRelation() const { return *extendsRelationPart_; }
  TemplateTypes &templateTypes() const { return *templateTypesPart_; }
  TypeDeclarer &typeDeclarer() const { return *typeDeclarerPart_; }
  ConstantFolder &constantFolder() const { return *constantFolderPart_; }
  FunctionMethodChecker &functionMethodChecker() const { return *functionMethodCheckerPart_; }
  DecoratorChecker &decoratorChecker() const { return *decoratorCheckerPart_; }
  MetadataChecker &metadataChecker() const { return *metadataCheckerPart_; }
  Destination &destination() const { return *destinationPart_; }
  DestinationScanner &destinationScanner() const { return *destinationScannerPart_; }
  DictChecker &dictChecker() const { return *dictCheckerPart_; }
  DictMethods &dictMethods() const { return *dictMethodsPart_; }
  DictNewChecker &dictNewChecker() const { return *dictNewCheckerPart_; }
  RecordLiteralChecker &recordLiteralChecker() const { return *recordLiteralCheckerPart_; }
  FlowChecker &flowChecker() const { return *flowCheckerPart_; }
  IndexProofs &indexProofs() const { return *indexProofsPart_; }
  ConditionFacts &conditionFacts() const { return *conditionFactsPart_; }
  LoopScanner &loopScanner() const { return *loopScannerPart_; }
  LoopBounds &loopBounds() const { return *loopBoundsPart_; }
  TypeParams &typeParams() const { return *typeParamsPart_; }
  GenericChecker &genericChecker() const { return *genericCheckerPart_; }
  TypeArgInferrer &typeArgInferrer() const { return *typeArgInferrerPart_; }
  GenericInstantiator &genericInstantiator() const { return *genericInstantiatorPart_; }
  AliasInstantiator &aliasInstantiator() const { return *aliasInstantiatorPart_; }
  GenericClassChecker &genericClassChecker() const { return *genericClassCheckerPart_; }
  GenericClassInstantiator &genericClassInstantiator() const {
    return *genericClassInstantiatorPart_;
  }
  ReturnInferrer &returnInferrer() const { return *returnInferrerPart_; }
  ClassBases &classBases() const { return *classBasesPart_; }
  ClassLinks &classLinks() const { return *classLinksPart_; }
  InheritChecker &inheritChecker() const { return *inheritCheckerPart_; }
  ConstructorCalls &constructorCalls() const { return *constructorCallsPart_; }
  GeneratorChecker &generatorChecker() const { return *generatorCheckerPart_; }
  IteratorMethods &iteratorMethods() const { return *iteratorMethodsPart_; }
  IterableChecker &iterableChecker() const { return *iterableCheckerPart_; }
  ModuleDeclarer &moduleDeclarer() const { return *moduleDeclarerPart_; }
  ImportBinder &importBinder() const { return *importBinderPart_; }
  TopLevelChecker &topLevelChecker() const { return *topLevelCheckerPart_; }
  LoadOrder &loadOrder() const { return *loadOrderPart_; }
  Unused &unused() const { return *unusedPart_; }
  ObjectLiteralChecker &objectLiteralChecker() const { return *objectLiteralCheckerPart_; }
  KeyedFieldChecker &keyedFieldChecker() const { return *keyedFieldCheckerPart_; }
  TupleChecker &tupleChecker() const { return *tupleCheckerPart_; }
  ObjectChecker &objectChecker() const { return *objectCheckerPart_; }
  FieldFacts &fieldFacts() const { return *fieldFactsPart_; }
  PredicateChecker &predicateChecker() const { return *predicateCheckerPart_; }
  TruthChecker &truthChecker() const { return *truthCheckerPart_; }
  TypeOperators &typeOperators() const { return *typeOperatorsPart_; }
  UtilityTypes &utilityTypes() const { return *utilityTypesPart_; }
  Prelude &prelude() const { return *preludePart_; }
  LiteralTypes &literalTypes() const { return *literalTypesPart_; }
  UnionNarrowing &unionNarrowing() const { return *unionNarrowingPart_; }
  UnionChecker &unionChecker() const { return *unionCheckerPart_; }
  UnknownChecker &unknownChecker() const { return *unknownCheckerPart_; }
  WideWrites &wideWrites() const { return *wideWritesPart_; }
  ByteProofs &byteProofs() const { return *byteProofsPart_; }
  BytesChecker &bytesChecker() const { return *bytesCheckerPart_; }
  BytesNewChecker &bytesNewChecker() const { return *bytesNewCheckerPart_; }
  TypedArrayChecker &typedArrayChecker() const { return *typedArrayCheckerPart_; }
  BufferChecker &bufferChecker() const { return *bufferCheckerPart_; }
  BufferStaticChecker &bufferStaticChecker() const { return *bufferStaticCheckerPart_; }
  ChildChecker &childChecker() const { return *childCheckerPart_; }
  CryptoChecker &cryptoChecker() const { return *cryptoCheckerPart_; }
  CryptoArgs &cryptoArgs() const { return *cryptoArgsPart_; }
  CryptoFunctionChecker &cryptoFunctionChecker() const { return *cryptoFunctionCheckerPart_; }
  KeyPairChecker &keyPairChecker() const { return *keyPairCheckerPart_; }
  CryptoMethodChecker &cryptoMethodChecker() const { return *cryptoMethodCheckerPart_; }
  WebCryptoChecker &webCryptoChecker() const { return *webCryptoCheckerPart_; }
  SubtleCallChecker &subtleCallChecker() const { return *subtleCallCheckerPart_; }
  DateChecker &dateChecker() const { return *dateCheckerPart_; }
  DateFormatChecker &dateFormatChecker() const { return *dateFormatCheckerPart_; }
  DecimalChecker &decimalChecker() const { return *decimalCheckerPart_; }
  LibraryCalls &libraryCalls() const { return *libraryCallsPart_; }
  EventMaps &eventMaps() const { return *eventMapsPart_; }
  EventMapInference &eventMapInference() const { return *eventMapInferencePart_; }
  EmitterChecker &emitterChecker() const { return *emitterCheckerPart_; }
  EventTargetChecker &eventTargetChecker() const { return *eventTargetCheckerPart_; }
  IntlChecker &intlChecker() const { return *intlCheckerPart_; }
  IntlOptionsChecker &intlOptionsChecker() const { return *intlOptionsCheckerPart_; }
  IntlMethodChecker &intlMethodChecker() const { return *intlMethodCheckerPart_; }
  JsonChecker &jsonChecker() const { return *jsonCheckerPart_; }
  JsonTypeChecker &jsonTypeChecker() const { return *jsonTypeCheckerPart_; }
  JsonStaticChecker &jsonStaticChecker() const { return *jsonStaticCheckerPart_; }
  BuiltinModuleChecker &builtinModuleChecker() const { return *builtinModuleCheckerPart_; }
  FsChecker &fsChecker() const { return *fsCheckerPart_; }
  FsDeclarer &fsDeclarer() const { return *fsDeclarerPart_; }
  PathChecker &pathChecker() const { return *pathCheckerPart_; }
  OsChecker &osChecker() const { return *osCheckerPart_; }
  RuntimeObjectChecker &runtimeObjectChecker() const { return *runtimeObjectCheckerPart_; }
  FsPromisesChecker &fsPromisesChecker() const { return *fsPromisesCheckerPart_; }
  ReadlineChecker &readlineChecker() const { return *readlineCheckerPart_; }
  ProcessChecker &processChecker() const { return *processCheckerPart_; }
  ReflectChecker &reflectChecker() const { return *reflectCheckerPart_; }
  ReflectCallChecker &reflectCallChecker() const { return *reflectCallCheckerPart_; }
  RegexChecker &regexChecker() const { return *regexCheckerPart_; }
  StringRegexChecker &stringRegexChecker() const { return *stringRegexCheckerPart_; }
  MatchChecker &matchChecker() const { return *matchCheckerPart_; }
  ResultTypes &resultTypes() const { return *resultTypesPart_; }
  ResultChecker &resultChecker() const { return *resultCheckerPart_; }
  ResultMethodChecker &resultMethodChecker() const { return *resultMethodCheckerPart_; }
  ChunkTypes &chunkTypes() const { return *chunkTypesPart_; }
  StreamNewChecker &streamNewChecker() const { return *streamNewCheckerPart_; }
  StreamChecker &streamChecker() const { return *streamCheckerPart_; }
  SetEncodingChecker &setEncodingChecker() const { return *setEncodingCheckerPart_; }
  StreamSources &streamSources() const { return *streamSourcesPart_; }
  StreamPipeline &streamPipeline() const { return *streamPipelinePart_; }
  TzModulesChecker &tzModulesChecker() const { return *tzModulesCheckerPart_; }
  UrlChecker &urlChecker() const { return *urlCheckerPart_; }
  HttpChecker &httpChecker() const { return *httpCheckerPart_; }
  WeakRefChecker &weakRefChecker() const { return *weakRefCheckerPart_; }

private:
  friend class DeclarePass;
  friend class InferPass;
  friend class CheckPass;
  friend class InstancesPass;
  friend class WholeProgramPass;
  friend class Scopes;
  friend class TypeResolver;
  friend class TypeRelations;
  friend class CallCycles;
  friend class FunctionChecker;
  friend class ReturnChecker;
  friend class StmtChecker;
  friend class JumpChecker;
  friend class LoopChecker;
  friend class SwitchChecker;
  friend class VarChecker;
  friend class ExprChecker;
  friend class NameChecker;
  friend class LiteralChecker;
  friend class UnaryChecker;
  friend class BinaryChecker;
  friend class ConcatChecker;
  friend class AssignChecker;
  friend class TargetChecker;
  friend class CallChecker;
  friend class ArgumentChecker;
  friend class ConsoleChecker;
  friend class MemberChecker;
  friend class ArgListChecker;
  friend class AsyncChecker;
  friend class PromiseChecker;
  friend class PromiseStaticChecker;
  friend class TimerChecker;
  friend class MathChecker;
  friend class GlobalFunctionChecker;
  friend class NumberMethods;
  friend class StringMethods;
  friend class ArrayMethods;
  friend class NewChecker;
  friend class ArrayLiteralChecker;
  friend class ClassDeclarer;
  friend class ClassChecker;
  friend class ClassMemberChecker;
  friend class StaticChecker;
  friend class MethodCallChecker;
  friend class InstanceOfChecker;
  friend class OverloadResolver;
  friend class MethodLiteralChecker;
  friend class ClassValueChecker;
  friend class LateClasses;
  friend class ClosureChecker;
  friend class NestedFunctions;
  friend class FunctionValueChecker;
  friend class CallbackChecker;
  friend class ConditionalTypes;
  friend class ExtendsRelation;
  friend class TemplateTypes;
  friend class TypeDeclarer;
  friend class ConstantFolder;
  friend class FunctionMethodChecker;
  friend class DecoratorChecker;
  friend class MetadataChecker;
  friend class Destination;
  friend class DestinationScanner;
  friend class DictChecker;
  friend class DictMethods;
  friend class DictNewChecker;
  friend class RecordLiteralChecker;
  friend class FlowChecker;
  friend class IndexProofs;
  friend class ConditionFacts;
  friend class LoopScanner;
  friend class LoopBounds;
  friend class TypeParams;
  friend class GenericChecker;
  friend class TypeArgInferrer;
  friend class GenericInstantiator;
  friend class AliasInstantiator;
  friend class GenericClassChecker;
  friend class GenericClassInstantiator;
  friend class ReturnInferrer;
  friend class ClassBases;
  friend class ClassLinks;
  friend class InheritChecker;
  friend class ConstructorCalls;
  friend class GeneratorChecker;
  friend class IteratorMethods;
  friend class IterableChecker;
  friend class ModuleDeclarer;
  friend class ImportBinder;
  friend class TopLevelChecker;
  friend class LoadOrder;
  friend class Unused;
  friend class ObjectLiteralChecker;
  friend class KeyedFieldChecker;
  friend class TupleChecker;
  friend class ObjectChecker;
  friend class FieldFacts;
  friend class PredicateChecker;
  friend class TruthChecker;
  friend class TypeOperators;
  friend class UtilityTypes;
  friend class Prelude;
  friend class LiteralTypes;
  friend class UnionNarrowing;
  friend class UnionChecker;
  friend class UnknownChecker;
  friend class WideWrites;
  friend class ByteProofs;
  friend class BytesChecker;
  friend class BytesNewChecker;
  friend class TypedArrayChecker;
  friend class BufferChecker;
  friend class BufferStaticChecker;
  friend class ChildChecker;
  friend class CryptoChecker;
  friend class CryptoArgs;
  friend class CryptoFunctionChecker;
  friend class KeyPairChecker;
  friend class CryptoMethodChecker;
  friend class WebCryptoChecker;
  friend class SubtleCallChecker;
  friend class DateChecker;
  friend class DateFormatChecker;
  friend class DecimalChecker;
  friend class LibraryCalls;
  friend class EventMaps;
  friend class EventMapInference;
  friend class EmitterChecker;
  friend class EventTargetChecker;
  friend class IntlChecker;
  friend class IntlOptionsChecker;
  friend class IntlMethodChecker;
  friend class JsonChecker;
  friend class JsonTypeChecker;
  friend class JsonStaticChecker;
  friend class BuiltinModuleChecker;
  friend class FsChecker;
  friend class FsDeclarer;
  friend class PathChecker;
  friend class OsChecker;
  friend class RuntimeObjectChecker;
  friend class FsPromisesChecker;
  friend class ReadlineChecker;
  friend class ProcessChecker;
  friend class ReflectChecker;
  friend class ReflectCallChecker;
  friend class RegexChecker;
  friend class StringRegexChecker;
  friend class MatchChecker;
  friend class ResultTypes;
  friend class ResultChecker;
  friend class ResultMethodChecker;
  friend class ChunkTypes;
  friend class StreamNewChecker;
  friend class StreamChecker;
  friend class SetEncodingChecker;
  friend class StreamSources;
  friend class StreamPipeline;
  friend class TzModulesChecker;
  friend class UrlChecker;
  friend class HttpChecker;
  friend class WeakRefChecker;
  std::unique_ptr<Scopes> scopesPart_;
  std::unique_ptr<TypeResolver> typeResolverPart_;
  std::unique_ptr<TypeRelations> typeRelationsPart_;
  std::unique_ptr<CallCycles> callCyclesPart_;
  std::unique_ptr<FunctionChecker> functionCheckerPart_;
  std::unique_ptr<ReturnChecker> returnCheckerPart_;
  std::unique_ptr<StmtChecker> stmtCheckerPart_;
  std::unique_ptr<JumpChecker> jumpCheckerPart_;
  std::unique_ptr<LoopChecker> loopCheckerPart_;
  std::unique_ptr<SwitchChecker> switchCheckerPart_;
  std::unique_ptr<VarChecker> varCheckerPart_;
  std::unique_ptr<ExprChecker> exprCheckerPart_;
  std::unique_ptr<NameChecker> nameCheckerPart_;
  std::unique_ptr<LiteralChecker> literalCheckerPart_;
  std::unique_ptr<UnaryChecker> unaryCheckerPart_;
  std::unique_ptr<BinaryChecker> binaryCheckerPart_;
  std::unique_ptr<ConcatChecker> concatCheckerPart_;
  std::unique_ptr<AssignChecker> assignCheckerPart_;
  std::unique_ptr<TargetChecker> targetCheckerPart_;
  std::unique_ptr<CallChecker> callCheckerPart_;
  std::unique_ptr<ArgumentChecker> argumentCheckerPart_;
  std::unique_ptr<ConsoleChecker> consoleCheckerPart_;
  std::unique_ptr<MemberChecker> memberCheckerPart_;
  std::unique_ptr<ArgListChecker> argListCheckerPart_;
  std::unique_ptr<AsyncChecker> asyncCheckerPart_;
  std::unique_ptr<PromiseChecker> promiseCheckerPart_;
  std::unique_ptr<PromiseStaticChecker> promiseStaticCheckerPart_;
  std::unique_ptr<TimerChecker> timerCheckerPart_;
  std::unique_ptr<MathChecker> mathCheckerPart_;
  std::unique_ptr<GlobalFunctionChecker> globalFunctionCheckerPart_;
  std::unique_ptr<NumberMethods> numberMethodsPart_;
  std::unique_ptr<StringMethods> stringMethodsPart_;
  std::unique_ptr<ArrayMethods> arrayMethodsPart_;
  std::unique_ptr<NewChecker> newCheckerPart_;
  std::unique_ptr<ArrayLiteralChecker> arrayLiteralCheckerPart_;
  std::unique_ptr<ClassDeclarer> classDeclarerPart_;
  std::unique_ptr<ClassChecker> classCheckerPart_;
  std::unique_ptr<ClassMemberChecker> classMemberCheckerPart_;
  std::unique_ptr<StaticChecker> staticCheckerPart_;
  std::unique_ptr<MethodCallChecker> methodCallCheckerPart_;
  std::unique_ptr<InstanceOfChecker> instanceOfCheckerPart_;
  std::unique_ptr<OverloadResolver> overloadResolverPart_;
  std::unique_ptr<MethodLiteralChecker> methodLiteralCheckerPart_;
  std::unique_ptr<ClassValueChecker> classValueCheckerPart_;
  std::unique_ptr<LateClasses> lateClassesPart_;
  std::unique_ptr<ClosureChecker> closureCheckerPart_;
  std::unique_ptr<NestedFunctions> nestedFunctionsPart_;
  std::unique_ptr<FunctionValueChecker> functionValueCheckerPart_;
  std::unique_ptr<CallbackChecker> callbackCheckerPart_;
  std::unique_ptr<ConditionalTypes> conditionalTypesPart_;
  std::unique_ptr<ExtendsRelation> extendsRelationPart_;
  std::unique_ptr<TemplateTypes> templateTypesPart_;
  std::unique_ptr<TypeDeclarer> typeDeclarerPart_;
  std::unique_ptr<ConstantFolder> constantFolderPart_;
  std::unique_ptr<FunctionMethodChecker> functionMethodCheckerPart_;
  std::unique_ptr<DecoratorChecker> decoratorCheckerPart_;
  std::unique_ptr<MetadataChecker> metadataCheckerPart_;
  std::unique_ptr<Destination> destinationPart_;
  std::unique_ptr<DestinationScanner> destinationScannerPart_;
  std::unique_ptr<DictChecker> dictCheckerPart_;
  std::unique_ptr<DictMethods> dictMethodsPart_;
  std::unique_ptr<DictNewChecker> dictNewCheckerPart_;
  std::unique_ptr<RecordLiteralChecker> recordLiteralCheckerPart_;
  std::unique_ptr<FlowChecker> flowCheckerPart_;
  std::unique_ptr<IndexProofs> indexProofsPart_;
  std::unique_ptr<ConditionFacts> conditionFactsPart_;
  std::unique_ptr<LoopScanner> loopScannerPart_;
  std::unique_ptr<LoopBounds> loopBoundsPart_;
  std::unique_ptr<TypeParams> typeParamsPart_;
  std::unique_ptr<GenericChecker> genericCheckerPart_;
  std::unique_ptr<TypeArgInferrer> typeArgInferrerPart_;
  std::unique_ptr<GenericInstantiator> genericInstantiatorPart_;
  std::unique_ptr<AliasInstantiator> aliasInstantiatorPart_;
  std::unique_ptr<GenericClassChecker> genericClassCheckerPart_;
  std::unique_ptr<GenericClassInstantiator> genericClassInstantiatorPart_;
  std::unique_ptr<ReturnInferrer> returnInferrerPart_;
  std::unique_ptr<ClassBases> classBasesPart_;
  std::unique_ptr<ClassLinks> classLinksPart_;
  std::unique_ptr<InheritChecker> inheritCheckerPart_;
  std::unique_ptr<ConstructorCalls> constructorCallsPart_;
  std::unique_ptr<GeneratorChecker> generatorCheckerPart_;
  std::unique_ptr<IteratorMethods> iteratorMethodsPart_;
  std::unique_ptr<IterableChecker> iterableCheckerPart_;
  std::unique_ptr<ModuleDeclarer> moduleDeclarerPart_;
  std::unique_ptr<ImportBinder> importBinderPart_;
  std::unique_ptr<TopLevelChecker> topLevelCheckerPart_;
  std::unique_ptr<LoadOrder> loadOrderPart_;
  std::unique_ptr<Unused> unusedPart_;
  std::unique_ptr<ObjectLiteralChecker> objectLiteralCheckerPart_;
  std::unique_ptr<KeyedFieldChecker> keyedFieldCheckerPart_;
  std::unique_ptr<TupleChecker> tupleCheckerPart_;
  std::unique_ptr<ObjectChecker> objectCheckerPart_;
  std::unique_ptr<FieldFacts> fieldFactsPart_;
  std::unique_ptr<PredicateChecker> predicateCheckerPart_;
  std::unique_ptr<TruthChecker> truthCheckerPart_;
  std::unique_ptr<TypeOperators> typeOperatorsPart_;
  std::unique_ptr<UtilityTypes> utilityTypesPart_;
  std::unique_ptr<Prelude> preludePart_;
  std::unique_ptr<LiteralTypes> literalTypesPart_;
  std::unique_ptr<UnionNarrowing> unionNarrowingPart_;
  std::unique_ptr<UnionChecker> unionCheckerPart_;
  std::unique_ptr<UnknownChecker> unknownCheckerPart_;
  std::unique_ptr<WideWrites> wideWritesPart_;
  std::unique_ptr<ByteProofs> byteProofsPart_;
  std::unique_ptr<BytesChecker> bytesCheckerPart_;
  std::unique_ptr<BytesNewChecker> bytesNewCheckerPart_;
  std::unique_ptr<TypedArrayChecker> typedArrayCheckerPart_;
  std::unique_ptr<BufferChecker> bufferCheckerPart_;
  std::unique_ptr<BufferStaticChecker> bufferStaticCheckerPart_;
  std::unique_ptr<ChildChecker> childCheckerPart_;
  std::unique_ptr<CryptoChecker> cryptoCheckerPart_;
  std::unique_ptr<CryptoArgs> cryptoArgsPart_;
  std::unique_ptr<CryptoFunctionChecker> cryptoFunctionCheckerPart_;
  std::unique_ptr<KeyPairChecker> keyPairCheckerPart_;
  std::unique_ptr<CryptoMethodChecker> cryptoMethodCheckerPart_;
  std::unique_ptr<WebCryptoChecker> webCryptoCheckerPart_;
  std::unique_ptr<SubtleCallChecker> subtleCallCheckerPart_;
  std::unique_ptr<DateChecker> dateCheckerPart_;
  std::unique_ptr<DateFormatChecker> dateFormatCheckerPart_;
  std::unique_ptr<DecimalChecker> decimalCheckerPart_;
  std::unique_ptr<LibraryCalls> libraryCallsPart_;
  std::unique_ptr<EventMaps> eventMapsPart_;
  std::unique_ptr<EventMapInference> eventMapInferencePart_;
  std::unique_ptr<EmitterChecker> emitterCheckerPart_;
  std::unique_ptr<EventTargetChecker> eventTargetCheckerPart_;
  std::unique_ptr<IntlChecker> intlCheckerPart_;
  std::unique_ptr<IntlOptionsChecker> intlOptionsCheckerPart_;
  std::unique_ptr<IntlMethodChecker> intlMethodCheckerPart_;
  std::unique_ptr<JsonChecker> jsonCheckerPart_;
  std::unique_ptr<JsonTypeChecker> jsonTypeCheckerPart_;
  std::unique_ptr<JsonStaticChecker> jsonStaticCheckerPart_;
  std::unique_ptr<BuiltinModuleChecker> builtinModuleCheckerPart_;
  std::unique_ptr<FsChecker> fsCheckerPart_;
  std::unique_ptr<FsDeclarer> fsDeclarerPart_;
  std::unique_ptr<PathChecker> pathCheckerPart_;
  std::unique_ptr<OsChecker> osCheckerPart_;
  std::unique_ptr<RuntimeObjectChecker> runtimeObjectCheckerPart_;
  std::unique_ptr<FsPromisesChecker> fsPromisesCheckerPart_;
  std::unique_ptr<ReadlineChecker> readlineCheckerPart_;
  std::unique_ptr<ProcessChecker> processCheckerPart_;
  std::unique_ptr<ReflectChecker> reflectCheckerPart_;
  std::unique_ptr<ReflectCallChecker> reflectCallCheckerPart_;
  std::unique_ptr<RegexChecker> regexCheckerPart_;
  std::unique_ptr<StringRegexChecker> stringRegexCheckerPart_;
  std::unique_ptr<MatchChecker> matchCheckerPart_;
  std::unique_ptr<ResultTypes> resultTypesPart_;
  std::unique_ptr<ResultChecker> resultCheckerPart_;
  std::unique_ptr<ResultMethodChecker> resultMethodCheckerPart_;
  std::unique_ptr<ChunkTypes> chunkTypesPart_;
  std::unique_ptr<StreamNewChecker> streamNewCheckerPart_;
  std::unique_ptr<StreamChecker> streamCheckerPart_;
  std::unique_ptr<SetEncodingChecker> setEncodingCheckerPart_;
  std::unique_ptr<StreamSources> streamSourcesPart_;
  std::unique_ptr<StreamPipeline> streamPipelinePart_;
  std::unique_ptr<TzModulesChecker> tzModulesCheckerPart_;
  std::unique_ptr<UrlChecker> urlCheckerPart_;
  std::unique_ptr<HttpChecker> httpCheckerPart_;
  std::unique_ptr<WeakRefChecker> weakRefCheckerPart_;
};

bool spreadsLists(const Type *type);

bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params);

const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic);

std::string argumentCount(unsigned min, unsigned max);

bool isLanguageTag(const std::string &tag);

const char *whyUnsupported(llvm::StringRef name);

inline std::string quotedName(llvm::StringRef name) {
  return name.empty() ? "the function" : "'" + name.str() + "'";
}
inline std::string functionLabel(llvm::StringRef name) {
  return name.empty() ? "the function" : "function '" + name.str() + "'";
}

std::string variantText(const FunctionDecl *fn);
std::string variantList(llvm::ArrayRef<FunctionDecl *> variants);

std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i);

std::string fieldText(const Expr *object, llvm::StringRef field);

void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction = true);
void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction = true);
void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names);

void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions = true);
void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions = true);

}
