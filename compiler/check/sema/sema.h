#pragma once

#include <memory>
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
#include <optional>

namespace tz {

// The modules built into tz that a program imports names from
// (sema_node.cpp): node:fs, node:path (and its posix and win32 styles),
// node:os, node:process, node:buffer, node:child_process, node:crypto and tz:io.
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

// What is wrong with importing `name` from node:crypto, written `path`, or nothing
// (sema_crypto.cpp).
std::string cryptoExportProblem(llvm::StringRef name, llvm::StringRef path);

// The name a file that says `crypto.subtle` imports the SubtleCrypto of tz:webcrypto under
// (compiler/load/loader/loader.cpp), which Sema reads it by (sema_webcrypto.cpp).
inline constexpr llvm::StringLiteral kSubtleName = "__tz_subtle";

// Why the built-in module `path` (see isBuiltinModule) cannot be imported:
// it is not there yet, or there is none; empty for the modules tz has. The
// loader reports it.
std::string builtinModuleProblem(llvm::StringRef path);

// The values an integer expression may have, as far as Sema knows: the
// bounds it found (see Sema::valueRange).
struct ValueRange {
  std::optional<int64_t> lo, hi;
};

// The event maps of the emitters whose types do not give them, `new
// EventEmitter()` and `class Server extends EventEmitter`, found from the
// emit() calls of the whole program (docs/events.md, section 2). EventRounds
// checks such a program in rounds, each with a syntax tree of its own: a
// round records what the emits it checks pass, and gives the listeners of
// the events known from the rounds before their types; once a round finds
// nothing new, a last one checks the program with the maps as they are,
// for real (see Sema::inferredEventMap).
struct EventInference {
  // A type as the next round makes it again: a class (and so a type that
  // has one in it) by its file and name, as each round has declarations
  // of its own; any other type as it is, the same in every round.
  struct Portable {
    enum class Kind {
      Same,
      Class,    // name in file, of the type arguments `parts`
      ClassRef, // the class itself as a value
      Optional, // parts[0] | undefined
      Array,    // parts[0][]
      Tuple,    // the elements, `min` of them needed, a rest last if `rest`
      Function, // the parameters then the result
      Union,
      Object, // the fields, named by `names`
      Promise,
      Dict,     // `dict` (Type::DictKind), the key and the value
      Iterator, // yielded, returned, sent
      Result,   // ok, err
      Bad,      // what the next round cannot make: a literal with methods
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
    Portable args;     // a tuple
    std::string where; // of the first emit: "main.tz:12:5"
  };
  // The events found so far, in the order found, by the place of the
  // emitter: `new EventEmitter()`, or the class that extends EventEmitter.
  std::map<std::string, std::vector<Event>> maps;
  bool open = false;    // set by Sema: some emitter's map comes from here
  bool changed = false; // set by Sema: the round found more
  bool final = false;   // set by EventRounds: the maps are complete
};

// Resolves names and checks types. Annotates the AST in place: every
// expression gets a type, names get their symbols, and implicit conversions
// become explicit CastExpr nodes, so code generation needs no type rules.
//
// Numeric rules: no implicit conversion loses information. int widens to long
// and double on its own, long to double. A double becomes an int through a
// bitwise operator (`x | 0`, JavaScript's ToInt32) or through Math.round and
// the like where an integer is expected. There is no `as`. Integer literals
// take the type the context expects (`let x: double = 1 / 2` divides floats).
//
// Sema also follows the flow of the program (see Flow): it narrows
// `T | undefined` after checks such as `x !== undefined`, and it proves
// indices in range. A read `a[i]` it can prove has type `T` and needs no check
// at run time; any other read has type `T | undefined`, as in JavaScript.
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
class ResultAsyncChecker;
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
class ThrowChecker;
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
class SafeTryChecker;
class ResultMethodChecker;
class ChunkTypes;
class StreamNewChecker;
class StreamChecker;
class SetEncodingChecker;
class StreamSources;
class StreamPipeline;
class TzModulesChecker;
class WeakRefChecker;

class Sema {
public:
  Sema(AstContext &ctx, Diagnostics &diags);
  // What `await` of a value of `type` gives (see awaitedType), for CodeGen.
  static const Type *awaitedTypeOf(const Type *type);
  // Where the event maps found from emit() calls are kept between rounds
  // (see EventInference); without it they are errors.
  void setEventInference(EventInference *inference) { inference_ = inference; }
private:
  struct ScopeEntry {
    VarSymbol *var = nullptr;
    FuncSymbol *func = nullptr;
    EnumDecl *enumDecl = nullptr;
    ClassDecl *cls = nullptr;
  };
  // A field reached from a variable through fields: `q.limit`, `a.b.c`.
  struct FieldPath {
    const VarSymbol *var = nullptr;
    llvm::SmallVector<llvm::StringRef, 2> names;
    bool operator==(const FieldPath &other) const {
      return var == other.var && names == other.names;
    }
  };
  // What is known at the current point of the program: facts about ints and
  // lengths, and the `T | undefined` variables and fields that hold a value.
  struct Flow {
    Facts facts;
    llvm::SmallPtrSet<const VarSymbol *, 4> defined;
    std::vector<FieldPath> definedFields;
    // Fields of a union narrowed by checks: `o.shape` is a Circle inside
    // `if (o.shape.kind == "circle")`. Forgotten as definedFields are.
    std::vector<std::pair<FieldPath, const Type *>> narrowedFields;
    // Variables of a type parameter whose type a check showed: `x` of type
    // T is a string inside `if (typeof x == "string")`.
    std::vector<std::pair<const VarSymbol *, const Type *>> narrowed;
    // Result variables known to hold an Ok, or an Err (after `r.isOk()`).
    llvm::SmallPtrSet<const VarSymbol *, 2> okResults, errResults;
    // In a constructor, or the initializers of the fields: the fields of
    // `this` set so far (see sema_class.cpp). In the constructor of a class
    // that extends another, `this` does not exist before `super(...)`.
    std::vector<llvm::StringRef> setFields;
    bool beforeSuper = false;
    bool unreachable = false; // after return, break or continue
  };
  // What a loop may change, found by scanning it before checking it (see
  // sema_flow.cpp).
  struct LoopScan {
    struct Change {
      unsigned count = 0;  // assignments and updates of the variable
      int64_t delta = 0;   // the constant step of `x++`, `x += 2`, `x--`..., or 0
      bool nested = false; // some are in a nested loop
    };
    llvm::StringMap<Change> changes; // by variable name
    bool callsFunctions = false;
    bool growsArrays = false;
    bool shrinksArrays = false;
    // The fields assigned to, by name, of objects of any type.
    llvm::StringSet<> fieldWrites;
    // Some element of some array may be removed or replaced (pop, shift,
    // `a[i] = x`, fill...), and so released.
    bool releasesElements = false;
    bool breaks = false; // a `break` may leave the loop
    // While scanning: the switches around the current statement inside the
    // loop, and the labels declared inside it.
    unsigned switches = 0;
    llvm::SmallVector<llvm::StringRef, 2> innerLabels;
  };
  // A statement that `break` (and for loops `continue`) can leave.
  struct JumpTarget {
    enum class Kind { Loop, Switch, Block };
    Kind kind;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    // What holds where a `break` leaves a switch or a labeled block. Loops do
    // without: their head state holds at every `break` (see loopHead).
    Flow breaks;
  };
  // Scopes
  void pushScope() {
    scopes_.emplace_back();
    typeScopes_.emplace_back();
    scopeBranches_.push_back(branchDepth_);
  }
  void popScope();
  // How many statements that may run their parts or not (if, loops,
  // switch, try) the code being checked is in, and that count where each
  // scope began: a setEncoding() in a branch of the scope of its stream
  // leaves the type of the stream unknown after it (sema_stream.cpp).
  int branchDepth_ = 0;
  std::vector<int> scopeBranches_;
  // Generics (sema_generic.cpp). The body of a generic function is checked
  // once, with opaque type parameters (Type::Kind::Param) that allow what
  // TypeScript allows on any type: assigning, passing, returning, `==`,
  // `typeof`, printing, and turning into a string. A call infers the type
  // arguments and calls an instance: a copy of the function whose body is
  // checked again with those types and every rule of the language, so that
  // what our rules reject there (an object turned into a string, `<` on
  // objects) is reported at the call. Type arguments that look the same in
  // memory share one instance (see representation).
  struct Signature;
  struct Instance {
    FunctionDecl *decl = nullptr; // the copy, which CodeGen emits
    FuncSymbol *generic = nullptr;
    // An instance of a generic class instead (see instantiateClass): the
    // class made, whose bodies are checked as those of a copy of a function.
    ClassDecl *cls = nullptr;
    std::vector<const Type *> args;     // representations
    std::vector<const Type *> concrete; // as the call that made it gave them
    std::vector<const Type *> exact;    // see exactClassArgs
    llvm::SMLoc callLoc;
    const Instance *requester = nullptr; // the instance whose body needs it
    bool checking = false, checked = false, failed = false;
    // The first error in its body, where it is, and the instance that error
    // comes from (itself, or one that it calls).
    std::string message;
    llvm::SMLoc errorLoc;
  };
  // Whether two classes are one for access to private members: the same
  // class, or instances of one generic class (and the generic class).
  static bool sameClass(const ClassDecl *a, const ClassDecl *b) {
    return a && b && a->staticOwner() == b->staticOwner();
  }
  // The value of an enum member's initializer.
  struct Constant {
    bool isString = false;
    double number = 0;
    std::u16string text;
  };
  // The variables the blocks being checked declare, for messages about
  // using one before its declaration.
  struct BlockNames {
    size_t functions; // functions_.size() in the block
    std::vector<llvm::StringRef> names;
  };
  // What checking a function body may change, so that one body can be
  // checked in the middle of another.
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
  // Closures (sema_closure.cpp): arrows, function expressions and functions
  // declared inside blocks, the variables they capture, calls through
  // function values, and the array methods that take callbacks.
  //
  // What a function is expected to be where it is written: the arguments it
  // will get, and the result wanted (null: whatever the body returns). With
  // `pad`, it takes all of params even if it names fewer, so that its type is
  // the one expected; a callback of a built-in method takes just the
  // parameters it names.
  struct Signature {
    std::vector<const Type *> params;
    size_t minArgs = 0;
    const Type *ret = nullptr;
    bool voidResult = false; // what it returns is dropped (forEach)
    bool pad = true;
    // With voidResult: an async function may go here, and its Promise is
    // dropped (the callback of a timer); elsewhere that is an error.
    bool asyncVoid = false;
    // The function takes `this` first (Type::hasThis): params[0] is its
    // type. A function or method written without a `this` parameter gets
    // one, `{ read() { this.push(x) } }` where a `(this: Readable, size:
    // int) => void` goes; an arrow, whose `this` is that around it, a
    // hidden one it does not use.
    bool hasThis = false;
  };
  FunctionDecl *currentDecl() const { return functions_.empty() ? nullptr : functions_.back(); }
  // A function, or an array or optional of them.
  static bool holdsFunctions(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isFunction();
  }
  // The same for objects, dictionaries among them.
  static bool holdsObjects(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isObject() || type->isDict();
  }
  // Dates, which join() writes as toString() does.
  static bool holdsDates(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isDate();
  }
  // Truthiness (sema_truth.cpp). As in JavaScript, a condition may test a
  // string or a value that may be undefined, but not a number; `&&` and `||`
  // give one of their operands. `use` says how a tested value is used, for
  // the hints of the errors.
  enum class TruthUse { Condition, Not, Or, And, OrAssign, AndAssign };
  // Built-in modules (sema_node.cpp, docs/io.md): node:fs, node:path,
  // node:os and node:process, also without "node:", and tz:io. A name a file
  // imports from one stands for a function, a constant or a class of it, or
  // for a namespace: `posix` of node:path, or `fs` of `import fs from "fs"`
  // (an error, but its uses are checked as `readFileSync` would be).
  struct NodeName {
    NodeModule module;
    llvm::StringRef name; // empty for the namespace
  };
  // While checkFsCall checks a function of node:fs/promises: it takes the
  // name without Sync and its options, and no fd.
  bool fsAsync_ = false;
  // Literals whose type comes from where the object goes
  // (sema_destination.cpp). `const p = { x: 1, y: 2 }; len(p)` with
  // `len(p: Point)` checks the literal as if the declaration said
  // `const p: Point`, so x and y are doubles. The places are found by
  // scanning the code that follows the declaration in its block: arguments of
  // functions known by name, typed variables and fields, returns, push...
  struct DestinationScan;
  // A Result, or an array or optional of them.
  static bool holdsResults(const Type *type) {
    while (type->isOptional() || type->isArray()) {
      type = type->inner();
    }

    return type->isResult();
  }
  // Why a value of `type` cannot be built from a Json, or empty; a class
  // through `classProblem`, or not at all without it.
  using ClassProblem = llvm::function_ref<std::string(const ClassDecl *)>;
  // What a check narrows: a variable, or a field read through fields from
  // one, of a union (or a literal type, or boolean), declared as `declared`.
  struct NarrowTarget {
    const VarSymbol *var = nullptr;
    std::optional<FieldPath> path;
    const Type *declared = nullptr;
  };
  // Functions that test (sema_predicate.cpp, docs/narrowing.md): what holds
  // about the parameters of a function when it returns true, and when it
  // returns false, by their positions (-1 is `this`); the facts are about
  // the parameters' symbols, `params`.
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
    const FuncSymbol *fn; // null when the function returns no boolean
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
  // The callbacks of the methods of typed arrays, which are checked knowing
  // the range of the element they get, and, for map, the range their result
  // must fit in.
  struct ByteCallback {
    unsigned param = 0; // the element's
    int64_t lo = 0, hi = 0;
    const Type *target = nullptr; // map: the typed array the result goes to
  };
  // The Reflect.get calls of a method with several variants.
  llvm::SmallPtrSet<const CallExpr *, 4> severalVariants_;
  // The classes converted to `Constructor`, by the conversion: one that a
  // copy for a class known when compiling drops (see exactClassArgs) makes
  // no class reachable.
  llvm::DenseMap<const CastExpr *, ClassDecl *> constructorCasts_;
  // The class being declared, whose constructors may take
  // `...args: ConstructorArgs`.
  ClassDecl *declaringClass_ = nullptr;
  std::vector<ClassDecl *> lateClasses_;
  size_t lateChecked_ = 0;
  // The copies of templates whose bodies made late classes: an error in the
  // code of such a class is the copy's, reported where it is called.
  llvm::DenseMap<const ClassDecl *, Instance *> lateOwners_;
  std::map<Instance *, std::vector<Diagnostics::Captured>> lateErrors_;
  // The program uses reflection: `new` of a Constructor, Reflect.get...
  bool reflectionUsed_ = false;
  llvm::DenseMap<const Type *, std::pair<const Type *, const Type *>> descriptorOrigins_;
  // A decorator to apply: of the declaration itself, or (with `param` set) of
  // a parameter of `owner`, a method or a constructor.
  struct DecoratorUse {
    const Decorator *decorator;
    int param = -1;
    const FunctionDecl *owner = nullptr;
  };
  struct MetaKeyType {
    const Type *type;
    llvm::SMLoc loc; // the record that gave it
  };
  llvm::DenseMap<const void *, MetaKeyType> metaTypes_;
  ClassDecl *libraryClass(llvm::StringRef name) const { return libClasses_.lookup(name); }
  // A listener of the event `event` whose listeners get `args`: a function
  // of them that returns nothing, an async one, or one that takes `this`
  // (`kind`, as library/events/events.tz numbers them), converted to an
  // unknown, as the emitter keeps it. `receiver` is the emitter, the `this` it
  // gets.
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
  // The callback parameters of transform() and flush() written in the
  // options of an object-mode Transform whose chunk type a round looks for:
  // what they are called with is what it gives.
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
  // Constants initialized with a literal (an object, a string), which the checks of Web Crypto
  // read where the constant is an algorithm (sema_webcrypto.cpp).
  llvm::DenseMap<const VarSymbol *, const Expr *> constLiterals_;
  bool isDefined(const VarSymbol *var) const { return flow_.defined.contains(var); }
  // Modules (sema_modules.cpp). Each file of a program has a top-level scope
  // of its own, with its own declarations and what it imports. The scope of
  // the file being checked (module_) is scopes_[0] and typeScopes_[0]; those
  // of the others wait in their ModuleState.
  //
  // A top-level declaration of some file, which an import or an export names
  // in the end, through any re-exports.
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
    const ExportDecl *all = nullptr; // `export * from`, which gave it
  };
  struct Import {
    Binding target;
    llvm::SMLoc loc;
  };
  struct ModuleState {
    llvm::StringMap<ScopeEntry> values;
    llvm::StringMap<Stmt *> types;
    // The names the file declares at the top level, also those not checked
    // yet, and its `let` and `const`.
    llvm::StringMap<llvm::SMLoc> declared;
    llvm::StringSet<> topLevelNames;
    // Its imports, by the names they get in it; and those already reported
    // as wrong.
    llvm::StringMap<Import> imports;
    llvm::StringSet<> badImports;
    llvm::SmallPtrSet<const VarSymbol *, 4> importedVars;
    // What it imports from neverthrow: the names they get in it, and the
    // names of neverthrow they stand for.
    llvm::StringMap<llvm::StringRef> neverthrow;
    // What it imports from built-in modules, by the names they get in it.
    llvm::StringMap<NodeName> nodeNames;
    llvm::StringMap<Export> exports;
    bool exportsDone = false, exportsBusy = false;
    // Its top-level code: the next statement to check, and what holds
    // there when the check stopped early (see loadEarly).
    size_t next = 0;
    std::optional<Flow> flow;
    bool busy = false;   // being checked
    bool loaded = false; // all checked
  };
  ModuleState &state(const Module *module) { return moduleStates_[module->order]; }
  AstContext &ctx_;
  Diagnostics &diags_;
  std::vector<llvm::StringMap<ScopeEntry>> scopes_;
  std::vector<llvm::StringMap<Stmt *>> typeScopes_; // TypeAliasDecl or EnumDecl
  FuncSymbol *currentFunction_ = nullptr;           // null in top-level code
  std::vector<JumpTarget> targets_;
  llvm::SmallVector<llvm::StringRef, 1> pendingLabels_;
  // Variables declared by the earlier cases of the switches being checked:
  // the case being checked must not use them (see checkSwitch).
  llvm::SmallPtrSet<const VarSymbol *, 4> otherCaseVars_;
  Flow flow_;
  llvm::StringSet<> namesInFunctions_;
  // The names of the fields the program assigns to anywhere: only those can
  // change during a call.
  llvm::StringSet<> assignedFields_;
  // The statements of the block being checked, and the position in them of
  // the one being checked: where a literal it declares may go is in the
  // statements around it (see literalDestination and followingStatements).
  llvm::ArrayRef<Stmt *> block_;
  size_t blockIndex_ = 0;
  // Those statements, for literalDestination.
  std::vector<const Stmt *> following_;
  // Variables initialized by literals that go to several places whose types
  // disagree, with those places: for the error at one of them.
  llvm::DenseMap<const VarSymbol *, std::vector<std::pair<const Type *, llvm::SMLoc>>>
      destinations_;
  // Set by literalDestination when it finds such places.
  std::vector<std::pair<const Type *, llvm::SMLoc>> destinationConflict_;
  // The values of the string constants, the keys `o[key]` may use.
  llvm::DenseMap<const VarSymbol *, std::string> constKeys_;
  // The variables whose initializers are being checked, for the message
  // about one that uses itself: `const o = { f: () => o.n }`.
  llvm::SmallVector<llvm::StringRef, 2> initializing_;
  // Some link of the optional chain being checked may end it.
  bool chainShortCircuits_ = false;
  // The last `+` of the chain `a + b + c` whose left side is about to be
  // checked, and the first `+` of the chain being checked that joins a
  // number or a boolean with a string: the chain reports it once, with a
  // template made of all its parts.
  BinaryExpr *concatChain_ = nullptr;
  const BinaryExpr *mixedConcat_ = nullptr;
  // Inferring the return type of currentFunction_: its return statements,
  // and for a generator its yields.
  std::vector<ReturnStmt *> *returns_ = nullptr;
  std::vector<YieldExpr *> *yields_ = nullptr;
  // Checking a copy of a function of a cycle (see inferCycle): a call within
  // the cycle may not have a type yet.
  bool trial_ = false;
  // Top-level code is still being checked; then a function inferred early,
  // because top-level code calls it, cannot see the globals declared later.
  bool topLevelDone_ = false;
  const FuncSymbol *early_ = nullptr;
  std::vector<std::vector<FuncSymbol *>> cycles_;
  llvm::DenseMap<const FuncSymbol *, unsigned> cycleOf_;
  // The variables declared without a type that the scopes being checked hold
  // (see noteUntyped), with the types of the values assigned to them.
  struct Untyped {
    VarSymbol *var;
    bool initUndefined; // `let x = undefined`
    std::vector<const Type *> assigned;
  };
  std::vector<Untyped> untyped_;
  llvm::DenseMap<const VarSymbol *, int64_t> constVars_;
  // The functions being checked, innermost last; empty in main's code.
  std::vector<FunctionDecl *> functions_;
  // The groups of the blocks being checked: the names their functions
  // assign, which a call may change.
  std::vector<NestedGroup *> groups_;
  unsigned seq_ = 0; // see VarSymbol::declSeq
  SourceFile *file_ = nullptr;
  // The topics of the library, which the checks of calls ask (see compiler/library).
  const FeatureRegistry *features_ = nullptr;
  // The functions of every file, as declared: what InferPass looks for cycles in.
  std::vector<FunctionDecl *> declaredFunctions_;
  // Generic arrows that top-level constants name (see FunctionDecl::lifted).
  std::vector<FunctionDecl *> lifted_;
  std::deque<Instance> instanceStore_;
  std::vector<Instance *> pendingInstances_;
  Instance *checkingInstance_ = nullptr;
  // Generic functions whose own body has errors: no instance of them is
  // checked, which would only repeat those.
  llvm::SmallPtrSet<const FuncSymbol *, 4> brokenGenerics_;
  // See exactTypeParam: by generic function, for each type parameter.
  llvm::DenseMap<const FuncSymbol *, std::vector<bool>> exactParams_;
  // Generic classes: the instances by generic class and type arguments (the
  // generic class for its own type parameters), the defaults of the type
  // parameters, the instances made before the generic classes had their
  // methods, which wait for them, and the instances being filled, innermost
  // last, for those that need ever bigger ones.
  std::map<std::pair<const ClassDecl *, std::vector<const Type *>>, ClassDecl *> classInstances_;
  llvm::DenseMap<const ClassDecl *, std::vector<const Type *>> classDefaults_;
  std::vector<ClassDecl *> unfilled_;
  std::vector<const ClassDecl *> filling_;
  llvm::StringMap<TypeAliasDecl *> builtinAliases_;
  // The types `infer` gives (bound) for its type parameters (params) while a
  // type is matched against the pattern of a conditional type.
  struct InferMatch {
    llvm::ArrayRef<const Type *> params;
    std::vector<const Type *> bound;
  };
  // What a conditional type that waits needs to be resolved again: its
  // text, the names it uses that stand for types (whose types are the
  // operands after `tag`), and what the other names it uses mean where it
  // is written.
  struct ConditionalInfo {
    const TypeRef *ref = nullptr;
    const Type *tag = nullptr;
    std::vector<llvm::StringRef> bound;
    std::vector<std::pair<llvm::StringRef, Stmt *>> decls;
  };
  // Inside the `extends` clause of a conditional type, where `infer` may be.
  unsigned extendsClause_ = 0;
  // A conditional type that is the body of a type alias may end in another
  // instance of an alias (`Len<R, [...Acc, H]>`): a tail call, which
  // instantiateAlias makes in a loop rather than nested, so that it may go
  // 1000 deep as in TypeScript. tailAllowed_ says the conditional type being
  // resolved is such a body; tailCall_ is the call it ends in.
  struct TailCall {
    TypeAliasDecl *alias = nullptr;
    std::vector<const Type *> args;
  };
  bool tailAllowed_ = false;
  std::optional<TailCall> tailCall_;
  llvm::DenseSet<const CastExpr *> widenings_;
  bool narrowedFields_ = false;
  // The type of a rest parameter is being resolved: a tuple type there may
  // have a rest element, `[int, ...string[]]`, a list of parameters.
  bool listContext_ = false;
  // Where the types being substituted come from, for the instances of
  // generic classes they make (see instantiateClass); none while unset.
  llvm::SMLoc instanceLoc_;
  bool methodsDeclared_ = false; // every class of the program has its methods
  bool classesLinked_ = false;   // linkClasses ran
  // Generic classes whose own bodies have errors: no instance of them is
  // checked, which would only repeat those.
  llvm::SmallPtrSet<const ClassDecl *, 2> brokenClasses_;
  // Where an instance's body calls an instance whose body failed: the error
  // is that one's.
  llvm::DenseMap<const char *, const Instance *> failedCalls_;
  // The conversions generic code made of values of type parameters (see
  // noteForced), by the position and kind of the expression.
  llvm::DenseMap<std::pair<const char *, unsigned>, const Type *> forced_;
  // The instances of generic aliases, and the alias and arguments that made
  // an object type (to substitute in it).
  struct AliasInstance {
    const Type *type = nullptr;
    Type *self = nullptr; // while it is resolved, for the fields that refer to it
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
  // See useModule. loading_ is the file whose top-level code is being
  // checked; null after all of it is.
  std::vector<ModuleState> moduleStates_;
  Module *module_ = nullptr;
  Module *loading_ = nullptr;
  // The file of each top-level function, type alias, enum, class and
  // method.
  llvm::DenseMap<const Stmt *, Module *> declModules_;
  // Classes (sema_class.cpp): the class whose code is being checked, which
  // may use its private members; the constructor (or initializers) being
  // checked and its `this`; whether the closures it creates may use `this`
  // (every field was set where they are created); checking a static
  // initializer, where `this` would be the class.
  ClassDecl *currentClass_ = nullptr;
  FunctionDecl *initFn_ = nullptr;
  const VarSymbol *initThis_ = nullptr;
  bool closureThisReady_ = true;
  bool staticInit_ = false;
  // `this.x` is being checked, a use of `this` the rules allow: x.
  llvm::StringRef thisMember_;
  // The callback being checked as an argument of a built-in such as map:
  // what it is called with, which chooses the variant of a method.
  const Signature *callback_ = nullptr;
  // The static fields of the class being declared that are set by now.
  llvm::SmallPtrSet<const VarSymbol *, 4> staticsSet_;
  unsigned classCount_ = 0;
  std::vector<ClassDecl *> classes_;
  // The `super(...)` statements at the top level of the bodies of
  // constructors: the only place where they are allowed.
  llvm::SmallPtrSet<const CallExpr *, 4> rootSupers_;
  // Abstract methods without a return type: the implementation whose type
  // they take; and overrides without one, whose type is that of the method
  // they override, once known (see inheritedReturnType).
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> returnSources_;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> inheritsReturn_;
  // The functions whose return types are being inferred, innermost last,
  // and those whose inference waits for a file checked out of turn: a call
  // to one of these is a cycle the types cannot settle.
  std::vector<const FuncSymbol *> inferring_;
  llvm::SmallPtrSet<const FuncSymbol *, 4> suspended_;
  // The callbacks of the methods of typed arrays (see ByteCallback).
  llvm::DenseMap<const FunctionDecl *, ByteCallback> byteCallbacks_;
  // The class Error (see declareErrorClass).
  ClassDecl *errorClass_ = nullptr;
  // Error and its built-in kinds, as kErrorKinds lists them.
  std::vector<ClassDecl *> errorKinds_;
  // SystemError, which the functions of node:fs throw (docs/io.md), and the
  // classes Stats and Dirent of node:fs.
  ClassDecl *systemErrorClass_ = nullptr;
  ClassDecl *statsClass_ = nullptr;
  ClassDecl *direntClass_ = nullptr;
  // ExecSyncError, which execSync and execFileSync throw when the program
  // they ran failed (docs/process.md).
  ClassDecl *execSyncErrorClass_ = nullptr;
  MemberExpr *pendingSetter_ = nullptr;
  llvm::DenseMap<const VarSymbol *, std::string> constPrefixes_;

public:
  ~Sema();

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
  ResultAsyncChecker &resultAsyncChecker() const { return *resultAsyncCheckerPart_; }
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
  ThrowChecker &throwChecker() const { return *throwCheckerPart_; }
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
  GenericClassInstantiator &genericClassInstantiator() const { return *genericClassInstantiatorPart_; }
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
  SafeTryChecker &safeTryChecker() const { return *safeTryCheckerPart_; }
  ResultMethodChecker &resultMethodChecker() const { return *resultMethodCheckerPart_; }
  ChunkTypes &chunkTypes() const { return *chunkTypesPart_; }
  StreamNewChecker &streamNewChecker() const { return *streamNewCheckerPart_; }
  StreamChecker &streamChecker() const { return *streamCheckerPart_; }
  SetEncodingChecker &setEncodingChecker() const { return *setEncodingCheckerPart_; }
  StreamSources &streamSources() const { return *streamSourcesPart_; }
  StreamPipeline &streamPipeline() const { return *streamPipelinePart_; }
  TzModulesChecker &tzModulesChecker() const { return *tzModulesCheckerPart_; }
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
  friend class ResultAsyncChecker;
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
  friend class ThrowChecker;
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
  friend class SafeTryChecker;
  friend class ResultMethodChecker;
  friend class ChunkTypes;
  friend class StreamNewChecker;
  friend class StreamChecker;
  friend class SetEncodingChecker;
  friend class StreamSources;
  friend class StreamPipeline;
  friend class TzModulesChecker;
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
  std::unique_ptr<ResultAsyncChecker> resultAsyncCheckerPart_;
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
  std::unique_ptr<ThrowChecker> throwCheckerPart_;
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
  std::unique_ptr<SafeTryChecker> safeTryCheckerPart_;
  std::unique_ptr<ResultMethodChecker> resultMethodCheckerPart_;
  std::unique_ptr<ChunkTypes> chunkTypesPart_;
  std::unique_ptr<StreamNewChecker> streamNewCheckerPart_;
  std::unique_ptr<StreamChecker> streamCheckerPart_;
  std::unique_ptr<SetEncodingChecker> setEncodingCheckerPart_;
  std::unique_ptr<StreamSources> streamSourcesPart_;
  std::unique_ptr<StreamPipeline> streamPipelinePart_;
  std::unique_ptr<TzModulesChecker> tzModulesCheckerPart_;
  std::unique_ptr<WeakRefChecker> weakRefCheckerPart_;
};

// A tuple type of generic code with lists of types spread into it, `[int,
// ...A]`.
bool spreadsLists(const Type *type);

// Whether `type` mentions one of the type parameters `params`.
bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params);

// The instance of the generic class `generic` (or `generic` itself) that the
// class `type` is, or that a class up its chain is; null if none.
const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic);

// "2 arguments", "1-3 arguments", "1 or more arguments".
std::string argumentCount(unsigned min, unsigned max);

// A language tag as BCP 47 writes it: "ru", "en-US", "zh-Hant-TW",
// "de-DE-u-co-phonebk". What ICU would read wrongly ("en_US") is not one.
bool isLanguageTag(const std::string &tag);

// Why a compiled program cannot have the global `name` of JavaScript (eval,
// Function, Proxy), or null for any other name.
const char *whyUnsupported(llvm::StringRef name);

// A function in messages: "'f'" and "function 'f'", or "the function" for an
// arrow without a name.
inline std::string quotedName(llvm::StringRef name) {
  return name.empty() ? "the function" : "'" + name.str() + "'";
}
inline std::string functionLabel(llvm::StringRef name) {
  return name.empty() ? "the function" : "function '" + name.str() + "'";
}

// A variant of a method in messages, `add(v: Vec, k?: double)`, and a list
// of them, "f(), f(x: int) and f(s: string)".
std::string variantText(const FunctionDecl *fn);
std::string variantList(llvm::ArrayRef<FunctionDecl *> variants);

// The statements a literal declared by `stmts[i]` may go to: the ones after
// it, and the functions declared before it, which may run later.
std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i);

// The text `o.name` that reads a field of `object`, for hints;
// `o["content-type"]` for a name that is not an identifier.
std::string fieldText(const Expr *object, llvm::StringRef field);

// The names a statement or an expression mentions. With `inFunction` unset,
// only those inside the functions it contains.
void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction = true);
void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction = true);
// The names of the methods `body` calls, `x.name(...)`.
void collectMemberCalls(const Stmt *body, llvm::StringSet<> &names);

// Calls `visit` on every expression inside `stmt`, those in the functions it
// contains included, unless `intoFunctions` is unset.
void visitExprs(const Stmt *stmt, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions = true);
void visitExprs(const Expr *expr, llvm::function_ref<void(const Expr *)> visit,
                bool intoFunctions = true);

} // namespace tz
