// What each piece of async code reads and writes, and which pieces may run
// in parallel (docs/async.md, sections 2-5; step 3 of its plan).
//
// A piece is the code of an async function between two awaits, or from its
// start to the first await. The top-level code is a piece up to its first
// await, and so is a callback that runs later on its own: one of then, of a
// timer, of queueMicrotask. The start of an async function runs inside the
// piece that calls it, as in node, and so does all of a function that is not
// async.
//
// The pass runs after Sema over the whole program. It walks each function
// the program may run once and splits its body at the awaits: an access of
// data that other code may see (a global, a captured variable, a field, the
// elements of an array or the entries of a dictionary, output, files) goes
// to the pieces that may be running where it is written, and so does a call.
// Data are told apart as docs/async.md, section 4 says: fields by name;
// objects by the expression that makes them where a constant holds them
// (`const a = new Counter()`), by type otherwise; the objects a function
// makes and keeps to itself not at all; and the element of an array that a
// callback of map writes at its own index, each task its own. A call adds
// what the start of the function called does, with `this` the object it is
// called on; a function value may be any function of its type that the
// program has as a value. The built-in modules written in tz (src/lib) are
// not walked: what their functions do with what the program gives them is
// described (see libraryCall).
//
// Then come the tasks: the top-level code, each call of an async function
// that nothing awaits right away, and each callback. A task runs its pieces
// one after another; tasks run at the same time, and so may several copies
// of one task (a call in a loop or in a callback of map). The tasks that an
// `await Promise.all(...)` waits for end before the code after it, unless a
// rejection there could be caught and code go on while some still run: so
// the tasks of two such awaits one after another never meet.
//
// Two pieces that may run at the same time and touch one location, one of
// them writing it, keep node's order; any other two may run in parallel. The
// part that keeps the order is the one from the first to the last access of
// that data in the piece.

#include "sema/parallel.h"

#include "sema/crypto_api.h"
#include "sema/types.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {
namespace {

//===----------------------------------------------------------------------===//
// Locations
//===----------------------------------------------------------------------===//

// The objects an access may touch, told apart as far as the pass can.
enum class RefKind : uint8_t {
  // An object that a function makes and keeps to itself: no other code sees
  // it, so accesses to it are nobody else's business and are not recorded.
  Local,
  // The `this` of the method `ptr` (a FunctionDecl): which object it is, the
  // call of the method says.
  This,
  // The objects that the expression `ptr` makes: objects of two such
  // expressions are never the same.
  Site,
  // Any object of the type `ptr`; null for any object at all.
  Type,
};

struct Ref {
  RefKind kind = RefKind::Type;
  const void *ptr = nullptr;
  bool operator==(const Ref &o) const { return kind == o.kind && ptr == o.ptr; }
  bool operator!=(const Ref &o) const { return !(*this == o); }
  bool operator<(const Ref &o) const {
    return kind != o.kind ? kind < o.kind : std::less<const void *>()(ptr, o.ptr);
  }
};

enum class LocKind : uint8_t {
  Var,      // a variable that several functions see: key
  Field,    // the field named key of ref ("*": any of its fields)
  Elements, // the elements of an array, the entries of a dictionary, the bytes of binary data
  OwnCell,  // `a[i]` where i is the index the callback key of map gets: each task its own
  Contents, // anything that can be reached from ref, as console.log reads it
  Resource, // output, input, files: res
};

enum class Res : uint8_t {
  Stdout,
  Stderr,
  Stdin,
  Files,
  Process,  // the exit code and the environment
  Signals,  // the state of the AbortSignals
  Metadata, // the records of Reflect.defineMetadata
  Runtime,  // what a built-in the pass does not know may change
};

struct Loc {
  LocKind kind = LocKind::Var;
  Ref ref;
  const void *key = nullptr;
  Res res = Res::Stdout;
  bool operator==(const Loc &o) const {
    return kind == o.kind && ref == o.ref && key == o.key && res == o.res;
  }
  bool operator<(const Loc &o) const {
    if (kind != o.kind)
      return kind < o.kind;
    if (ref != o.ref)
      return ref < o.ref;
    if (key != o.key)
      return std::less<const void *>()(key, o.key);
    return res < o.res;
  }
};

struct Access {
  Loc loc;
  bool write = false;
  // Where in the piece's own code: the access, or the call that makes it.
  llvm::SMLoc at;
  // The function called there that makes it; null for the code itself.
  const FunctionDecl *via = nullptr;
  bool operator<(const Access &o) const {
    if (!(loc == o.loc))
      return loc < o.loc;
    if (write != o.write)
      return write < o.write;
    return std::less<const char *>()(at.getPointer(), o.at.getPointer());
  }
};

using Effects = std::set<Access>;

// The pieces active at a point of a function, by the number of the segment
// where each starts; kept sorted.
using Active = llvm::SmallVector<unsigned, 4>;

void merge(Active &into, llvm::ArrayRef<unsigned> from) {
  for (unsigned s : from) {
    auto it = std::lower_bound(into.begin(), into.end(), s);
    if (it == into.end() || *it != s)
      into.insert(it, s);
  }
}

//===----------------------------------------------------------------------===//
// Functions, their segments and their calls
//===----------------------------------------------------------------------===//

enum class SegKind : uint8_t {
  Start,
  Await,    // after an `await`, or a `yield*` of an async generator
  ForAwait, // after the await of each step of a `for await`
  Yield,    // after a `yield` of an async generator: the loop that iterates it resumes it
};

// The code of a function from one point where it waits to the next ones.
struct Segment {
  SegKind kind = SegKind::Start;
  const Node *at = nullptr;
  llvm::SMLoc loc;
  bool delegate = false; // after a `yield*`
  // What it does: its own accesses, and then those of the functions it
  // calls, whose start runs inside it.
  Effects effects;
};

// How the functions that a call record names come to run.
enum class CallKind : uint8_t {
  Call,      // called here
  Inline,    // given to a built-in that calls it during the call: map, new Promise
  Later,     // given to a built-in that calls it later on its own: then, setTimeout
  IterSync,  // iterated here: a generator runs as it gives its values
  IterAsync, // iterated by `for await`: an async generator, inside the loop's task
};

// The listeners that the program gives to the built-in modules, by what runs
// them (CallRec::channel): those of emitters, and those of event targets and
// abort signals.
enum Channel : int { EmitterListeners, TargetListeners, ChannelCount };

// How one function named by a call record runs.
enum class Mode : uint8_t {
  Sync,      // all of it, here
  Await,     // its start here, the rest in this task: the code waits for it
  Spawn,     // its start here, the rest as a task of its own
  Callback,  // later, as a task of its own
  AsyncIter, // an async generator: its start and its yields here, the rest in this task
};

struct Callee {
  unsigned fn = 0;
  // The `this` of the callee: the object it runs on; none for a function
  // without `this`.
  std::optional<Ref> self;
  Mode mode = Mode::Sync;
  bool byType = false; // a function value of the type, not this function by name
  bool operator==(const Callee &o) const { return fn == o.fn && self == o.self && mode == o.mode; }
};

struct CallRec {
  CallKind kind = CallKind::Call;
  const Node *site = nullptr;     // the call, or the function given, or the loop
  const CallExpr *call = nullptr; // the call itself, or of the built-in a function is given to
  llvm::SMLoc loc;
  Active segs; // where it runs
  std::vector<Callee> callees;
  // Resolved after the walk: any function of this type that the program has
  // as a value; with `byName`, also the methods of that name of the classes
  // that the type `byObject` may hold (a method of an interface).
  const Type *byType = nullptr;
  llvm::StringRef byName;
  const Type *byObject = nullptr;
  std::optional<Ref> bySelf;
  // How the Promise of an async function called is used.
  const AwaitExpr *await = nullptr; // this await waits for it right away
  bool returned = false;            // an async function returns it, and its own Promise follows it
  bool voided = false;              // `void f()`
  const AwaitExpr *join = nullptr;  // given to the Promise.all (or allSettled) this await waits for
  // It may run several times in one run of its function: in a loop, or a
  // callback that map calls for each element (inLoop: only the loop).
  bool repeated = false;
  bool inLoop = false;
  bool interval = false; // setInterval: the callback runs again and again
  // A callback of then and the like: a Promise takes what it throws, where
  // the error of a timer's callback ends the program.
  bool handled = false;
  // The listeners it runs, which the program gave to the built-in modules
  // (Analysis::listeners_), or -1: emit() runs those of emitters, abort()
  // and dispatchEvent() those of event targets.
  int channel = -1;
  // Reflect.apply: any method of the program's classes, or any function it
  // has as a value, on the object `bySelf`; Reflect.construct and fromJson
  // of a class value: the constructors of any of its classes.
  enum class Reflect : uint8_t { None, Apply, Construct } reflect = Reflect::None;
};

struct FnInfo {
  const FunctionDecl *decl = nullptr; // null for the top-level code
  // The method whose `this` the function sees: itself, or the method around
  // an arrow.
  const FunctionDecl *thisOwner = nullptr;
  bool async = false, asyncGen = false, syncGen = false;
  std::vector<Segment> segs;
  std::vector<CallRec> calls;
  llvm::DenseMap<const Node *, unsigned> segOf;
  llvm::DenseMap<std::pair<const void *, unsigned>, unsigned> callOf;
  // What runs inside whoever calls it: its start (and for an async generator
  // what follows each yield), with the functions it calls.
  Effects inlineEffects;
  // The calls of it that run its start inside their pieces: (function, call).
  std::vector<std::pair<unsigned, unsigned>> callers;
  // The calls that name it at all, for the checks of how often it runs.
  std::vector<std::pair<unsigned, unsigned>> named;
  // (a, b): segment b may start where segment a waits.
  std::set<std::pair<unsigned, unsigned>> succ;
  bool isInline(unsigned seg) const { return seg == 0 || segs[seg].kind == SegKind::Yield; }
};

// How the value of an expression is used, for what it calls.
struct Use {
  const AwaitExpr *awaited = nullptr; // the operand of this await
  bool returned = false;              // returned by an async function
  bool voided = false;                // `void e`
  const AwaitExpr *join = nullptr;    // a Promise that Promise.all takes, awaited there
  bool lambda = false;                // the arrow of a constant, which calls go to directly
};

//===----------------------------------------------------------------------===//
// Tasks and pieces
//===----------------------------------------------------------------------===//

// A function run in a task, with `this` an object: its scheduled segments
// are pieces of the task.
struct Activation {
  unsigned fn = 0;
  int root = 0;
  std::optional<Ref> self;
  // Its start is a piece of its own (a callback, the top-level code), rather
  // than part of the piece that calls it.
  bool callbackStart = false;
  // Its start is the start of the task, which runs inside the piece of
  // another task that starts it.
  bool rootStart = false;
  // The pieces of the task its inline segments run inside.
  std::set<unsigned> hosts;
};

struct Root {
  unsigned fn = 0;
  std::optional<Ref> self;
  int parent = -1;
  unsigned callerFn = 0;
  int rec = -1; // in callerFn; -1 for the top-level code
  Mode mode = Mode::Callback;
  bool multi = false;
  bool recursive = false;
  const AwaitExpr *join = nullptr;
  // The pieces of the parent where it starts, and those while it may run.
  std::vector<unsigned> spawnAt, interval;
  // The tasks up the chain, each with the pieces of its own during which
  // this one may run: those where the task below on the chain may, if that
  // one is waited for, or else any that may come after it starts.
  std::vector<std::pair<int, std::vector<unsigned>>> chain;
  // Each copy of the task writes the element of an array at its own index
  // (see LocKind::OwnCell): the callback of map that it runs.
  const FunctionDecl *ownCell = nullptr;
};

struct PieceInst {
  unsigned fn = 0;
  unsigned seg = 0;
  int root = 0;
  std::optional<Ref> self;
};

// What one location means for a piece: how it is touched, and where.
struct Touch {
  Loc loc;
  bool read = false, write = false;
  std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> ats, readAts, writeAts;
};

const Expr *strip(const Expr *e) {
  while (e) {
    if (auto *c = dyn_cast<CastExpr>(e); c && c->isImplicit()) {
      e = c->operand;
      continue;
    }
    if (auto *u = dyn_cast<UnaryExpr>(e); u && u->op == UnaryOp::NonNull) {
      e = u->operand;
      continue;
    }
    break;
  }
  return e;
}

// Whether values of `type` hold references to objects whose contents may
// change.
bool holdsReferences(const Type *type) {
  if (!type)
    return false;
  type = type->nonOptional();
  switch (type->kind()) {
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::Json:
  case Type::Kind::Iterator:
  case Type::Kind::Unknown:
  case Type::Kind::Param:
    return true;
  case Type::Kind::Union:
    return llvm::any_of(type->members(), holdsReferences);
  case Type::Kind::Result:
    return holdsReferences(type->okType()) || holdsReferences(type->errType());
  case Type::Kind::Promise:
    return holdsReferences(type->promised());
  default:
    return false;
  }
}

// The built-ins whose result is a new object, which no other code has yet.
bool makesNewObject(Builtin builtin) {
  switch (builtin) {
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrSlice:
  case Builtin::ArrConcat:
  case Builtin::ArrayFrom:
  case Builtin::IterToArray:
  case Builtin::StrSplit:
  case Builtin::StrSplitRegex:
  case Builtin::StrMatchGlobal:
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
  case Builtin::SetUnion:
  case Builtin::SetIntersection:
  case Builtin::SetDifference:
  case Builtin::SetSymmetricDifference:
  case Builtin::BytesNew:
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BytesOf:
  case Builtin::BytesSlice:
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted:
  case Builtin::BytesWith:
  case Builtin::BufferFromString:
  case Builtin::BufferAllocFill:
  case Builtin::BufferConcat:
  case Builtin::ArrayBufferNew:
  case Builtin::TextEncode:
  case Builtin::DateNew:
  case Builtin::DateCopy:
  case Builtin::DateFromString:
  case Builtin::DateFromFields:
  case Builtin::JsonParse:
  case Builtin::RegExpNew:
  case Builtin::WeakRefNew:
  case Builtin::CryptoKeyPair:
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
  case Builtin::IterFrom:
    return true;
  default:
    return isIteratorStage(builtin);
  }
}

// The built-ins that call the functions given to them, and when; the others
// keep them as values, which code may call later.
std::optional<CallKind> callbackKind(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseThen:
  case Builtin::PromiseCatch:
  case Builtin::PromiseFinally:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::RlQuestionCallback:
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr:
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse:
  case Builtin::ResultAsyncMatch:
  case Builtin::ResultAsyncUnwrapOr:
  case Builtin::ResultAsyncAndTee:
  case Builtin::ResultAsyncOrTee:
  case Builtin::ResultAsyncFromPromise:
    return CallKind::Later;
  case Builtin::ResultFromThrowable:
  case Builtin::ResultAsyncFromThrowable:
    return std::nullopt; // the function it makes calls them: they are values of its type
  default:
    break;
  }
  if (isResultCallback(builtin) || isIteratorStage(builtin) || isIteratorSink(builtin))
    return CallKind::Inline;
  switch (builtin) {
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
  case Builtin::ArrayFrom:
  case Builtin::MapForEach:
  case Builtin::DictGroupBy:
  case Builtin::StrReplaceRegexFn:
  case Builtin::SafeTry:
  case Builtin::PromiseNew:
    return CallKind::Inline;
  default:
    return std::nullopt;
  }
}

// The built-ins of `callbackKind` that call a function once, rather than for
// each element.
bool callsOnce(Builtin builtin) {
  switch (builtin) {
  case Builtin::PromiseNew:
  case Builtin::SafeTry:
    return true;
  default:
    return isResultCallback(builtin);
  }
}

// Whether a built-in may throw where it is called. Those of promises, timers
// and output do not: an error there rejects a Promise, or comes later.
bool mayThrow(Builtin builtin) {
  if (isPromiseBuiltin(builtin))
    return false;
  switch (builtin) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate:
    return false;
  default:
    return true;
  }
}

// Whether the pass knows what the built-in does (see Analysis::builtinEffects);
// one it does not know may change whatever it is given.
bool knownBuiltin(Builtin builtin);

// The name of a class of a built-in module; of a generic one, for its copies
// too.
llvm::StringRef libraryClassName(const ClassDecl *cls) {
  if (!cls)
    return "";
  return cls->generic ? llvm::StringRef(cls->generic->name) : llvm::StringRef(cls->name);
}

// The emitters of node:events, the event targets and the abort signals.
bool isEmitterClass(llvm::StringRef cls) {
  return cls == "EventEmitter" || cls == "EventEmitterBase";
}
bool isTargetClass(llvm::StringRef cls) {
  return cls == "EventTarget" || cls == "AbortSignal";
}
bool isSignalClass(llvm::StringRef cls) {
  return cls == "AbortSignal" || cls == "AbortController";
}

//===----------------------------------------------------------------------===//
// The analysis
//===----------------------------------------------------------------------===//

class Analysis {
public:
  Analysis(const SourceFile &program, const llvm::SourceMgr &sources)
      : program_(program), sources_(sources) {
    for (const Module *module : program.modules) {
      if (!module->builtin || module->file.items.empty())
        continue;
      if (unsigned id = sources.FindBufferContainingLoc(module->file.items.front()->loc())) {
        const llvm::MemoryBuffer *buffer = sources.getMemoryBuffer(id);
        libRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
      }
    }
    for (const Stmt *item : program.items)
      if (auto *cls = dyn_cast<ClassDecl>(item); cls && !cls->builtin && !inLibrary(cls->loc()))
        programClasses_.push_back(cls);
  }

  void run();
  ParallelPlan plan() const;
  void print(llvm::raw_ostream &out);

private:
  //===--------------------------------------------------------------------===//
  // The walk
  //===--------------------------------------------------------------------===//

  unsigned discover(const FunctionDecl *fn);
  void walk(unsigned fi);
  void stmt(const Stmt *s);
  void declarator(const VarDeclarator &d);
  void expr(const Expr *e, const Use &use = {});
  void name(const NameExpr *n, const Use &use);
  void member(const MemberExpr *m);
  void assign(const AssignExpr *a);
  void update(const Expr *target);
  void call(const CallExpr *c, const Use &use);
  void builtinCall(const CallExpr *c, const Use &use);
  void functionArg(const CallExpr *c, const Expr *arg, CallKind kind, const Use &use);
  void addFunctionCallees(const Expr *f, const Expr *arg, CallKind kind);
  bool inLibrary(llvm::SMLoc loc) const;
  bool isLibrary(const FunctionDecl *fn) const { return fn && inLibrary(fn->loc()); }
  const FunctionDecl *libraryTarget(const CallExpr *c) const;
  void libraryCall(const CallExpr *c, const FunctionDecl *decl, const Use &use);
  void libraryNew(const NewExpr *n);
  void libraryArg(const CallExpr *c, llvm::StringRef cls, llvm::StringRef name, const Expr *arg,
                  llvm::SMLoc at);
  void libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval = false,
                       bool handled = false);
  void listen(const Expr *f, Channel channel);
  void libraryReads(const Expr *e, llvm::SMLoc at);
  void dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use);
  void libraryState(llvm::StringRef cls, llvm::StringRef name, std::optional<Ref> self,
                    bool write, llvm::SMLoc at);
  bool librarySetter(const MemberExpr *m, const Expr *value);
  void builtinEffects(const CallExpr *c, const Expr *recv);
  void newExpr(const NewExpr *n);
  void awaitExpr(const AwaitExpr *a);
  void yieldExpr(const YieldExpr *y);
  void forOf(const ForOfStmt *f, llvm::StringRef label);
  void iterate(const Expr *iterable, const Node *site);
  void loop(llvm::StringRef label, llvm::function_ref<void()> head, llvm::function_ref<void()> body,
            llvm::function_ref<void()> step, bool headFirst);
  void throwPoint();
  void throughFinally();
  bool caught() const;
  void joinPoint();
  void moveTo(unsigned seg);
  unsigned segment(const Node *at, SegKind kind, llvm::SMLoc loc);
  CallRec &record(const void *key, CallKind kind, const Use &use, llvm::SMLoc loc);
  void addCallee(CallRec &rec, const FunctionDecl *decl, std::optional<Ref> self,
                 bool byType = false);
  std::vector<const FunctionDecl *> methodTargets(const FunctionDecl *method, bool virtualCall);
  std::optional<Ref> closureSelf(const FunctionDecl *callee, const FnInfo &caller) const;
  void registerValue(const FunctionDecl *fn, const Type *type);
  bool functionRef(const Expr *e, const Use &use, const Type *type);
  void noteClass(const ClassDecl *cls);

  void touch(LocKind kind, Ref ref, const void *key, bool write, llvm::SMLoc at,
             Res res = Res::Stdout);
  void resource(Res res, bool write, llvm::SMLoc at) {
    touch(LocKind::Resource, Ref(), nullptr, write, at, res);
  }
  void var(const VarSymbol *v, bool write, llvm::SMLoc at);
  void fieldAccess(const Expr *object, llvm::StringRef name, bool write, llvm::SMLoc at);
  void indexAccess(const IndexExpr *ix, bool write);
  void elements(const Expr *e, bool write, llvm::SMLoc at);
  void contents(const Expr *e, bool write, llvm::SMLoc at);
  void readSpread(const Expr *e, bool object, llvm::SMLoc at);
  const char *intern(llvm::StringRef name) { return names_.insert(name).first->getKeyData(); }

  //===--------------------------------------------------------------------===//
  // What objects are
  //===--------------------------------------------------------------------===//

  Ref refOf(const Expr *e);
  Ref receiverRef(const Expr *e);
  bool isAllocation(const Expr *e) const;
  bool isLocal(const VarSymbol *v);
  bool shared(const VarSymbol *v) const { return (v->isGlobal || v->captured) && v->reassigned; }
  using Escapes = llvm::DenseSet<const VarSymbol *>;
  const Escapes &escapes(const FunctionDecl *owner);
  void scanStmt(const Stmt *s, Escapes &out);
  void scanUses(const Expr *e, bool safe, Escapes &out, bool discarded = false);
  bool constructorLeaks(const ClassDecl *cls);
  const FunctionDecl *thisOwnerOf(const FunctionDecl *fn) const;
  const Type *typeOfRef(Ref r) const;
  bool sameObject(Ref a, Ref b);
  bool typesMayAlias(const Type *a, const Type *b);
  bool objectTypesMayAlias(const Type *a, const Type *b);
  bool mayReach(Ref from, Ref to, bool innerOnly);
  const std::vector<const Type *> &inside(const Type *type);
  bool locsMayMeet(const Loc &a, const Loc &b);

  //===--------------------------------------------------------------------===//
  // After the walk
  //===--------------------------------------------------------------------===//

  void resolve();
  void fold();
  std::optional<Access> substitute(const Access &a, const FnInfo &callee,
                                   const std::optional<Ref> &self, const CallRec &rec) const;
  Ref classRef(const FunctionDecl *owner) const;
  std::optional<Ref> classRefOrNone(const FnInfo &f) const {
    return f.thisOwner ? std::optional<Ref>(classRef(f.thisOwner)) : std::nullopt;
  }
  void explore();
  unsigned activate(unsigned fn, int root, std::optional<Ref> self, bool callbackStart,
                    bool rootStart);
  int newRoot(int parent, unsigned fn, std::optional<Ref> self, unsigned callerFn, unsigned rec,
              Mode mode);
  std::optional<Ref> bind(const std::optional<Ref> &self, const Activation &caller) const;
  bool scheduled(const Activation &act, unsigned seg) const;
  void findPieces();
  void findHosts();
  std::vector<unsigned> piecesAt(const Activation &act, llvm::ArrayRef<unsigned> segs) const;
  bool once(unsigned fi, unsigned rec, unsigned rootFn, unsigned depth);
  bool fnOnce(unsigned fi, unsigned rootFn, unsigned depth);
  bool rejectionEnds(const AwaitExpr *a, unsigned depth);
  bool fnRejectionEnds(unsigned fi, unsigned depth);
  void findJoins();
  void buildFlow();
  std::vector<unsigned> reach(int root, const std::vector<unsigned> &from) const;
  unsigned startNode(int root) const { return unsigned(pieces_.size()) + unsigned(root); }
  bool concurrent(unsigned a, unsigned b) const;
  uint64_t maskOf(const Loc &loc) const;
  void decide();

  //===--------------------------------------------------------------------===//
  // Explaining
  //===--------------------------------------------------------------------===//

  struct Pos {
    unsigned buffer = 0;
    unsigned offset = 0;
    bool operator<(const Pos &o) const {
      return buffer != o.buffer ? buffer < o.buffer : offset < o.offset;
    }
  };
  Pos posOf(llvm::SMLoc loc) const;
  std::string where(llvm::SMLoc loc) const;
  // "at line 12", or "at lib.tz:12" in another file; "line 12" or
  // "lib.tz:12".
  static bool inOtherFile(const std::string &where) {
    return where.find(".tz:") != std::string::npos;
  }
  static std::string atText(const std::string &where) {
    return inOtherFile(where) ? "at " + where : "at line " + where;
  }
  static std::string lineText(const std::string &where) {
    return inOtherFile(where) ? where : "line " + where;
  }
  unsigned lineOf(llvm::SMLoc loc) const;
  std::string fnTitle(const FnInfo &f) const;
  std::string fnName(const FnInfo &f) const;
  std::string segTitle(const FnInfo &f, unsigned seg) const;
  std::string rootTitle(int root) const;
  std::string rootTitleAt(int root, bool column) const;
  std::string pieceTitle(unsigned piece, bool withContext) const;
  std::string contextOf(unsigned piece) const;
  std::string refName(Ref r) const;
  std::string locName(const Loc &loc) const;
  std::string typeText(const Type *t) const;
  std::string calleeText(const Expr *e) const;
  std::string callText(const CallExpr *c) const;
  std::string positions(const std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> &ats) const;
  std::vector<Touch> touches(const Effects &effects) const;
  std::vector<std::string> decisionLines(unsigned piece) const;

  const SourceFile &program_;
  const llvm::SourceMgr &sources_;

  std::deque<FnInfo> fns_;
  llvm::DenseMap<const FunctionDecl *, unsigned> fnIndex_;
  std::deque<unsigned> worklist_;
  llvm::StringSet<> names_;
  const char *star_ = nullptr;
  const char *length_ = nullptr;
  const char *inner_ = nullptr;
  const char *state_ = nullptr;
  const char *listenersKey_ = nullptr;
  llvm::DenseMap<const VarSymbol *, const Expr *> declInit_;
  llvm::DenseMap<const Expr *, llvm::StringRef> siteName_;
  // The functions the program has as values, by their type.
  std::map<const Type *, std::vector<unsigned>> values_;
  llvm::DenseSet<unsigned> valueFns_;
  // The built-in modules written in tz (src/lib): the program's calls into
  // them are described (libraryCall), and their code is not walked.
  std::vector<std::pair<const char *, const char *>> libRanges_;
  // The listeners the program gives them, by what runs them (CallRec::
  // channel): those of emitters (on, once...), and those of event targets
  // and abort signals (addEventListener, onabort); the functions by name,
  // and the types of the function values given.
  std::vector<unsigned> listeners_[ChannelCount];
  std::vector<const Type *> listenerTypes_[ChannelCount];
  // The functions that settle a Promise, given to the executor of `new
  // Promise`: no code of the program.
  llvm::DenseSet<const VarSymbol *> resolvers_;
  // The index parameters of callbacks of map and forEach (see OwnCell).
  llvm::DenseSet<const VarSymbol *> ownIndex_;
  // The callbacks of map whose results Promise.all of this await takes.
  llvm::DenseMap<const FunctionDecl *, const AwaitExpr *> joinReturns_;
  struct AwaitInfo {
    unsigned fn = 0;
    bool caught = false;
  };
  llvm::DenseMap<const AwaitExpr *, AwaitInfo> awaits_;
  struct JoinInfo {
    unsigned fn = 0;
    Active segs;
    bool settled = false;
  };
  llvm::DenseMap<const AwaitExpr *, JoinInfo> joins_;
  std::vector<const ClassDecl *> classes_;
  // The classes the program declares, whose objects reflection may make.
  std::vector<const ClassDecl *> programClasses_;
  llvm::DenseSet<const ClassDecl *> classSeen_;
  std::map<const FunctionDecl *, Escapes> escapes_;
  llvm::DenseMap<const ClassDecl *, bool> leaks_;
  std::map<std::pair<const Type *, const Type *>, bool> alias_;
  std::map<const Type *, std::vector<const Type *>> reach_;
  std::map<std::pair<unsigned, unsigned>, bool> fnOnce_;
  std::map<unsigned, bool> rejectionEnds_;

  // The state of the walk of one function.
  unsigned fi_ = 0;
  FnInfo *fn_ = nullptr;
  Active cur_;
  struct Target {
    llvm::StringRef label;
    bool loop = false;
    bool block = false; // a labeled block, which only `break label` leaves
    Active breaks, continues;
  };
  std::vector<Target> targets_;
  enum class Phase { Block, Handler, Finalizer };
  struct TryState {
    Phase phase = Phase::Block;
    bool handler = false, finalizer = false;
    Active entry, finallyEntry;
  };
  std::vector<TryState> tries_;
  unsigned loops_ = 0;
  llvm::StringRef label_;
  struct OpenJoin {
    const AwaitExpr *await;
    Active active;
  };
  std::vector<OpenJoin> openJoins_;

  // Tasks and pieces.
  std::vector<Activation> acts_;
  std::map<std::tuple<unsigned, int, std::optional<Ref>, bool>, unsigned> actIndex_;
  // From a call of a function run inline: (caller, call, callee).
  std::vector<std::tuple<unsigned, unsigned, unsigned>> inlineEdges_;
  std::vector<Root> roots_;
  std::vector<PieceInst> pieces_;
  std::map<std::tuple<unsigned, unsigned, int, std::optional<Ref>>, unsigned> pieceIndex_;
  std::vector<std::vector<unsigned>> actPieces_;
  // The activations of a function in a task.
  std::map<std::pair<unsigned, int>, std::vector<unsigned>> actsByTask_;
  const std::vector<unsigned> &actsOf(unsigned fn, int root) const {
    static const std::vector<unsigned> none;
    auto found = actsByTask_.find({fn, root});
    return found == actsByTask_.end() ? none : found->second;
  }
  std::map<std::tuple<int, unsigned, std::optional<Ref>, unsigned, unsigned>, int> rootIndex_;
  // The pieces of each segment, and how many there are of a segment in a task.
  std::map<std::pair<unsigned, unsigned>, std::vector<unsigned>> segPieces_;
  std::map<std::tuple<unsigned, unsigned, int>, unsigned> sameTask_;
  // The pieces of each task that may follow each piece (and its start).
  std::vector<std::map<unsigned, std::vector<unsigned>>> flow_;

  // Decisions, by piece.
  struct Partner {
    unsigned piece;
    std::vector<Loc> locs;
  };
  struct Decision {
    bool alone = true;
    std::vector<Partner> partners;
    llvm::SMLoc from, to;
    // Every place of the piece's own code where it touches what keeps
    // node's order (the access, or the call that makes it).
    std::vector<llvm::SMLoc> ats;
  };
  std::vector<std::vector<Touch>> pieceTouches_;
  std::vector<Decision> decisions_;
  mutable std::vector<std::string> rootTitles_;
};

//===----------------------------------------------------------------------===//
// The walk
//===----------------------------------------------------------------------===//

const FunctionDecl *Analysis::thisOwnerOf(const FunctionDecl *fn) const {
  for (; fn; fn = fn->parent) {
    if (fn->takesThis())
      return fn;
    if (!fn->isArrow)
      return nullptr;
  }
  return nullptr;
}

unsigned Analysis::discover(const FunctionDecl *fn) {
  auto [it, inserted] = fnIndex_.try_emplace(fn, unsigned(fns_.size()));
  if (!inserted)
    return it->second;
  FnInfo &info = fns_.emplace_back();
  info.decl = fn;
  if (fn) {
    info.async = fn->isAsync && !fn->isGenerator;
    info.asyncGen = fn->isAsync && fn->isGenerator;
    info.syncGen = !fn->isAsync && fn->isGenerator;
    info.thisOwner = thisOwnerOf(fn);
    if (fn->cls)
      noteClass(fn->cls);
  }
  info.segs.emplace_back();
  if (fn)
    info.segs[0].loc = fn->loc();
  worklist_.push_back(it->second);
  return it->second;
}

void Analysis::noteClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent)
    if (classSeen_.insert(cls).second)
      classes_.push_back(cls);
}

void Analysis::walk(unsigned fi) {
  fi_ = fi;
  fn_ = &fns_[fi];
  cur_ = {0};
  targets_.clear();
  tries_.clear();
  openJoins_.clear();
  loops_ = 0;
  label_ = {};
  const FunctionDecl *decl = fn_->decl;
  if (!decl) {
    for (const Stmt *item : program_.items)
      if (!isa<FunctionDecl>(item) && !inLibrary(item->loc()))
        stmt(item);
    return;
  }
  for (const Param &p : decl->params)
    if (p.defaultValue)
      expr(p.defaultValue);
  stmt(decl->body);
}

unsigned Analysis::segment(const Node *at, SegKind kind, llvm::SMLoc loc) {
  auto [it, inserted] = fn_->segOf.try_emplace(at, unsigned(fn_->segs.size()));
  if (inserted) {
    Segment &seg = fn_->segs.emplace_back();
    seg.kind = kind;
    seg.at = at;
    seg.loc = loc;
  }
  return it->second;
}

CallRec &Analysis::record(const void *key, CallKind kind, const Use &use, llvm::SMLoc loc) {
  auto [it, inserted] =
      fn_->callOf.try_emplace(std::make_pair(key, unsigned(kind)), unsigned(fn_->calls.size()));
  if (inserted) {
    CallRec &rec = fn_->calls.emplace_back();
    rec.kind = kind;
    rec.site = static_cast<const Node *>(key);
    rec.loc = loc;
    rec.await = use.awaited;
    rec.returned = use.returned;
    rec.voided = use.voided;
    rec.join = use.join;
  }
  CallRec &rec = fn_->calls[it->second];
  rec.repeated |= loops_ > 0;
  rec.inLoop |= loops_ > 0;
  merge(rec.segs, cur_);
  return rec;
}

void Analysis::addCallee(CallRec &rec, const FunctionDecl *decl, std::optional<Ref> self,
                         bool byType) {
  if (!decl || !decl->body || isLibrary(decl))
    return;
  unsigned fi = discover(decl);
  const FnInfo &callee = fns_[fi];
  Mode mode = Mode::Sync;
  switch (rec.kind) {
  case CallKind::Call:
    // Calling an async generator only makes it: the loop that iterates it
    // runs it.
    if (callee.asyncGen)
      return;
    mode = callee.async ? (rec.await || rec.returned ? Mode::Await : Mode::Spawn) : Mode::Sync;
    break;
  case CallKind::Inline:
    if (callee.asyncGen)
      return;
    mode = callee.async ? Mode::Spawn : Mode::Sync;
    break;
  case CallKind::Later:
    mode = Mode::Callback;
    break;
  case CallKind::IterSync:
    if (!callee.syncGen)
      return;
    mode = Mode::Sync;
    break;
  case CallKind::IterAsync:
    if (!callee.asyncGen)
      return;
    mode = Mode::AsyncIter;
    break;
  }
  Callee c;
  c.fn = fi;
  c.self = self;
  c.mode = mode;
  c.byType = byType;
  if (!llvm::is_contained(rec.callees, c))
    rec.callees.push_back(c);
}

std::vector<const FunctionDecl *> Analysis::methodTargets(const FunctionDecl *method,
                                                          bool virtualCall) {
  std::vector<const FunctionDecl *> targets{method};
  if (!virtualCall || !method)
    return targets;
  // The methods that override it, down the chain: any of them may run.
  std::vector<const ClassDecl *> pending;
  if (method->cls)
    for (const ClassDecl *child : method->cls->children)
      pending.push_back(child);
  while (!pending.empty()) {
    const ClassDecl *cls = pending.back();
    pending.pop_back();
    noteClass(cls);
    for (const FunctionDecl *fn : cls->methods)
      for (const FunctionDecl *up = fn->overrides; up; up = up->overrides)
        if (up == method) {
          targets.push_back(fn);
          break;
        }
    for (const ClassDecl *child : cls->children)
      pending.push_back(child);
  }
  return targets;
}

std::optional<Ref> Analysis::closureSelf(const FunctionDecl *callee, const FnInfo &caller) const {
  const FunctionDecl *owner = thisOwnerOf(callee);
  if (!owner)
    return std::nullopt;
  if (owner == caller.thisOwner)
    return Ref{RefKind::This, owner};
  return classRef(owner);
}

Ref Analysis::classRef(const FunctionDecl *owner) const {
  return Ref{RefKind::Type, owner && owner->cls ? owner->cls->type : nullptr};
}

void Analysis::registerValue(const FunctionDecl *fn, const Type *type) {
  if (!fn || !fn->body || isLibrary(fn))
    return;
  unsigned fi = discover(fn);
  valueFns_.insert(fi);
  if (!type || !type->nonOptional()->isFunction())
    return;
  std::vector<unsigned> &list = values_[type->nonOptional()];
  if (!llvm::is_contained(list, fi))
    list.push_back(fi);
}

// Registers the function `e` names, if it names one, as a value of `type`.
bool Analysis::functionRef(const Expr *e, const Use &use, const Type *type) {
  e = strip(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    if (!use.lambda)
      registerValue(fe->fn, type);
    return true;
  }
  auto *n = dyn_cast_or_null<NameExpr>(e);
  if (n && n->func) {
    registerValue(n->func->decl, type);
    return true;
  }
  if (n && n->var && (n->var->function || n->var->lambda)) {
    registerValue(n->var->function ? n->var->function : n->var->lambda, type);
    return true;
  }
  auto *m = dyn_cast_or_null<MemberExpr>(e);
  if (m && m->method) {
    for (const FunctionDecl *fn : methodTargets(m->method->decl, m->virtualCall))
      registerValue(fn, type);
    return true;
  }
  return false;
}

void Analysis::touch(LocKind kind, Ref ref, const void *key, bool write, llvm::SMLoc at, Res res) {
  if (ref.kind == RefKind::Local || cur_.empty())
    return;
  Access a;
  a.loc.kind = kind;
  a.loc.ref = ref;
  a.loc.key = key;
  a.loc.res = res;
  a.write = write;
  a.at = at;
  for (unsigned s : cur_)
    fn_->segs[s].effects.insert(a);
}

void Analysis::var(const VarSymbol *v, bool write, llvm::SMLoc at) {
  if (v && shared(v))
    touch(LocKind::Var, Ref(), v, write, at);
}

// The pieces active now may throw, into the nearest `catch` or `finally`.
void Analysis::throwPoint() {
  for (auto it = tries_.rbegin(); it != tries_.rend(); ++it) {
    if (it->phase == Phase::Block) {
      merge(it->entry, cur_);
      merge(it->finallyEntry, cur_);
      if (it->handler)
        return;
    } else if (it->phase == Phase::Handler) {
      merge(it->finallyEntry, cur_);
    }
  }
}

// A return, break or continue runs the `finally` blocks it leaves.
void Analysis::throughFinally() {
  for (TryState &t : tries_)
    if (t.finalizer && t.phase != Phase::Finalizer)
      merge(t.finallyEntry, cur_);
}

// Whether an error here could be caught in this function, or run a
// `finally` block, before it leaves the function.
bool Analysis::caught() const {
  for (auto it = tries_.rbegin(); it != tries_.rend(); ++it) {
    if (it->phase == Phase::Block && (it->handler || it->finalizer))
      return true;
    if (it->phase == Phase::Handler && it->finalizer)
      return true;
  }
  return false;
}

void Analysis::joinPoint() {
  for (OpenJoin &j : openJoins_)
    merge(j.active, cur_);
}

// The code waits here and goes on in segment `seg`.
void Analysis::moveTo(unsigned seg) {
  for (unsigned s : cur_)
    fn_->succ.insert({s, seg});
  cur_ = {seg};
  joinPoint();
  throwPoint();
}

// A loop: `head` runs before each pass (the condition, or the next step of an
// iteration) and the loop may end after it; then the pass, `body`, and
// `step`, where `continue` goes too. A do-while has its head after the body.
// The passes are walked until the pieces active at the head settle.
void Analysis::loop(llvm::StringRef label, llvm::function_ref<void()> head,
                    llvm::function_ref<void()> body, llvm::function_ref<void()> step,
                    bool headFirst) {
  Active entry = cur_, exits;
  ++loops_;
  while (true) {
    cur_ = entry;
    if (headFirst) {
      head();
      merge(exits, cur_);
    }
    targets_.push_back({label, true, false, {}, {}});
    body();
    Target target = std::move(targets_.back());
    targets_.pop_back();
    merge(cur_, target.continues);
    if (step)
      step();
    if (!headFirst) {
      head();
      merge(exits, cur_);
    }
    merge(exits, target.breaks);
    Active next = entry;
    merge(next, cur_);
    if (next == entry)
      break;
    entry = std::move(next);
  }
  --loops_;
  cur_ = std::move(exits);
}

void Analysis::stmt(const Stmt *s) {
  if (!s)
    return;
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body)
      stmt(x);
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls)
      declarator(d);
    return;
  case Stmt::Kind::Function:
    // Declared here; it runs when something calls it.
    return;
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(s)->expr);
    return;
  case Stmt::Kind::If: {
    auto *i = cast<IfStmt>(s);
    expr(i->cond);
    Active before = cur_;
    stmt(i->thenStmt);
    Active then = std::move(cur_);
    cur_ = std::move(before);
    stmt(i->elseStmt);
    merge(cur_, then);
    return;
  }
  case Stmt::Kind::While: {
    auto *w = cast<WhileStmt>(s);
    loop(std::exchange(label_, {}), [&] { expr(w->cond); }, [&] { stmt(w->body); }, {}, true);
    return;
  }
  case Stmt::Kind::DoWhile: {
    auto *w = cast<DoWhileStmt>(s);
    loop(std::exchange(label_, {}), [&] { expr(w->cond); }, [&] { stmt(w->body); }, {}, false);
    return;
  }
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    llvm::StringRef label = std::exchange(label_, {});
    stmt(f->init);
    loop(label, [&] { expr(f->cond); }, [&] { stmt(f->body); }, [&] { expr(f->step); }, true);
    return;
  }
  case Stmt::Kind::ForOf:
    forOf(cast<ForOfStmt>(s), std::exchange(label_, {}));
    return;
  case Stmt::Kind::Switch: {
    auto *sw = cast<SwitchStmt>(s);
    llvm::StringRef label = std::exchange(label_, {});
    expr(sw->discriminant);
    Active disc = cur_, fall;
    bool hasDefault = false;
    targets_.push_back({label, false, false, {}, {}});
    for (const SwitchCase &c : sw->cases) {
      cur_ = disc;
      merge(cur_, fall);
      if (c.test)
        expr(c.test);
      else
        hasDefault = true;
      for (const Stmt *x : c.body)
        stmt(x);
      fall = cur_;
    }
    Target target = std::move(targets_.back());
    targets_.pop_back();
    cur_ = std::move(fall);
    merge(cur_, target.breaks);
    if (!hasDefault && !sw->exhaustive)
      merge(cur_, disc);
    return;
  }
  case Stmt::Kind::Labeled: {
    auto *l = cast<LabeledStmt>(s);
    if (isa<WhileStmt, DoWhileStmt, ForStmt, ForOfStmt, SwitchStmt>(l->body)) {
      label_ = l->label;
      stmt(l->body);
      return;
    }
    targets_.push_back({l->label, false, true, {}, {}});
    stmt(l->body);
    Target target = std::move(targets_.back());
    targets_.pop_back();
    merge(cur_, target.breaks);
    return;
  }
  case Stmt::Kind::Return: {
    auto *r = cast<ReturnStmt>(s);
    Use use;
    use.returned = fn_->async;
    // A callback of map whose results Promise.all waits for.
    if (!fn_->async && fn_->decl)
      use.join = joinReturns_.lookup(fn_->decl);
    expr(r->value, use);
    throughFinally();
    cur_.clear();
    return;
  }
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue: {
    bool isBreak = isa<BreakStmt>(s);
    llvm::StringRef label = isBreak ? cast<BreakStmt>(s)->label : cast<ContinueStmt>(s)->label;
    for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
      // Without a label, break leaves the innermost loop or switch, and
      // continue goes on with the innermost loop.
      bool matches = !label.empty() ? it->label == label : isBreak ? !it->block : it->loop;
      if (!matches)
        continue;
      throughFinally();
      merge(isBreak ? it->breaks : it->continues, cur_);
      break;
    }
    cur_.clear();
    return;
  }
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(s)->value);
    throwPoint();
    cur_.clear();
    return;
  case Stmt::Kind::Try: {
    auto *t = cast<TryStmt>(s);
    tries_.push_back({Phase::Block, t->handler != nullptr, t->finalizer != nullptr, {}, {}});
    stmt(t->block);
    merge(tries_.back().finallyEntry, cur_);
    Active normal = cur_;
    if (t->handler) {
      tries_.back().phase = Phase::Handler;
      cur_ = tries_.back().entry;
      stmt(t->handler);
      merge(tries_.back().finallyEntry, cur_);
      merge(normal, cur_);
    }
    TryState done = std::move(tries_.back());
    tries_.pop_back();
    if (t->finalizer) {
      // The block runs on every way out of the try; after it the code goes
      // on from any piece it may end in.
      cur_ = std::move(done.finallyEntry);
      stmt(t->finalizer);
    } else {
      cur_ = std::move(normal);
    }
    return;
  }
  case Stmt::Kind::Class: {
    // The static fields get their values and the decorators run where the
    // class is declared; its methods run when called.
    auto *cls = cast<ClassDecl>(s);
    if (cls->builtin || cls->generic)
      return;
    for (const ClassField &f : cls->fields)
      if (f.isStatic && f.init)
        expr(f.init);
    for (const Stmt *x : cls->decorations)
      stmt(x);
    return;
  }
  default:
    return;
  }
}

void Analysis::declarator(const VarDeclarator &d) {
  if (d.alias || !d.init)
    return;
  Use use;
  if (d.sym && d.sym->lambda)
    use.lambda = true; // calls through the constant go to the arrow directly
  expr(d.init, use);
  if (!d.sym)
    return;
  const Expr *init = strip(d.init);
  declInit_[d.sym] = init;
  if (!d.sym->reassigned && isAllocation(init))
    siteName_.try_emplace(init, d.name);
  // A closure made before the declaration may see the variable change here.
  if (d.sym->capturedEarly)
    var(d.sym, true, d.loc);
}

void Analysis::forOf(const ForOfStmt *f, llvm::StringRef label) {
  expr(f->iterable);
  loop(
      label,
      [&] {
        iterate(f->iterable, f);
        if (f->isAwait)
          moveTo(segment(f, SegKind::ForAwait, f->loc())); // each step waits for the next value
      },
      [&] { stmt(f->body); }, {}, true);
}

// One step of iterating `iterable`: an iterator moves on, and a generator
// runs up to its next value.
void Analysis::iterate(const Expr *iterable, const Node *site) {
  if (!iterable || !iterable->type)
    return;
  const Type *type = iterable->type->nonOptional();
  llvm::SMLoc at = iterable->loc();
  const Expr *source = strip(iterable);
  // The lines of readline: each step takes the next line of input.
  if (type->runtimeKind() == Type::Runtime::Readline ||
      type->runtimeKind() == Type::Runtime::ReadlinePromises) {
    resource(Res::Stdin, true, at);
    return;
  }
  if (type->isAsyncIterator() || type->isIterator()) {
    throwPoint(); // a generator may throw as it runs
    bool asyncIter = type->isAsyncIterator();
    CallRec &rec = record(site, asyncIter ? CallKind::IterAsync : CallKind::IterSync, {}, at);
    auto *c = dyn_cast<CallExpr>(source);
    if (c && libraryTarget(c)) {
      // An iterator of a built-in module (a stream, on() of node:events):
      // each step moves on what it iterates, and the listeners and sources
      // the program gave run as their calls say.
      const FunctionDecl *lib = libraryTarget(c);
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (m && c->method)
        libraryState(libraryClassName(lib->cls), lib->name, receiverRef(m->object), true, at);
      return;
    }
    if (c && (c->func || c->method)) {
      // A generator called right here: its iterator is no one else's.
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (c->method)
        for (const FunctionDecl *fn : methodTargets(c->method->decl, c->virtualCall))
          addCallee(rec, fn, m ? std::optional<Ref>(receiverRef(m->object)) : std::nullopt);
      else
        addCallee(rec, c->func->decl, closureSelf(c->func->decl, *fn_));
      return;
    }
    // An iterator a built-in makes right here (of an array, a map...) runs
    // no code of the program, and is no one else's.
    if (c && c->builtin != Builtin::None && makesNewObject(c->builtin))
      return;
    rec.byType = type;
    elements(iterable, true, at);
    return;
  }
  if (type->isArray() || type->isDict() || type->isTypedArray() || type->isJson())
    elements(iterable, false, at);
}

void Analysis::expr(const Expr *e, const Use &use) {
  if (!e)
    return;
  switch (e->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    return;
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs)
      expr(x);
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements) {
      if (el.hole)
        continue;
      Use inner;
      if (!el.spread)
        inner.join = use.join; // an element of the array Promise.all takes
      expr(el.expr, inner);
      if (el.spread)
        readSpread(el.expr, false, el.expr->loc());
    }
    return;
  case Expr::Kind::ObjectLit: {
    auto *o = cast<ObjectLitExpr>(e);
    if (o->cls)
      noteClass(o->cls);
    for (const ObjectLitExpr::Property &p : o->props) {
      if (p.key)
        expr(p.key);
      if (p.method)
        continue; // a method of the object, which runs when called
      expr(p.value);
      if (p.spread)
        readSpread(p.value, true, p.loc);
    }
    return;
  }
  case Expr::Kind::Name:
    name(cast<NameExpr>(e), use);
    return;
  case Expr::Kind::Member:
    member(cast<MemberExpr>(e));
    return;
  case Expr::Kind::Index: {
    auto *ix = cast<IndexExpr>(e);
    expr(ix->object);
    expr(ix->index);
    indexAccess(ix, false);
    return;
  }
  case Expr::Kind::Unary: {
    auto *u = cast<UnaryExpr>(e);
    Use inner;
    if (u->op == UnaryOp::Void)
      inner.voided = true;
    if (u->op == UnaryOp::NonNull)
      inner = use;
    expr(u->operand, inner);
    if (u->op == UnaryOp::NonNull)
      throwPoint();
    return;
  }
  case Expr::Kind::Binary: {
    auto *b = cast<BinaryExpr>(e);
    if (b->op == BinaryOp::LogAnd || b->op == BinaryOp::LogOr || b->op == BinaryOp::Nullish) {
      // The right side may not run; the value is either side's.
      expr(b->lhs, use);
      Active before = cur_;
      expr(b->rhs, use);
      merge(cur_, before);
      return;
    }
    expr(b->lhs);
    expr(b->rhs);
    // A division by zero, or an operation on decimals, may throw.
    if (b->op == BinaryOp::Div || b->op == BinaryOp::Rem ||
        (b->lhs->type && b->lhs->type->nonOptional()->isDecimal()))
      throwPoint();
    return;
  }
  case Expr::Kind::Assign:
    assign(cast<AssignExpr>(e));
    return;
  case Expr::Kind::Update:
    update(cast<UpdateExpr>(e)->target);
    return;
  case Expr::Kind::Call:
    call(cast<CallExpr>(e), use);
    return;
  case Expr::Kind::New:
    newExpr(cast<NewExpr>(e));
    return;
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(e);
    expr(c->cond);
    Active before = cur_;
    expr(c->thenExpr, use);
    Active then = std::move(cur_);
    cur_ = std::move(before);
    expr(c->elseExpr, use);
    merge(cur_, then);
    return;
  }
  case Expr::Kind::Cast: {
    auto *c = cast<CastExpr>(e);
    // A function known by name, adapted to another type of function.
    if (c->type && c->type->isFunction() && functionRef(c->operand, use, c->type)) {
      expr(c->operand, use);
      return;
    }
    expr(c->operand, c->isImplicit() ? use : Use());
    // The text of an array shows its elements.
    if (c->type && c->type->isString() && holdsReferences(c->operand->type))
      contents(c->operand, false, c->loc());
    return;
  }
  case Expr::Kind::Sequence: {
    auto *s = cast<SequenceExpr>(e);
    for (size_t i = 0; i < s->exprs.size(); ++i)
      expr(s->exprs[i], i + 1 == s->exprs.size() ? use : Use());
    return;
  }
  case Expr::Kind::OptionalChain: {
    // Any link may end the chain.
    Active before = cur_;
    expr(cast<OptionalChainExpr>(e)->chain, use);
    merge(cur_, before);
    return;
  }
  case Expr::Kind::Function: {
    auto *fe = cast<FunctionExpr>(e);
    discover(fe->fn);
    functionRef(fe, use, fe->type);
    return;
  }
  case Expr::Kind::Yield:
    yieldExpr(cast<YieldExpr>(e));
    return;
  case Expr::Kind::Await:
    awaitExpr(cast<AwaitExpr>(e));
    return;
  }
}

void Analysis::name(const NameExpr *n, const Use &use) {
  // A function as a value: code may call it later through the value.
  if (n->func || (n->var && (n->var->function || n->var->lambda))) {
    functionRef(n, use, n->type);
    return;
  }
  var(n->var, false, n->loc());
}

void Analysis::member(const MemberExpr *m) {
  expr(m->object);
  if (m->method) {
    functionRef(m, {}, m->type); // a method bound to its object, as a value
    return;
  }
  fieldAccess(m->object, m->name, false, m->loc());
}

void Analysis::fieldAccess(const Expr *object, llvm::StringRef name, bool write, llvm::SMLoc at) {
  const Type *type = object && object->type ? object->type->nonOptional() : nullptr;
  if (!type)
    return;
  if (type->isArray()) {
    // The length is apart from the elements: a task that writes an element
    // of its own does not change it.
    if (name == "length") {
      touch(LocKind::Field, refOf(object), length_, write, at);
      if (write)
        touch(LocKind::Elements, refOf(object), nullptr, true, at);
    }
    return;
  }
  if (type->isTypedArray())
    return; // its length never changes
  if (type->isJson()) {
    touch(LocKind::Elements, refOf(object), nullptr, write, at);
    return;
  }
  const Field *field = nullptr;
  if (type->isObject()) {
    field = type->field(name);
  } else if (type->isUnion()) {
    for (const Type *m : type->members())
      if (m->isObject() && (field = m->field(name)))
        break;
  }
  // Built-in properties are calls; a field that never changes is not data
  // that could be shared wrongly.
  if (!field)
    return;
  if (!write && (field->readonly || (field->type && field->type->isLiteral())))
    return;
  touch(LocKind::Field, refOf(object), intern(name), write, at);
}

void Analysis::indexAccess(const IndexExpr *ix, bool write) {
  if (ix->enumDecl || !ix->object->type)
    return;
  if (!ix->field.empty()) {
    fieldAccess(ix->object, ix->field, write, ix->loc());
    return;
  }
  const Type *type = ix->object->type->nonOptional();
  if (ix->keyed) {
    touch(LocKind::Field, refOf(ix->object), star_, write, ix->loc());
    return;
  }
  if (type->isString())
    return;
  if (type->isArray()) {
    // The index a callback of map gets: each call its own element.
    auto *index = dyn_cast<NameExpr>(strip(ix->index));
    if (index && index->var && ownIndex_.contains(index->var) && !index->var->reassigned &&
        !ix->mayAppend) {
      touch(LocKind::OwnCell, refOf(ix->object), index->var->owner, write, ix->loc());
      return;
    }
    // A write past the end makes the array longer.
    if (write && ix->mayAppend)
      touch(LocKind::Field, refOf(ix->object), length_, true, ix->loc());
  }
  touch(LocKind::Elements, refOf(ix->object), nullptr, write, ix->loc());
}

void Analysis::elements(const Expr *e, bool write, llvm::SMLoc at) {
  if (e && holdsReferences(e->type))
    touch(LocKind::Elements, refOf(e), nullptr, write, at);
}

void Analysis::contents(const Expr *e, bool write, llvm::SMLoc at) {
  if (!e || !holdsReferences(e->type))
    return;
  // What a literal made here holds is what its parts hold; an array that a
  // built-in makes holds the objects of another.
  const Expr *made = strip(e);
  if (auto *lit = dyn_cast<ArrayLitExpr>(made)) {
    for (const ArrayLitExpr::Element &el : lit->elements)
      if (!el.hole)
        contents(el.expr, write, at);
    return;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(made)) {
    for (const ObjectLitExpr::Property &p : lit->props)
      if (!p.method)
        contents(p.value, write, at);
    return;
  }
  // The objects inside one that is the code's own may be anyone's.
  Ref ref = refOf(e);
  if (ref.kind != RefKind::Local) {
    touch(LocKind::Contents, ref, nullptr, write, at);
    return;
  }
  const Type *type = e->type->nonOptional();
  if (llvm::any_of(inside(type), holdsReferences))
    touch(LocKind::Contents, Ref{RefKind::Type, type}, inner_, write, at);
}

void Analysis::readSpread(const Expr *e, bool object, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (!type)
    return;
  if (type->isIterator() || type->isAsyncIterator()) {
    iterate(e, e);
    return;
  }
  if (object && type->isObject()) {
    touch(LocKind::Field, refOf(e), star_, false, at);
    return;
  }
  elements(e, false, at);
}

void Analysis::assign(const AssignExpr *a) {
  bool compound = a->op.has_value();
  bool logical = compound && (*a->op == BinaryOp::LogAnd || *a->op == BinaryOp::LogOr ||
                              *a->op == BinaryOp::Nullish);
  auto value = [&] {
    if (!logical) {
      expr(a->value);
      return;
    }
    Active before = cur_;
    expr(a->value);
    merge(cur_, before);
  };
  const Expr *target = a->target;
  if (auto *n = dyn_cast<NameExpr>(target)) {
    if (compound)
      var(n->var, false, n->loc());
    value();
    var(n->var, true, n->loc());
    return;
  }
  if (auto *m = dyn_cast<MemberExpr>(target)) {
    expr(m->object);
    if (compound)
      fieldAccess(m->object, m->name, false, m->loc());
    value();
    if (!librarySetter(m, a->value))
      fieldAccess(m->object, m->name, true, m->loc());
    return;
  }
  if (auto *ix = dyn_cast<IndexExpr>(target)) {
    expr(ix->object);
    expr(ix->index);
    if (compound)
      indexAccess(ix, false);
    value();
    indexAccess(ix, true);
    return;
  }
  expr(target);
  value();
}

void Analysis::update(const Expr *target) {
  if (auto *n = dyn_cast<NameExpr>(target)) {
    var(n->var, false, n->loc());
    var(n->var, true, n->loc());
  } else if (auto *m = dyn_cast<MemberExpr>(target)) {
    expr(m->object);
    fieldAccess(m->object, m->name, false, m->loc());
    fieldAccess(m->object, m->name, true, m->loc());
  } else if (auto *ix = dyn_cast<IndexExpr>(target)) {
    expr(ix->object);
    expr(ix->index);
    indexAccess(ix, false);
    indexAccess(ix, true);
  } else {
    expr(target);
  }
}

void Analysis::awaitExpr(const AwaitExpr *a) {
  auto *c = dyn_cast<CallExpr>(strip(a->value));
  bool join = c && (c->builtin == Builtin::PromiseAll || c->builtin == Builtin::PromiseAllSettled);
  if (join)
    openJoins_.push_back({a, cur_});
  Use use;
  use.awaited = a;
  expr(a->value, use);
  awaits_[a] = {fi_, caught()};
  if (join) {
    OpenJoin open = std::move(openJoins_.back());
    openJoins_.pop_back();
    merge(open.active, cur_);
    JoinInfo &info = joins_[a];
    info.fn = fi_;
    merge(info.segs, open.active);
    info.settled = c->builtin == Builtin::PromiseAllSettled;
  }
  moveTo(segment(a, SegKind::Await, a->loc()));
}

void Analysis::yieldExpr(const YieldExpr *y) {
  expr(y->value);
  if (y->delegate && y->value)
    iterate(y->value, y);
  if (!fn_->asyncGen)
    return;
  // An async generator gives its value and waits: the loop that iterates
  // it resumes it, inside its own piece. Each step of a `yield*` awaits the
  // other iterator first.
  unsigned seg = segment(y, y->delegate ? SegKind::Await : SegKind::Yield, y->loc());
  fn_->segs[seg].delegate = y->delegate;
  moveTo(seg);
}

void Analysis::call(const CallExpr *c, const Use &use) {
  if (c->builtin != Builtin::None) {
    builtinCall(c, use);
    return;
  }
  if (const FunctionDecl *lib = libraryTarget(c)) {
    libraryCall(c, lib, use);
    return;
  }
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (fn_->thisOwner)
      self = Ref{RefKind::This, fn_->thisOwner};
  } else if (m && (c->method || !c->unionMethods.empty())) {
    expr(m->object);
    self = receiverRef(m->object);
  } else if (!c->func) {
    expr(c->callee); // the function value
  }
  for (const Expr *arg : c->args)
    expr(arg);
  // An async function does not throw: its error rejects its Promise.
  const FunctionDecl *direct = c->func ? c->func->decl : c->method ? c->method->decl : nullptr;
  if (!direct || !direct->isAsync || c->virtualCall || superCtor)
    throwPoint();
  CallRec &rec = record(c, CallKind::Call, use, c->loc());
  rec.call = c;
  if (superCtor) {
    // The constructor of the class extended, then the initializers of the
    // fields.
    if (c->method)
      addCallee(rec, c->method->decl, self);
    for (const ClassDecl *k = fn_->decl ? fn_->decl->cls : nullptr; k; k = k->parent)
      if (k->init)
        addCallee(rec, k->init, self);
    return;
  }
  if (c->method) {
    for (const FunctionDecl *fn : methodTargets(c->method->decl, c->virtualCall)) {
      addCallee(rec, fn, self);
      // A decorator may have put another function in the method's place.
      if (fn->impl && fn->impl->type && !rec.byType)
        rec.byType = fn->impl->type;
    }
    return;
  }
  if (!c->unionMethods.empty()) {
    for (const CallExpr::UnionMethod &um : c->unionMethods) {
      if (!um.method) {
        rec.byType = c->callee->type; // a function in a field
        continue;
      }
      for (const FunctionDecl *fn : methodTargets(um.method->decl, um.virtualCall))
        addCallee(rec, fn, self);
    }
    return;
  }
  if (c->func) {
    addCallee(rec, c->func->decl, closureSelf(c->func->decl, *fn_));
    return;
  }
  // A function value: any function of its type that the program has as a
  // value, and through an object, any method of that name of its classes.
  auto *callee = dyn_cast<NameExpr>(strip(c->callee));
  if (callee && callee->var && resolvers_.contains(callee->var))
    return;
  if (m && (m->name == "resolve" || m->name == "reject") && m->object->type &&
      m->object->type->nonOptional()->isObject() &&
      m->object->type->nonOptional()->field("promise"))
    return; // of Promise.withResolvers()
  rec.byType = c->callee->type;
  if (m) {
    rec.byName = m->name;
    rec.byObject = m->object->type;
    rec.bySelf = receiverRef(m->object);
  }
}

void Analysis::builtinCall(const CallExpr *c, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  const Expr *recv = m ? m->object : nullptr;
  if (recv)
    expr(recv);
  Builtin b = c->builtin;
  // Promise.all and allSettled awaited right away wait there for the tasks
  // given to them.
  Use inner;
  if (b == Builtin::PromiseAll || b == Builtin::PromiseAllSettled)
    inner.join = use.awaited;
  std::optional<CallKind> kind = callbackKind(b);
  bool unknown = !knownBuiltin(b);
  for (size_t i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    bool fn = arg->type && arg->type->nonOptional()->isFunction();
    if (fn && (kind || unknown)) {
      // One the pass does not know may call it now or later.
      if (unknown) {
        functionArg(c, arg, CallKind::Inline, use);
        functionArg(c, arg, CallKind::Later, use);
      } else {
        functionArg(c, arg, *kind, use);
      }
      continue;
    }
    if ((b == Builtin::ResultFromThrowable || b == Builtin::ResultAsyncFromThrowable) && fn &&
        functionRef(arg, {}, c->type)) {
      expr(arg);
      continue;
    }
    expr(arg, i == 0 ? inner : Use());
  }
  if (mayThrow(b))
    throwPoint();
  builtinEffects(c, recv);
}

void Analysis::functionArg(const CallExpr *c, const Expr *arg, CallKind kind, const Use &use) {
  const Expr *f = strip(arg);
  Builtin b = c->builtin;
  // What the callback is given.
  auto *fe = dyn_cast<FunctionExpr>(f);
  if (fe && b == Builtin::PromiseNew)
    for (size_t i = 0; i < fe->fn->params.size() && i < 2; ++i)
      if (fe->fn->params[i].sym)
        resolvers_.insert(fe->fn->params[i].sym);
  bool ofArray = c->callee && isa<MemberExpr>(c->callee) &&
                 cast<MemberExpr>(c->callee)->object->type &&
                 cast<MemberExpr>(c->callee)->object->type->nonOptional()->isArray();
  if (fe && ofArray && (b == Builtin::ArrMap || b == Builtin::ArrForEach) &&
      fe->fn->params.size() > 1 && fe->fn->params[1].sym)
    ownIndex_.insert(fe->fn->params[1].sym);
  if (fe && b == Builtin::ArrMap && use.join && !fe->fn->isAsync)
    joinReturns_[fe->fn] = use.join;

  Use recUse;
  if (b == Builtin::ArrMap && fe && fe->fn->isAsync)
    recUse.join = use.join;
  CallRec &rec = record(f, kind, recUse, f->loc());
  rec.call = c;
  if (kind == CallKind::Later) {
    rec.interval |= b == Builtin::SetInterval;
    rec.handled |= b == Builtin::PromiseThen || b == Builtin::PromiseCatch ||
                   b == Builtin::PromiseFinally || isResultAsyncMethod(b) ||
                   b == Builtin::RlQuestionCallback;
  }
  if (kind == CallKind::Inline && !callsOnce(b))
    rec.repeated = true;
  addFunctionCallees(f, arg, kind);
}

// The functions that `f`, given to be called, may be: (the strip of `arg`).
void Analysis::addFunctionCallees(const Expr *f, const Expr *arg, CallKind kind) {
  CallRec &rec = fn_->calls[fn_->callOf.lookup(std::make_pair((const void *)f, unsigned(kind)))];
  if (auto *fe = dyn_cast<FunctionExpr>(f)) {
    discover(fe->fn);
    addCallee(rec, fe->fn, closureSelf(fe->fn, *fn_));
  } else if (auto *n = dyn_cast<NameExpr>(f); n && n->func) {
    addCallee(rec, n->func->decl, std::nullopt);
  } else if (n && n->var && (n->var->function || n->var->lambda)) {
    const FunctionDecl *decl = n->var->function ? n->var->function : n->var->lambda;
    addCallee(rec, decl, closureSelf(decl, *fn_));
  } else if (n && n->var && resolvers_.contains(n->var)) {
    // It settles a Promise: no code of the program runs.
  } else if (auto *mem = dyn_cast<MemberExpr>(f); mem && mem->method) {
    expr(mem->object);
    CallRec &again =
        fn_->calls[fn_->callOf.lookup(std::make_pair((const void *)f, unsigned(kind)))];
    for (const FunctionDecl *fn : methodTargets(mem->method->decl, mem->virtualCall))
      addCallee(again, fn, receiverRef(mem->object));
  } else {
    expr(arg);
    CallRec &again =
        fn_->calls[fn_->callOf.lookup(std::make_pair((const void *)f, unsigned(kind)))];
    again.byType = arg->type;
  }
}

void Analysis::newExpr(const NewExpr *n) {
  if (n->cls && n->cls->library) {
    libraryNew(n);
    return;
  }
  if (!n->cls) {
    if (n->ctor)
      call(n->ctor, {});
    else
      for (const Expr *arg : n->args)
        expr(arg);
    return;
  }
  noteClass(n->cls);
  for (const Expr *arg : n->ctor ? n->ctor->args : n->args)
    expr(arg);
  throwPoint();
  CallRec &rec = record(n, CallKind::Call, {}, n->loc());
  // Until the constructors give the object away, it is nobody else's: what
  // they do to it is not shared.
  std::optional<Ref> self =
      constructorLeaks(n->cls) ? Ref{RefKind::Site, n} : Ref{RefKind::Local, nullptr};
  if (n->ctor && n->ctor->func) {
    addCallee(rec, n->ctor->func->decl, self);
  } else {
    for (const ClassDecl *k = n->cls; k; k = k->parent)
      if (!k->constructors.empty()) {
        for (const FunctionDecl *ctor : k->constructors)
          addCallee(rec, ctor, self);
        break;
      }
  }
  for (const ClassDecl *k = n->cls; k; k = k->parent)
    if (k->init)
      addCallee(rec, k->init, self);
}

//===----------------------------------------------------------------------===//
// The built-in modules written in tz
//===----------------------------------------------------------------------===//
//
// node:events, tz:http, fetch, URL, streams, EventTarget and AbortController
// are code in tz (src/lib), which the program calls. Their code is not
// walked: what the program sees of it is described here. A call reads what
// it is given and reads or changes the state of the object it is called on
// (a getter only reads it); the functions given to it may run now or later:
//   - serve(options, handler): the handler runs later, for each request, in
//     many copies at once, each a task of its own, whose error the server
//     answers with a 500;
//   - on(), once(), addListener() and the like of an emitter keep a
//     listener, which emit() runs right there, as node does;
//   - addEventListener() and `onabort = f` of an event target or an abort
//     signal keep one, which dispatchEvent() and abort() run right there,
//     and AbortSignal.timeout() later; the state of the signals is one, as
//     an abort is seen by all that wait on them;
//   - the source of a ReadableStream: start() runs as it is made, pull()
//     later and again, cancel() later;
//   - fetch() and the methods of a body give a Promise, settled later,
//     touching nothing of the program but what they are given;
//   - any other function given may run now and later.

bool Analysis::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();
  return llvm::any_of(libRanges_, [&](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  });
}

const FunctionDecl *Analysis::libraryTarget(const CallExpr *c) const {
  if (c->func && isLibrary(c->func->decl))
    return c->func->decl;
  if (c->method && isLibrary(c->method->decl))
    return c->method->decl;
  return nullptr;
}

void Analysis::libraryCall(const CallExpr *c, const FunctionDecl *decl, const Use &use) {
  auto *m = dyn_cast<MemberExpr>(c->callee);
  bool superCtor = c->superCall && !m;
  std::optional<Ref> self;
  if (superCtor) {
    if (fn_->thisOwner)
      self = Ref{RefKind::This, fn_->thisOwner};
  } else if (m && c->method) {
    expr(m->object);
    self = receiverRef(m->object);
  }
  llvm::StringRef cls = libraryClassName(decl->cls);
  llvm::StringRef name = decl->name;
  llvm::SMLoc at = c->loc();
  for (const Expr *arg : c->args)
    libraryArg(c, cls, name, arg, at);
  if (!decl->isAsync)
    throwPoint();
  CallRec &rec = record(c, CallKind::Call, use, at);
  rec.call = c;
  if (superCtor) {
    // The initializers of the fields of the program's classes that extend it.
    for (const ClassDecl *k = fn_->decl ? fn_->decl->cls : nullptr; k; k = k->parent)
      if (k->init)
        addCallee(rec, k->init, self);
  } else if (c->method && c->virtualCall) {
    // The methods of the program's classes that override it.
    for (const FunctionDecl *fn : methodTargets(decl, true))
      addCallee(rec, fn, self);
  }
  if (self && !superCtor)
    libraryState(cls, name, self, decl->accessor != FunctionDecl::Accessor::Getter, at);
  if (isEmitterClass(cls) && name == "emit")
    dispatch(c, CallKind::Inline, EmitterListeners, use);
  else if ((isTargetClass(cls) && name == "dispatchEvent") ||
           (cls == "AbortController" && name == "abort"))
    dispatch(c, CallKind::Inline, TargetListeners, use);
  else if (cls == "AbortSignal" && name == "timeout")
    dispatch(c, CallKind::Later, TargetListeners, use);
}

void Analysis::libraryNew(const NewExpr *n) {
  llvm::SMLoc at = n->loc();
  for (const Expr *arg : n->ctor ? n->ctor->args : n->args)
    libraryArg(n->ctor, libraryClassName(n->cls), "constructor", arg, at);
  throwPoint();
  // A new object, which only this expression has.
}

// What a function of a built-in module does with one of its arguments.
void Analysis::libraryArg(const CallExpr *c, llvm::StringRef cls, llvm::StringRef name,
                          const Expr *arg, llvm::SMLoc at) {
  const Expr *f = strip(arg);
  // A listener is given as a value of any type: `__tz_listener(f)`.
  if (auto *wrap = dyn_cast<CallExpr>(f); wrap && wrap->builtin == Builtin::LibListener &&
                                          !wrap->args.empty())
    f = strip(wrap->args[0]);
  bool fn = f->type && f->type->nonOptional()->isFunction();
  if (fn) {
    if (isEmitterClass(cls) && (name == "on" || name == "addListener" || name == "once" ||
                                name == "prependListener" || name == "prependOnceListener"))
      listen(f, EmitterListeners);
    else if (isTargetClass(cls) && (name == "addEventListener" || name == "addFunctionListener" ||
                                    name == "setOnabort"))
      listen(f, TargetListeners);
    else if (cls.empty() && name == "serve")
      libraryCallback(c, f, CallKind::Later, /*interval=*/true, /*handled=*/true);
    else {
      libraryCallback(c, f, CallKind::Inline);
      libraryCallback(c, f, CallKind::Later);
    }
    return;
  }
  // The functions of an object literal given: the source of a stream, the
  // options of a server.
  if (auto *lit = dyn_cast<ObjectLitExpr>(f)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Expr *v = strip(p.value);
      if (p.spread || !v || !v->type || !v->type->nonOptional()->isFunction()) {
        expr(p.value);
        libraryReads(v, at);
        continue;
      }
      if (cls == "ReadableStream" && p.name == "start") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (cls == "ReadableStream" && p.name == "pull") {
        libraryCallback(c, v, CallKind::Later, /*interval=*/true);
      } else if (cls == "ReadableStream" && p.name == "cancel") {
        libraryCallback(c, v, CallKind::Later);
      } else if (cls.empty() && name == "serve" && p.name == "onListen") {
        libraryCallback(c, v, CallKind::Inline);
      } else if (isTargetClass(cls) &&
                 (name == "addEventListener" || name == "addObjectListener") &&
                 p.name == "handleEvent") {
        listen(v, TargetListeners);
      } else {
        libraryCallback(c, v, CallKind::Inline);
        libraryCallback(c, v, CallKind::Later);
      }
    }
    return;
  }
  expr(arg);
  libraryReads(f, at);
}

// What a function of a built-in module reads of a value it is given: the
// state of an object of one of their classes, or else all it holds.
void Analysis::libraryReads(const Expr *e, llvm::SMLoc at) {
  const Type *type = e && e->type ? e->type->nonOptional() : nullptr;
  if (const ClassDecl *cls = type ? type->classDecl() : nullptr; cls && cls->library) {
    libraryState(libraryClassName(cls), "", refOf(e), false, at);
    return;
  }
  contents(e, false, at);
}

void Analysis::libraryCallback(const CallExpr *c, const Expr *f, CallKind kind, bool interval,
                               bool handled) {
  CallRec &rec = record(f, kind, {}, f->loc());
  rec.call = c;
  rec.interval |= interval;
  rec.handled |= handled;
  if (kind == CallKind::Inline)
    rec.repeated = true;
  addFunctionCallees(f, f, kind);
}

// A listener kept, which a call of `channel` runs.
void Analysis::listen(const Expr *f, Channel channel) {
  f = strip(f);
  expr(f); // a function of the program as a value
  const FunctionDecl *decl = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(f))
    decl = fe->fn;
  else if (auto *n = dyn_cast<NameExpr>(f); n && n->func)
    decl = n->func->decl;
  else if (n && n->var && (n->var->function || n->var->lambda))
    decl = n->var->function ? n->var->function : n->var->lambda;
  if (decl && decl->body && !isLibrary(decl)) {
    unsigned fi = discover(decl);
    if (!llvm::is_contained(listeners_[channel], fi))
      listeners_[channel].push_back(fi);
    return;
  }
  if (f->type && f->type->nonOptional()->isFunction() &&
      !llvm::is_contained(listenerTypes_[channel], f->type->nonOptional()))
    listenerTypes_[channel].push_back(f->type->nonOptional());
}

void Analysis::dispatch(const CallExpr *c, CallKind kind, Channel channel, const Use &use) {
  (void)use;
  CallRec &rec = record(c, kind, {}, c->loc());
  rec.call = c;
  rec.channel = channel;
}

// What a call of the method `name` of a class of a built-in module touches
// of the object it is called on.
void Analysis::libraryState(llvm::StringRef cls, llvm::StringRef name, std::optional<Ref> self,
                            bool write, llvm::SMLoc at) {
  if (isSignalClass(cls)) {
    resource(Res::Signals, write, at);
    return;
  }
  if (!self)
    return;
  if (isEmitterClass(cls) || isTargetClass(cls)) {
    bool reads = name == "emit" || name == "dispatchEvent" || name == "listenerCount" ||
                 name == "listeners" || name == "rawListeners" || name == "eventNames" ||
                 name == "getMaxListeners";
    touch(LocKind::Field, *self, listenersKey_, write && !reads, at);
    return;
  }
  touch(LocKind::Field, *self, state_, write, at);
}

// `o.x = v` of a setter of a class of a built-in module: its call. A
// function assigned to `onabort` (or another `on...`) is a listener.
bool Analysis::librarySetter(const MemberExpr *m, const Expr *value) {
  const Type *type = m->object->type ? m->object->type->nonOptional() : nullptr;
  const ClassDecl *cls = type ? type->classDecl() : nullptr;
  if (!cls || !cls->library)
    return false;
  bool setter = false;
  for (const ClassDecl *k = cls; k && !setter; k = k->parent)
    for (const FunctionDecl *fn : k->methods)
      setter |= fn->name == m->name && fn->accessor == FunctionDecl::Accessor::Setter;
  if (!setter)
    return false;
  if (llvm::StringRef(m->name).starts_with("on") && value && value->type &&
      value->type->nonOptional()->isFunction())
    listen(value, TargetListeners);
  libraryState(libraryClassName(cls), m->name, receiverRef(m->object), true, m->loc());
  return true;
}

bool knownBuiltin(Builtin b) {
  // Families that compute values, and touch data only as builtinEffects
  // says.
  if (b == Builtin::None || isMathBuiltin(b) || isDecimalBuiltin(b) || isIntlBuiltin(b) ||
      isResultBuiltin(b) || isDateBuiltin(b) || isRegexBuiltin(b) || isJsonBuiltin(b) ||
      isDictBuiltin(b) || isBytesBuiltin(b) || isMetaBuiltin(b))
    return true;
  // console, process, numbers, strings, arrays, objects and iterators.
  if (b >= Builtin::ConsoleLog && b <= Builtin::IterFind)
    return true;
  // The modules of node and the async ones by name, and those since: a new
  // built-in is not known until it is added here (the built-in modules
  // written in tz, such as the server of tz:http, are described apart: see
  // libraryCall).
  switch (b) {
  case Builtin::ErrorStack:
  case Builtin::ErrorToString:
  case Builtin::TupleJoin:
  case Builtin::PathJoin:
  case Builtin::PathResolve:
  case Builtin::PathNormalize:
  case Builtin::PathIsAbsolute:
  case Builtin::PathDirname:
  case Builtin::PathBasename:
  case Builtin::PathExtname:
  case Builtin::PathRelative:
  case Builtin::PathParse:
  case Builtin::PathFormat:
  case Builtin::PathToNamespacedPath:
  case Builtin::OsString:
  case Builtin::OsNumber:
  case Builtin::OsAvailableParallelism:
  case Builtin::OsCpus:
  case Builtin::OsLoadavg:
  case Builtin::OsUserInfo:
  case Builtin::ReadLine:
  case Builtin::FsReadFile:
  case Builtin::FsWriteFile:
  case Builtin::FsExists:
  case Builtin::FsAccess:
  case Builtin::FsChmod:
  case Builtin::FsTruncate:
  case Builtin::FsStat:
  case Builtin::FsReaddir:
  case Builtin::FsMkdir:
  case Builtin::FsMkdtemp:
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
  case Builtin::FsRm:
  case Builtin::FsRename:
  case Builtin::FsCopyFile:
  case Builtin::FsRealpath:
  case Builtin::FsIsKind:
  case Builtin::FsStatsDate:
  case Builtin::FspReadFile:
  case Builtin::FspWriteFile:
  case Builtin::FspAccess:
  case Builtin::FspChmod:
  case Builtin::FspTruncate:
  case Builtin::FspStat:
  case Builtin::FspReaddir:
  case Builtin::FspMkdir:
  case Builtin::FspMkdtemp:
  case Builtin::FspRmdir:
  case Builtin::FspUnlink:
  case Builtin::FspRm:
  case Builtin::FspRename:
  case Builtin::FspCopyFile:
  case Builtin::FspRealpath:
  case Builtin::FspOpen:
  case Builtin::FhFd:
  case Builtin::FhClose:
  case Builtin::FhReadFile:
  case Builtin::FhWriteFile:
  case Builtin::FhWrite:
  case Builtin::FhStat:
  case Builtin::FhTruncate:
  case Builtin::FhSync:
  case Builtin::RlCreate:
  case Builtin::RlQuestion:
  case Builtin::RlQuestionCallback:
  case Builtin::RlClose:
  case Builtin::PromiseNew:
  case Builtin::PromiseThen:
  case Builtin::PromiseCatch:
  case Builtin::PromiseFinally:
  case Builtin::PromiseResolve:
  case Builtin::PromiseReject:
  case Builtin::PromiseAll:
  case Builtin::PromiseAllSettled:
  case Builtin::PromiseRace:
  case Builtin::PromiseAny:
  case Builtin::PromiseWithResolvers:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::TimerRef:
  case Builtin::TimerUnref:
  case Builtin::TimerHasRef:
  case Builtin::TimerRefresh:
  case Builtin::ResultAsyncFromPromise:
  case Builtin::ResultAsyncFromSafePromise:
  case Builtin::ResultAsyncFromThrowable:
  case Builtin::ResultAsyncMap:
  case Builtin::ResultAsyncMapErr:
  case Builtin::ResultAsyncAndThen:
  case Builtin::ResultAsyncOrElse:
  case Builtin::ResultAsyncMatch:
  case Builtin::ResultAsyncUnwrapOr:
  case Builtin::ResultAsyncAndTee:
  case Builtin::ResultAsyncOrTee:
  case Builtin::ResultAsyncWrapOk:
  case Builtin::ResultAsyncWrapErr:
  case Builtin::ResultAsyncTeeDone:
  case Builtin::TimersSetTimeout:
  case Builtin::TimersSetImmediate:
  case Builtin::TimersSetInterval:
  case Builtin::WeakRefNew:
  case Builtin::WeakRefDeref:
  case Builtin::ChildSpawnSync:
  case Builtin::ChildExecFileSync:
  case Builtin::ChildExecSync:
  case Builtin::Crypto:
  case Builtin::CryptoKeyPair:
  case Builtin::LibListener:
  case Builtin::LibAs:
  case Builtin::LibNativeSignal:
  case Builtin::ClassName:
  case Builtin::ReflectGet:
  case Builtin::ReflectApply:
  case Builtin::ReflectConstruct:
  case Builtin::FromJsonClass:
    return true;
  default:
    return false;
  }
}

// What a built-in reads and writes, once its arguments are evaluated.
void Analysis::builtinEffects(const CallExpr *c, const Expr *recv) {
  llvm::SMLoc at = c->loc();
  Builtin b = c->builtin;
  auto arg = [&](size_t i) -> const Expr * { return i < c->args.size() ? c->args[i] : nullptr; };
  auto readArgs = [&] {
    for (const Expr *a : c->args)
      contents(a, false, at);
  };
  auto field = [&](const Expr *object, llvm::StringRef name, bool write) {
    if (object && holdsReferences(object->type))
      touch(LocKind::Field, refOf(object), intern(name), write, at);
  };
  switch (b) {
  case Builtin::ConsoleLog:
  case Builtin::ProcessStdoutWrite:
    readArgs();
    resource(Res::Stdout, true, at);
    return;
  case Builtin::ConsoleError:
  case Builtin::ProcessStderrWrite:
    readArgs();
    resource(Res::Stderr, true, at);
    return;
  case Builtin::ProcessExit:
    // Ends the program: after what was printed, before what was not.
    resource(Res::Process, true, at);
    resource(Res::Stdout, true, at);
    resource(Res::Stderr, true, at);
    return;
  case Builtin::ProcessExitCode:
  case Builtin::ProcessEnv:
  case Builtin::EnvMap:
    resource(Res::Process, false, at);
    return;
  case Builtin::ProcessSetExitCode:
    resource(Res::Process, true, at);
    return;
  case Builtin::ReadLine:
  case Builtin::RlQuestion:
  case Builtin::RlQuestionCallback:
    // Takes the next line of input, after writing the question.
    resource(Res::Stdin, true, at);
    resource(Res::Stdout, true, at);
    return;
  case Builtin::RlClose:
    resource(Res::Stdin, true, at);
    return;
  case Builtin::StringOf:
  case Builtin::JsonStringify:
    readArgs();
    return;
  case Builtin::TupleJoin:
    field(recv, "*", false);
    return;
  // Arrays.
  case Builtin::ArrPush:
  case Builtin::ArrPop:
  case Builtin::ArrShift:
  case Builtin::ArrUnshift:
  case Builtin::ArrSplice:
    elements(recv, true, at);
    if (recv && holdsReferences(recv->type))
      touch(LocKind::Field, refOf(recv), length_, true, at);
    return;
  case Builtin::ArrReverse:
  case Builtin::ArrFill:
  case Builtin::ArrSort:
  case Builtin::ArrSortWith:
    elements(recv, true, at);
    return;
  case Builtin::ArrJoin:
  case Builtin::ArrToString:
    contents(recv, false, at);
    return;
  case Builtin::ArrConcat:
    elements(recv, false, at);
    for (const Expr *a : c->args)
      elements(a, false, at);
    return;
  case Builtin::ArrAt:
  case Builtin::ArrSlice:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrIncludes:
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
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
    elements(recv, false, at);
    return;
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
    if (const Expr *a = arg(0); a && a->type && a->type->nonOptional()->isObject())
      field(a, "*", false);
    else
      elements(a, false, at);
    return;
  // Iterators: a step moves an iterator on, and a generator runs.
  case Builtin::IterNext:
    if (recv)
      iterate(recv, c);
    return;
  case Builtin::IterFrom:
  case Builtin::ArrayFrom:
    if (const Expr *a = arg(0);
        a && a->type &&
        (a->type->nonOptional()->isIterator() || a->type->nonOptional()->isAsyncIterator()))
      iterate(a, c);
    else
      contents(arg(0), false, at);
    return;
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
    if (recv)
      iterate(recv, c);
    return;
  // Dictionaries.
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapSize:
  case Builtin::MapForEach:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
    elements(recv, false, at);
    return;
  case Builtin::MapSet:
  case Builtin::MapDelete:
  case Builtin::MapClear:
  case Builtin::SetAdd:
  case Builtin::RecordSet:
    elements(recv, true, at);
    return;
  case Builtin::RecordNullishSet:
    elements(recv, false, at);
    elements(recv, true, at);
    return;
  case Builtin::SetUnion:
  case Builtin::SetIntersection:
  case Builtin::SetDifference:
  case Builtin::SetSymmetricDifference:
  case Builtin::SetIsSubsetOf:
  case Builtin::SetIsSupersetOf:
  case Builtin::SetIsDisjointFrom:
    elements(recv, false, at);
    elements(arg(0), false, at);
    return;
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
    contents(arg(0), false, at);
    return;
  // The files, with fd 0 for input and 1 and 2 for output.
  case Builtin::FsReadFile:
    if (const Expr *a = arg(0); a && a->type && a->type->isInteger())
      resource(Res::Stdin, true, at);
    resource(Res::Files, false, at);
    return;
  case Builtin::FsWriteFile:
    if (const Expr *a = arg(0); a && a->type && a->type->isInteger()) {
      resource(Res::Stdout, true, at);
      resource(Res::Stderr, true, at);
    }
    contents(arg(1), false, at);
    resource(Res::Files, true, at);
    return;
  case Builtin::FsExists:
  case Builtin::FsAccess:
  case Builtin::FsStat:
  case Builtin::FsReaddir:
  case Builtin::FsRealpath:
    resource(Res::Files, false, at);
    return;
  case Builtin::FsChmod:
  case Builtin::FsTruncate:
  case Builtin::FsMkdir:
  case Builtin::FsMkdtemp:
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
  case Builtin::FsRm:
  case Builtin::FsRename:
  case Builtin::FsCopyFile:
    resource(Res::Files, true, at);
    return;
  // Regular expressions keep their lastIndex.
  case Builtin::RegExpTest:
  case Builtin::RegExpExec:
    field(recv, "lastIndex", true);
    return;
  case Builtin::StrMatch:
  case Builtin::StrMatchGlobal:
  case Builtin::StrMatchAll:
  case Builtin::StrSearch:
  case Builtin::StrReplaceRegex:
  case Builtin::StrReplaceRegexFn:
  case Builtin::StrSplitRegex:
    field(arg(0), "lastIndex", true);
    return;
  // Dates hold their time.
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal:
    field(recv, "%time", true);
    return;
  case Builtin::DateGetTime:
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal:
  case Builtin::DateToISOString:
  case Builtin::DateToJSON:
  case Builtin::DateToUTCString:
  case Builtin::DateToLocalString:
  case Builtin::DateToLocale:
    field(recv, "%time", false);
    return;
  case Builtin::DateCopy:
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts:
    field(arg(0), "%time", false);
    return;
  // Binary data.
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BytesOf:
  case Builtin::BufferConcat:
    contents(arg(0), false, at);
    return;
  case Builtin::BytesAt:
  case Builtin::BytesIndexOf:
  case Builtin::BytesJoin:
  case Builtin::BytesSlice:
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted:
  case Builtin::BytesWith:
  case Builtin::ArrayBufferSlice:
  case Builtin::DataViewGet:
  case Builtin::BufferToString:
  case Builtin::BufferIndexOf:
  case Builtin::BufferRead:
  case Builtin::BufferReadVar:
  case Builtin::BufferToJSON:
  case Builtin::BufferEquals:
  case Builtin::BufferCompareTo:
    elements(recv, false, at);
    for (const Expr *a : c->args)
      elements(a, false, at);
    return;
  case Builtin::BufferCompare:
    elements(arg(0), false, at);
    elements(arg(1), false, at);
    return;
  case Builtin::BytesReverse:
  case Builtin::BytesSort:
  case Builtin::BytesFill:
  case Builtin::BytesCopyWithin:
  case Builtin::DataViewSet:
  case Builtin::BufferWrite:
  case Builtin::BufferFill:
  case Builtin::BufferWriteNumber:
  case Builtin::BufferWriteVar:
  case Builtin::BufferSwap:
    elements(recv, true, at);
    return;
  case Builtin::BytesSet:
    elements(recv, true, at);
    contents(arg(0), false, at);
    return;
  case Builtin::BufferCopy:
    elements(recv, false, at);
    elements(arg(0), true, at);
    return;
  case Builtin::TextEncodeInto:
    elements(arg(1), true, at);
    return;
  case Builtin::TextDecode:
    elements(arg(0), false, at);
    return;
  // Metadata.
  case Builtin::MetaDefine:
  case Builtin::MetaDelete:
    resource(Res::Metadata, true, at);
    return;
  // A child process of child_process: what it does, the program cannot see
  // but in the files, the output it shares and the environment.
  case Builtin::ChildSpawnSync:
  case Builtin::ChildExecFileSync:
  case Builtin::ChildExecSync:
    readArgs();
    resource(Res::Files, true, at);
    resource(Res::Stdout, true, at);
    resource(Res::Stderr, true, at);
    resource(Res::Process, false, at);
    return;
  // node:crypto, by the kinds of the arguments of its function (sema/
  // crypto_api.h): an object of node:crypto changes as it is used (update,
  // final, digest), the bytes that randomFill and getRandomValues fill are
  // written, and the data given read.
  case Builtin::Crypto: {
    auto *number = dyn_cast_or_null<IntLitExpr>(strip(arg(0)));
    if (!number || number->value >= std::size(kCryptoFunctions)) {
      readArgs();
      return;
    }
    auto fn = CryptoFn(unsigned(number->value));
    llvm::StringRef kinds = kCryptoFunctions[size_t(number->value)].params;
    bool fills = fn == CryptoFn::RandomFill || fn == CryptoFn::GetRandomValues;
    for (size_t i = 1; i < c->args.size(); ++i) {
      const Expr *a = c->args[i];
      char kind = i - 1 < kinds.size() ? kinds[i - 1] : 'd';
      if (!a || !holdsReferences(a->type))
        continue;
      bool bytes = a->type->nonOptional()->isBytes();
      if (kind == 'o' && bytes)
        elements(a, fills, at);
      else if (kind == 'o')
        touch(LocKind::Field, refOf(a), state_, true, at);
      else if (bytes)
        elements(a, false, at);
      else
        contents(a, false, at);
    }
    return;
  }
  case Builtin::CryptoKeyPair:
    readArgs();
    return;
  // Reflection: the fields read by a name known only when the program runs,
  // and the method or the constructor it runs (Analysis::resolve).
  case Builtin::ReflectGet:
    if (const Expr *a = arg(0); a && holdsReferences(a->type))
      touch(LocKind::Field, refOf(a), star_, false, at);
    return;
  case Builtin::ReflectApply: {
    for (size_t i = 2; i < c->args.size(); ++i)
      contents(c->args[i], false, at);
    CallRec &rec = record(c, CallKind::Call, {}, at);
    rec.call = c;
    const Expr *fn = strip(arg(0));
    if (fn && fn->type && fn->type->nonOptional()->isFunction()) {
      rec.byType = fn->type; // a function the program has as a value
      return;
    }
    // A method that Reflect.get found by name: one of the classes of the
    // object it is applied to.
    rec.reflect = CallRec::Reflect::Apply;
    const Expr *self = strip(arg(1));
    rec.bySelf = self ? std::optional<Ref>(receiverRef(self)) : std::nullopt;
    rec.byObject = self ? self->type : nullptr;
    return;
  }
  case Builtin::ReflectConstruct:
  case Builtin::FromJsonClass: {
    for (size_t i = 1; i < c->args.size(); ++i)
      contents(c->args[i], false, at);
    CallRec &rec = record(c, CallKind::Call, {}, at);
    rec.call = c;
    rec.reflect = CallRec::Reflect::Construct;
    return;
  }
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn:
  case Builtin::MetaHas:
  case Builtin::MetaHasOwn:
    resource(Res::Metadata, false, at);
    return;
  default:
    break;
  }
  if (knownBuiltin(b))
    return; // numbers, strings, paths, promises, timers...: nothing shared
  // A built-in the pass does not know may change what it is given.
  contents(recv, true, at);
  for (const Expr *a : c->args)
    contents(a, true, at);
  resource(Res::Runtime, true, at);
}

//===----------------------------------------------------------------------===//
// What objects are
//===----------------------------------------------------------------------===//

bool Analysis::isAllocation(const Expr *e) const {
  e = strip(e);
  if (!e)
    return false;
  // A function of a built-in module makes what it gives (fetch() a
  // Response, serve() a Server), and so does an async method (text() of a
  // body, read() of a reader); a getter or another method may give an
  // object it has.
  const Expr *made = e;
  if (auto *a = dyn_cast<AwaitExpr>(e))
    made = strip(a->value);
  if (auto *c = dyn_cast_or_null<CallExpr>(made))
    if (const FunctionDecl *lib = libraryTarget(c))
      return made == e ? !lib->cls : lib->isAsync;
  if (isa<ArrayLitExpr, ObjectLitExpr, RegexLitExpr>(e))
    return true;
  if (auto *n = dyn_cast<NewExpr>(e))
    return n->cls || (n->type && n->type->isArray());
  if (auto *c = dyn_cast<CallExpr>(e)) {
    if (makesNewObject(c->builtin))
      return true;
    // A function of node:crypto that gives a reference makes the object it
    // gives: a hash, a cipher, the bytes of a digest.
    if (c->builtin == Builtin::Crypto && !c->args.empty())
      if (auto *number = dyn_cast<IntLitExpr>(strip(c->args[0]));
          number && number->value < std::size(kCryptoFunctions)) {
        char result = kCryptoFunctions[size_t(number->value)].result;
        return result == 'o' || result == 'p';
      }
    // A generator called makes a new iterator.
    const FunctionDecl *fn = c->func ? c->func->decl : c->method ? c->method->decl : nullptr;
    return fn && fn->isGenerator && !fn->safeTry;
  }
  return false;
}

Ref Analysis::refOf(const Expr *e) {
  e = strip(e);
  if (!e || !e->type)
    return Ref{RefKind::Type, nullptr};
  if (auto *n = dyn_cast<NameExpr>(e); n && n->var) {
    const VarSymbol *v = n->var;
    if (v->owner && v->owner->thisVar == v)
      return Ref{RefKind::This, v->owner};
    auto it = declInit_.find(v);
    if (it != declInit_.end() && !v->reassigned && isAllocation(it->second)) {
      if (isLocal(v))
        return Ref{RefKind::Local, nullptr};
      return Ref{RefKind::Site, it->second};
    }
  }
  // An object made right here, which only this expression has.
  if (isAllocation(e))
    return Ref{RefKind::Local, nullptr};
  return Ref{RefKind::Type, e->type->nonOptional()};
}

// The object a method is called on: one made right here may still be kept
// by the method.
Ref Analysis::receiverRef(const Expr *e) {
  const Expr *object = strip(e);
  if (object && isAllocation(object))
    return Ref{RefKind::Site, object};
  return refOf(e);
}

// A variable that holds an object its function made, which never leaves the
// function: no other code can touch the object.
bool Analysis::isLocal(const VarSymbol *v) {
  if (v->isGlobal || v->captured || v->reassigned || !holdsReferences(v->type))
    return false;
  auto it = declInit_.find(v);
  if (it == declInit_.end() || !isAllocation(it->second))
    return false;
  if (auto *n = dyn_cast<NewExpr>(it->second); n && n->cls && constructorLeaks(n->cls))
    return false;
  return !escapes(v->owner).contains(v);
}

const Analysis::Escapes &Analysis::escapes(const FunctionDecl *owner) {
  auto found = escapes_.find(owner);
  if (found != escapes_.end())
    return found->second;
  Escapes out;
  if (owner) {
    for (const Param &p : owner->params)
      scanUses(p.defaultValue, false, out);
    scanStmt(owner->body, out);
  } else {
    for (const Stmt *item : program_.items)
      if (!isa<FunctionDecl, ClassDecl>(item))
        scanStmt(item, out);
  }
  return escapes_[owner] = std::move(out);
}

void Analysis::scanStmt(const Stmt *s, Escapes &out) {
  if (!s)
    return;
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *x : cast<BlockStmt>(s)->body)
      scanStmt(x, out);
    return;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(s)->decls)
      scanUses(d.init, false, out);
    return;
  case Stmt::Kind::Expr:
    scanUses(cast<ExprStmt>(s)->expr, true, out, /*discarded=*/true);
    return;
  case Stmt::Kind::If:
    scanUses(cast<IfStmt>(s)->cond, true, out);
    scanStmt(cast<IfStmt>(s)->thenStmt, out);
    scanStmt(cast<IfStmt>(s)->elseStmt, out);
    return;
  case Stmt::Kind::While:
    scanUses(cast<WhileStmt>(s)->cond, true, out);
    scanStmt(cast<WhileStmt>(s)->body, out);
    return;
  case Stmt::Kind::DoWhile:
    scanStmt(cast<DoWhileStmt>(s)->body, out);
    scanUses(cast<DoWhileStmt>(s)->cond, true, out);
    return;
  case Stmt::Kind::For: {
    auto *f = cast<ForStmt>(s);
    scanStmt(f->init, out);
    scanUses(f->cond, true, out);
    scanUses(f->step, true, out, true);
    scanStmt(f->body, out);
    return;
  }
  case Stmt::Kind::ForOf:
    scanUses(cast<ForOfStmt>(s)->iterable, true, out);
    scanStmt(cast<ForOfStmt>(s)->body, out);
    return;
  case Stmt::Kind::Switch:
    scanUses(cast<SwitchStmt>(s)->discriminant, true, out);
    for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
      scanUses(c.test, true, out);
      for (const Stmt *x : c.body)
        scanStmt(x, out);
    }
    return;
  case Stmt::Kind::Labeled:
    scanStmt(cast<LabeledStmt>(s)->body, out);
    return;
  case Stmt::Kind::Return:
    scanUses(cast<ReturnStmt>(s)->value, false, out);
    return;
  case Stmt::Kind::Throw:
    scanUses(cast<ThrowStmt>(s)->value, false, out);
    return;
  case Stmt::Kind::Try:
    scanStmt(cast<TryStmt>(s)->block, out);
    scanStmt(cast<TryStmt>(s)->handler, out);
    scanStmt(cast<TryStmt>(s)->finalizer, out);
    return;
  default:
    return;
  }
}

// The built-ins that read or change their receiver and give back no reference
// to it, unless their result is dropped.
bool keepsReceiver(Builtin b, bool discarded) {
  switch (b) {
  case Builtin::ArrReverse:
  case Builtin::ArrFill:
  case Builtin::ArrSort:
  case Builtin::ArrSortWith:
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::BytesReverse:
  case Builtin::BytesSort:
  case Builtin::BytesFill:
  case Builtin::BytesCopyWithin:
  case Builtin::BufferFill:
  case Builtin::BufferSwap:
    return discarded;
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
  case Builtin::BytesSubarray:
  case Builtin::BytesBuffer:
  case Builtin::BytesView:
    return false;
  default:
    return knownBuiltin(b);
  }
}

// The built-ins that only read their arguments, or copy what they hold.
bool onlyReadsArgs(Builtin b) {
  switch (b) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::StringOf:
  case Builtin::JsonStringify:
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
  case Builtin::ArrayIsArray:
  case Builtin::ArrIncludes:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrConcat:
  case Builtin::ArrayFrom:
  case Builtin::MapHas:
  case Builtin::MapGet:
  case Builtin::DictNew:
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BufferConcat:
  case Builtin::BufferCompare:
  case Builtin::BufferEquals:
  case Builtin::BufferIsBuffer:
  case Builtin::ProcessStdoutWrite:
  case Builtin::ProcessStderrWrite:
    return true;
  default:
    return false;
  }
}

// Finds the variables whose value may leave the function: given to a call,
// stored, returned, captured. `safe` says that the expression is in a place
// that only reads or changes the object it gives (`x.f`, `x[i]`, `x.push(v)`,
// console.log(x)).
void Analysis::scanUses(const Expr *e, bool safe, Escapes &out, bool discarded) {
  if (!e)
    return;
  switch (e->kind()) {
  case Expr::Kind::Name:
    if (!safe && cast<NameExpr>(e)->var)
      out.insert(cast<NameExpr>(e)->var);
    return;
  case Expr::Kind::Member: {
    auto *m = cast<MemberExpr>(e);
    scanUses(m->object, !m->method, out);
    return;
  }
  case Expr::Kind::Index:
    scanUses(cast<IndexExpr>(e)->object, true, out);
    scanUses(cast<IndexExpr>(e)->index, false, out);
    return;
  case Expr::Kind::Cast:
    scanUses(cast<CastExpr>(e)->operand, cast<CastExpr>(e)->isImplicit() && safe, out, discarded);
    return;
  case Expr::Kind::Unary: {
    auto *u = cast<UnaryExpr>(e);
    bool reads = u->op != UnaryOp::NonNull;
    scanUses(u->operand, reads || safe, out, discarded && !reads);
    return;
  }
  case Expr::Kind::Binary: {
    auto *b = cast<BinaryExpr>(e);
    bool flows =
        b->op == BinaryOp::LogAnd || b->op == BinaryOp::LogOr || b->op == BinaryOp::Nullish;
    scanUses(b->lhs, flows ? safe : true, out);
    scanUses(b->rhs, flows ? safe : true, out);
    return;
  }
  case Expr::Kind::Assign: {
    auto *a = cast<AssignExpr>(e);
    if (auto *m = dyn_cast<MemberExpr>(a->target)) {
      scanUses(m->object, true, out);
    } else if (auto *ix = dyn_cast<IndexExpr>(a->target)) {
      scanUses(ix->object, true, out);
      scanUses(ix->index, false, out);
    }
    scanUses(a->value, false, out);
    return;
  }
  case Expr::Kind::Update: {
    const Expr *t = cast<UpdateExpr>(e)->target;
    if (auto *m = dyn_cast<MemberExpr>(t))
      scanUses(m->object, true, out);
    else if (auto *ix = dyn_cast<IndexExpr>(t)) {
      scanUses(ix->object, true, out);
      scanUses(ix->index, false, out);
    }
    return;
  }
  case Expr::Kind::Call: {
    auto *c = cast<CallExpr>(e);
    auto *m = dyn_cast<MemberExpr>(c->callee);
    if (c->builtin != Builtin::None) {
      if (m)
        scanUses(m->object, keepsReceiver(c->builtin, discarded), out);
      bool reads = onlyReadsArgs(c->builtin);
      for (const Expr *arg : c->args)
        scanUses(arg, reads, out);
      return;
    }
    if (m && c->method && isLibrary(c->method->decl))
      scanUses(m->object, true, out); // a built-in module keeps it from the program
    else if (m && (c->method || !c->unionMethods.empty()))
      scanUses(m->object, false, out); // a method may keep `this`
    else
      scanUses(c->callee, true, out);
    for (const Expr *arg : c->args)
      scanUses(arg, false, out);
    return;
  }
  case Expr::Kind::New: {
    auto *n = cast<NewExpr>(e);
    for (const Expr *arg : n->ctor ? n->ctor->args : n->args)
      scanUses(arg, false, out);
    return;
  }
  case Expr::Kind::Conditional:
    scanUses(cast<ConditionalExpr>(e)->cond, true, out);
    scanUses(cast<ConditionalExpr>(e)->thenExpr, safe, out);
    scanUses(cast<ConditionalExpr>(e)->elseExpr, safe, out);
    return;
  case Expr::Kind::Sequence: {
    auto *s = cast<SequenceExpr>(e);
    for (size_t i = 0; i < s->exprs.size(); ++i)
      scanUses(s->exprs[i], i + 1 < s->exprs.size() || safe, out);
    return;
  }
  case Expr::Kind::OptionalChain:
    scanUses(cast<OptionalChainExpr>(e)->chain, safe, out, discarded);
    return;
  case Expr::Kind::Template:
    for (const Expr *x : cast<TemplateExpr>(e)->exprs)
      scanUses(x, true, out);
    return;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &el : cast<ArrayLitExpr>(e)->elements)
      scanUses(el.expr, el.spread, out);
    return;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(e)->props) {
      scanUses(p.key, true, out);
      if (!p.method)
        scanUses(p.value, p.spread, out);
    }
    return;
  case Expr::Kind::Await:
    scanUses(cast<AwaitExpr>(e)->value, false, out);
    return;
  case Expr::Kind::Yield:
    scanUses(cast<YieldExpr>(e)->value, false, out);
    return;
  default:
    return;
  }
}

// Whether a constructor of the class (or of one up its chain) may give
// `this` away before the object is done: then what it writes may be seen.
bool Analysis::constructorLeaks(const ClassDecl *cls) {
  auto found = leaks_.find(cls);
  if (found != leaks_.end())
    return found->second;
  leaks_[cls] = true; // a cycle of classes is an error anyway
  bool leaks = false;
  for (const ClassDecl *k = cls; k && !leaks; k = k->parent) {
    std::vector<const FunctionDecl *> fns(k->constructors.begin(), k->constructors.end());
    if (k->init)
      fns.push_back(k->init);
    for (const FunctionDecl *fn : fns) {
      if (!fn->thisVar)
        continue;
      if (fn->thisVar->captured || escapes(fn).contains(fn->thisVar)) {
        leaks = true;
        break;
      }
    }
  }
  return leaks_[cls] = leaks;
}

const Type *Analysis::typeOfRef(Ref r) const {
  switch (r.kind) {
  case RefKind::Site: {
    const Type *t = static_cast<const Expr *>(r.ptr)->type;
    return t ? t->nonOptional() : nullptr;
  }
  case RefKind::Type:
    return static_cast<const Type *>(r.ptr);
  case RefKind::This: {
    auto *owner = static_cast<const FunctionDecl *>(r.ptr);
    return owner && owner->cls ? owner->cls->type : nullptr;
  }
  case RefKind::Local:
    return nullptr;
  }
  return nullptr;
}

bool Analysis::sameObject(Ref a, Ref b) {
  if (a.kind == RefKind::Site && b.kind == RefKind::Site)
    return a.ptr == b.ptr;
  return typesMayAlias(typeOfRef(a), typeOfRef(b));
}

bool Analysis::typesMayAlias(const Type *a, const Type *b) {
  if (!a || !b)
    return true;
  a = a->nonOptional();
  b = b->nonOptional();
  if (a == b)
    return true;
  auto key = std::make_pair(std::min(a, b, std::less<const Type *>()),
                            std::max(a, b, std::less<const Type *>()));
  auto found = alias_.find(key);
  if (found != alias_.end())
    return found->second;
  bool result = false;
  auto any = [](const Type *t) { return t->isUnknown() || t->isAnyObject() || t->isParam(); };
  if (any(a) || any(b)) {
    result = holdsReferences(a) && holdsReferences(b);
  } else if (a->isUnion() || b->isUnion()) {
    const Type *u = a->isUnion() ? a : b, *other = a->isUnion() ? b : a;
    alias_[key] = false;
    for (const Type *m : u->members())
      if (typesMayAlias(m, other)) {
        result = true;
        break;
      }
  } else if (a->isArray() && b->isArray()) {
    // A Dog[] may be seen as an Animal[] that nothing writes through.
    const Type *x = a->inner()->nonOptional(), *y = b->inner()->nonOptional();
    result = x == y || (x->isObject() && y->isObject() && objectTypesMayAlias(x, y));
  } else if (a->isDict() && b->isDict()) {
    result = a->dictKind() == b->dictKind() && a->keyType() == b->keyType() && a->valueType() &&
             b->valueType() && a->valueType()->isObject() && b->valueType()->isObject() &&
             objectTypesMayAlias(a->valueType(), b->valueType());
  } else if (a->isBytes() && b->isBytes()) {
    result = true; // views of one buffer
  } else if (a->isObject() && b->isObject()) {
    result = objectTypesMayAlias(a, b);
  }
  return alias_[key] = result;
}

bool Analysis::objectTypesMayAlias(const Type *a, const Type *b) {
  if (a == b || a->isSubtypeOf(b) || b->isSubtypeOf(a))
    return true;
  for (const Type *t : program_.subtypes)
    if (t->isSubtypeOf(a) && t->isSubtypeOf(b))
      return true;
  for (const auto &[x, y] : program_.views)
    if ((x == a && y == b) || (x == b && y == a))
      return true;
  return false;
}

// The types of the values inside a value of `type`, at any depth: what its
// fields, elements and entries hold, and what those hold in turn.
const std::vector<const Type *> &Analysis::inside(const Type *type) {
  auto found = reach_.find(type);
  if (found != reach_.end())
    return found->second;
  std::vector<const Type *> out;
  std::set<const Type *> seen;
  std::vector<const Type *> pending;
  auto add = [&](const Type *t) {
    if (t && seen.insert(t).second)
      pending.push_back(t);
  };
  auto fieldsOf = [&](const Type *t) {
    for (const Field &f : t->fields())
      add(f.type);
  };
  std::function<void(const Type *)> parts = [&](const Type *t) {
    if (!t)
      return;
    switch (t->kind()) {
    case Type::Kind::Optional:
      parts(t->inner());
      break;
    case Type::Kind::Union:
      for (const Type *m : t->members())
        parts(m);
      break;
    case Type::Kind::Array:
    case Type::Kind::Promise:
      add(t->inner());
      break;
    case Type::Kind::Dict:
      add(t->keyType());
      add(t->valueType());
      break;
    case Type::Kind::Result:
      add(t->okType());
      add(t->errType());
      break;
    case Type::Kind::Iterator:
      add(t->yielded());
      break;
    case Type::Kind::Object: {
      fieldsOf(t);
      // An object of a class may be one of a class down its chain, and one
      // of an interface any type declared to be it.
      std::vector<const ClassDecl *> down;
      if (const ClassDecl *cls = t->classDecl())
        down.assign(cls->children.begin(), cls->children.end());
      while (!down.empty()) {
        const ClassDecl *cls = down.back();
        down.pop_back();
        if (cls->type)
          fieldsOf(cls->type);
        down.insert(down.end(), cls->children.begin(), cls->children.end());
      }
      for (const Type *sub : program_.subtypes)
        if (sub != t && sub->isSubtypeOf(t))
          fieldsOf(sub);
      break;
    }
    default:
      break;
    }
  };
  parts(type);
  while (!pending.empty()) {
    const Type *t = pending.back();
    pending.pop_back();
    out.push_back(t);
    parts(t);
  }
  return reach_[type] = std::move(out);
}

// Whether the object of `to` may be the object of `from`, or one inside it;
// with `innerOnly`, only one inside it.
bool Analysis::mayReach(Ref from, Ref to, bool innerOnly) {
  if (!innerOnly && sameObject(from, to))
    return true;
  const Type *source = typeOfRef(from), *target = typeOfRef(to);
  if (!source || !target)
    return true;
  for (const Type *t : inside(source))
    if (holdsReferences(t) && typesMayAlias(t, target))
      return true;
  return false;
}

bool Analysis::locsMayMeet(const Loc &a, const Loc &b) {
  if (a.kind == LocKind::Resource || b.kind == LocKind::Resource) {
    if (a.kind != b.kind)
      return false;
    auto output = [](Res r) { return r == Res::Stdout || r == Res::Stderr; };
    return a.res == b.res || (output(a.res) && output(b.res));
  }
  if (a.kind == LocKind::Var || b.kind == LocKind::Var)
    return a.kind == b.kind && a.key == b.key;
  if (a.kind == LocKind::Contents || b.kind == LocKind::Contents) {
    const Loc &c = a.kind == LocKind::Contents ? a : b;
    const Loc &o = a.kind == LocKind::Contents ? b : a;
    return mayReach(c.ref, o.ref, c.key == inner_) ||
           (o.kind == LocKind::Contents && mayReach(o.ref, c.ref, o.key == inner_));
  }
  if (a.kind == LocKind::Field || b.kind == LocKind::Field) {
    if (a.kind != b.kind)
      return false;
    return (a.key == b.key || a.key == star_ || b.key == star_) && sameObject(a.ref, b.ref);
  }
  return sameObject(a.ref, b.ref);
}

//===----------------------------------------------------------------------===//
// After the walk: the functions called through values, and what calls do
//===----------------------------------------------------------------------===//

void Analysis::resolve() {
  while (true) {
    while (!worklist_.empty()) {
      unsigned fi = worklist_.front();
      worklist_.pop_front();
      walk(fi);
    }
    // The functions called through values, now that the values are known.
    std::map<const Type *, std::vector<unsigned>> generators;
    for (unsigned fi = 0; fi < fns_.size(); ++fi) {
      const FnInfo &f = fns_[fi];
      if ((f.syncGen || f.asyncGen) && f.decl->sym && f.decl->sym->ret)
        generators[f.decl->sym->ret].push_back(fi);
    }
    for (unsigned fi = 0; fi < fns_.size(); ++fi) {
      FnInfo &caller = fns_[fi];
      for (CallRec &rec : caller.calls) {
        if (rec.reflect != CallRec::Reflect::None) {
          std::vector<const ClassDecl *> classes = programClasses_;
          for (const ClassDecl *cls : classes_)
            if (!cls->library && !llvm::is_contained(classes, cls))
              classes.push_back(cls);
          for (const ClassDecl *cls : classes) {
            if (cls->generic)
              continue;
            if (rec.reflect == CallRec::Reflect::Apply) {
              const Type *object = rec.byObject ? rec.byObject->nonOptional() : nullptr;
              if (object && object->isObject() && !object->isAnyObject() && cls->type &&
                  !typesMayAlias(cls->type, object))
                continue;
              for (const FunctionDecl *method : cls->methods)
                if (!method->isStatic)
                  addCallee(rec, method, rec.bySelf, true);
              continue;
            }
            std::optional<Ref> self = Ref{RefKind::Site, rec.site};
            for (const FunctionDecl *ctor : cls->constructors)
              addCallee(rec, ctor, self, true);
            for (const ClassDecl *k = cls; k; k = k->parent)
              if (k->init)
                addCallee(rec, k->init, self, true);
          }
          continue;
        }
        if (rec.channel >= 0) {
          for (unsigned h : std::vector<unsigned>(listeners_[rec.channel]))
            addCallee(rec, fns_[h].decl, classRefOrNone(fns_[h]), true);
          for (const Type *t : listenerTypes_[rec.channel]) {
            auto found = values_.find(t->nonOptional());
            if (found != values_.end())
              for (unsigned g : std::vector<unsigned>(found->second))
                addCallee(rec, fns_[g].decl, classRefOrNone(fns_[g]), true);
          }
          continue;
        }
        if (!rec.byType)
          continue;
        if (rec.kind == CallKind::IterSync || rec.kind == CallKind::IterAsync) {
          auto found = generators.find(rec.byType->nonOptional());
          if (found != generators.end())
            for (unsigned g : found->second)
              addCallee(rec, fns_[g].decl, classRefOrNone(fns_[g]), true);
          continue;
        }
        auto found = values_.find(rec.byType->nonOptional());
        if (found != values_.end())
          for (unsigned g : std::vector<unsigned>(found->second)) {
            std::optional<Ref> self;
            if (fns_[g].thisOwner)
              self = classRef(fns_[g].thisOwner);
            addCallee(rec, fns_[g].decl, self, true);
          }
        if (!rec.byName.empty() && rec.byObject) {
          const Type *object = rec.byObject->nonOptional();
          for (size_t k = 0; k < classes_.size(); ++k) {
            const ClassDecl *cls = classes_[k];
            if (!cls->type || !(cls->literal || typesMayAlias(cls->type, object)))
              continue;
            for (const FunctionDecl *method : cls->methods)
              if (method->name == rec.byName && !method->isStatic)
                addCallee(rec, method, rec.bySelf, true);
          }
        }
      }
    }
    if (worklist_.empty())
      break;
  }
}

std::optional<Access> Analysis::substitute(const Access &a, const FnInfo &callee,
                                           const std::optional<Ref> &self,
                                           const CallRec &rec) const {
  Access b = a;
  b.at = rec.loc;
  b.via = callee.decl;
  if (b.loc.ref.kind == RefKind::This) {
    auto *owner = static_cast<const FunctionDecl *>(b.loc.ref.ptr);
    if (owner == callee.thisOwner && self) {
      if (self->kind == RefKind::Local)
        return std::nullopt;
      b.loc.ref = *self;
    } else if (owner != callee.thisOwner || !self) {
      b.loc.ref = classRef(owner);
    }
  }
  return b;
}

// What calls do goes into the pieces that make them, until nothing grows.
void Analysis::fold() {
  for (unsigned fi = 0; fi < fns_.size(); ++fi) {
    FnInfo &f = fns_[fi];
    for (unsigned s = 0; s < f.segs.size(); ++s)
      if (f.isInline(s))
        f.inlineEffects.insert(f.segs[s].effects.begin(), f.segs[s].effects.end());
    for (unsigned r = 0; r < f.calls.size(); ++r)
      for (const Callee &c : f.calls[r].callees) {
        fns_[c.fn].named.push_back({fi, r});
        if (c.mode != Mode::Callback)
          fns_[c.fn].callers.push_back({fi, r});
      }
  }
  std::deque<unsigned> queue;
  std::vector<bool> queued(fns_.size(), true);
  for (unsigned fi = 0; fi < fns_.size(); ++fi)
    queue.push_back(fi);
  while (!queue.empty()) {
    unsigned fi = queue.front();
    queue.pop_front();
    queued[fi] = false;
    FnInfo &f = fns_[fi];
    bool grew = false;
    for (const CallRec &rec : f.calls)
      for (const Callee &c : rec.callees) {
        if (c.mode == Mode::Callback)
          continue;
        const FnInfo &g = fns_[c.fn];
        std::vector<Access> added;
        for (const Access &a : g.inlineEffects)
          if (std::optional<Access> b = substitute(a, g, c.self, rec))
            added.push_back(*b);
        for (const Access &b : added)
          for (unsigned s : rec.segs)
            if (f.segs[s].effects.insert(b).second && f.isInline(s))
              grew |= f.inlineEffects.insert(b).second;
      }
    if (!grew)
      continue;
    for (const auto &[caller, r] : f.callers)
      if (!queued[caller]) {
        queued[caller] = true;
        queue.push_back(caller);
      }
  }
}

//===----------------------------------------------------------------------===//
// Tasks
//===----------------------------------------------------------------------===//

std::optional<Ref> Analysis::bind(const std::optional<Ref> &self, const Activation &caller) const {
  if (!self || self->kind != RefKind::This)
    return self;
  auto *owner = static_cast<const FunctionDecl *>(self->ptr);
  if (owner == fns_[caller.fn].thisOwner && caller.self)
    return caller.self;
  return classRef(owner);
}

bool Analysis::scheduled(const Activation &act, unsigned seg) const {
  const FnInfo &f = fns_[act.fn];
  if (seg == 0)
    return act.callbackStart;
  return f.segs[seg].kind == SegKind::Await || f.segs[seg].kind == SegKind::ForAwait;
}

unsigned Analysis::activate(unsigned fn, int root, std::optional<Ref> self, bool callbackStart,
                            bool rootStart) {
  auto key = std::make_tuple(fn, root, self, callbackStart);
  auto [it, inserted] = actIndex_.try_emplace(key, unsigned(acts_.size()));
  if (inserted) {
    Activation &act = acts_.emplace_back();
    act.fn = fn;
    act.root = root;
    act.self = self;
    act.callbackStart = callbackStart;
    actsByTask_[{fn, root}].push_back(it->second);
  }
  acts_[it->second].rootStart |= rootStart;
  return it->second;
}

int Analysis::newRoot(int parent, unsigned fn, std::optional<Ref> self, unsigned callerFn,
                      unsigned rec, Mode mode) {
  // A task that starts itself again down its chain is that task again:
  // several of it may run at once.
  for (int p = parent; p >= 0; p = roots_[p].parent) {
    Root &up = roots_[p];
    if (up.fn == fn && up.self == self && up.callerFn == callerFn && up.rec == int(rec)) {
      up.multi = up.recursive = true;
      return p;
    }
  }
  auto key = std::make_tuple(parent, fn, self, callerFn, rec);
  if (auto found = rootIndex_.find(key); found != rootIndex_.end())
    return found->second;
  // A program that makes too many kinds of tasks gets them told apart less.
  constexpr size_t kMaxRoots = 4096;
  if (roots_.size() >= kMaxRoots)
    for (size_t i = 0; i < roots_.size(); ++i) {
      Root &r = roots_[i];
      if (r.fn == fn && r.callerFn == callerFn && r.rec == int(rec)) {
        r.multi = true;
        r.parent = -1;
        return int(i);
      }
    }
  rootIndex_[key] = int(roots_.size());
  Root &root = roots_.emplace_back();
  root.fn = fn;
  root.self = self;
  root.parent = parent;
  root.callerFn = callerFn;
  root.rec = int(rec);
  root.mode = mode;
  return int(roots_.size() - 1);
}

void Analysis::explore() {
  Root &main = roots_.emplace_back();
  main.fn = 0;
  std::deque<unsigned> queue{activate(0, 0, std::nullopt, true, false)};
  llvm::DenseSet<unsigned> done;
  while (!queue.empty()) {
    unsigned ai = queue.front();
    queue.pop_front();
    if (!done.insert(ai).second)
      continue;
    Activation act = acts_[ai];
    const FnInfo &f = fns_[act.fn];
    for (unsigned r = 0; r < f.calls.size(); ++r) {
      const CallRec &rec = f.calls[r];
      for (const Callee &c : rec.callees) {
        std::optional<Ref> self = bind(c.self, act);
        unsigned next;
        switch (c.mode) {
        case Mode::Sync:
        case Mode::Await:
        case Mode::AsyncIter:
          next = activate(c.fn, act.root, self, false, false);
          inlineEdges_.push_back({ai, r, next});
          break;
        case Mode::Spawn:
          next = activate(c.fn, newRoot(act.root, c.fn, self, act.fn, r, Mode::Spawn), self, false,
                          true);
          break;
        case Mode::Callback:
          next = activate(c.fn, newRoot(act.root, c.fn, self, act.fn, r, Mode::Callback), self,
                          true, false);
          break;
        }
        if (!done.contains(next))
          queue.push_back(next);
      }
    }
  }
}

void Analysis::findPieces() {
  actPieces_.assign(acts_.size(), {});
  for (unsigned ai = 0; ai < acts_.size(); ++ai) {
    const Activation &act = acts_[ai];
    const FnInfo &f = fns_[act.fn];
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      if (!scheduled(act, s))
        continue;
      auto key = std::make_tuple(act.fn, s, act.root, act.self);
      auto [it, inserted] = pieceIndex_.try_emplace(key, unsigned(pieces_.size()));
      if (inserted) {
        pieces_.push_back({act.fn, s, act.root, act.self});
        segPieces_[{act.fn, s}].push_back(it->second);
        ++sameTask_[std::make_tuple(act.fn, s, act.root)];
      }
      actPieces_[ai].push_back(it->second);
    }
  }
}

// The pieces of its task that a function's inline segments run inside.
void Analysis::findHosts() {
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[caller, r, callee] : inlineEdges_) {
      const Activation &from = acts_[caller];
      std::vector<unsigned> at = piecesAt(from, fns_[from.fn].calls[r].segs);
      Activation &to = acts_[callee];
      for (unsigned p : at)
        changed |= to.hosts.insert(p).second;
    }
  }
}

std::vector<unsigned> Analysis::piecesAt(const Activation &act,
                                         llvm::ArrayRef<unsigned> segs) const {
  std::vector<unsigned> out;
  const FnInfo &f = fns_[act.fn];
  for (unsigned s : segs) {
    if (scheduled(act, s)) {
      auto it = pieceIndex_.find(std::make_tuple(act.fn, s, act.root, act.self));
      if (it != pieceIndex_.end())
        out.push_back(it->second);
    } else if (f.isInline(s)) {
      // Its start runs inside the piece that calls it; the start of a task
      // runs inside the piece of another task.
      if (act.rootStart)
        out.push_back(startNode(act.root));
      else
        out.insert(out.end(), act.hosts.begin(), act.hosts.end());
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Whether the call runs at most once in one run of the task that runs
// `rootFn`.
bool Analysis::once(unsigned fi, unsigned rec, unsigned rootFn, unsigned depth) {
  if (fns_[fi].calls[rec].repeated)
    return false;
  return fnOnce(fi, rootFn, depth);
}

bool Analysis::fnOnce(unsigned fi, unsigned rootFn, unsigned depth) {
  if (fi == rootFn)
    return true;
  if (depth > 32 || valueFns_.contains(fi))
    return false;
  auto key = std::make_pair(fi, rootFn);
  auto found = fnOnce_.find(key);
  if (found != fnOnce_.end())
    return found->second;
  fnOnce_[key] = false; // a recursive function runs more than once
  const FnInfo &f = fns_[fi];
  bool result = false;
  if (f.named.size() == 1) {
    auto [cf, cr] = f.named[0];
    const CallRec &rec = fns_[cf].calls[cr];
    bool direct = llvm::any_of(rec.callees, [&](const Callee &c) {
      return c.fn == fi && !c.byType &&
             (c.mode == Mode::Sync || c.mode == Mode::Await || c.mode == Mode::AsyncIter);
    });
    result = direct && once(cf, cr, rootFn, depth + 1);
  }
  return fnOnce_[key] = result;
}

// Whether an error that the await throws ends the program before any other
// code runs: then no code after it may run while the tasks it waited for
// still do.
bool Analysis::rejectionEnds(const AwaitExpr *a, unsigned depth) {
  auto found = awaits_.find(a);
  if (found == awaits_.end() || found->second.caught)
    return false;
  return fnRejectionEnds(found->second.fn, depth);
}

bool Analysis::fnRejectionEnds(unsigned fi, unsigned depth) {
  if (fi == 0)
    return true; // the top-level code: an error there ends the program
  if (depth > 32 || valueFns_.contains(fi) || !fns_[fi].async)
    return false;
  auto found = rejectionEnds_.find(fi);
  if (found != rejectionEnds_.end())
    return found->second;
  rejectionEnds_[fi] = false;
  bool result = true;
  for (const auto &[cf, cr] : fns_[fi].named) {
    const CallRec &rec = fns_[cf].calls[cr];
    for (const Callee &c : rec.callees) {
      if (c.fn != fi)
        continue;
      bool ends = false;
      switch (c.mode) {
      case Mode::Await:
        ends = rec.await ? rejectionEnds(rec.await, depth + 1)
                         : rec.returned && fnRejectionEnds(cf, depth + 1);
        break;
      case Mode::Spawn:
        // `void f()`: an error nobody handles ends the program.
        ends = rec.voided ||
               (rec.join && !joins_.lookup(rec.join).settled && rejectionEnds(rec.join, depth + 1));
        break;
      case Mode::Callback:
        ends = !rec.handled;
        break;
      default:
        break;
      }
      result &= ends && !c.byType;
    }
  }
  return rejectionEnds_[fi] = result;
}

void Analysis::findJoins() {
  for (size_t i = 1; i < roots_.size(); ++i) {
    Root &root = roots_[i];
    if (root.rec < 0)
      continue;
    const FnInfo &caller = fns_[root.callerFn];
    const CallRec &rec = caller.calls[root.rec];
    int parentFn = root.parent >= 0 ? int(roots_[root.parent].fn) : -1;
    root.multi |= rec.interval || parentFn < 0 || !once(root.callerFn, root.rec, parentFn, 0);
    // Each copy of a task that map starts for an element writes the element
    // of its own index: if the map runs once, no two copies share one.
    const FunctionDecl *callee = fns_[root.fn].decl;
    if (rec.kind == CallKind::Inline && rec.call && rec.call->builtin == Builtin::ArrMap &&
        callee && callee->params.size() > 1 && callee->params[1].sym &&
        ownIndex_.contains(callee->params[1].sym) && !rec.inLoop && parentFn >= 0 &&
        fnOnce(root.callerFn, unsigned(parentFn), 0)) {
      bool single = true;
      for (int p = root.parent; p >= 0; p = roots_[p].parent)
        single &= !roots_[p].multi;
      if (single)
        root.ownCell = callee;
    }
    if (root.parent < 0)
      continue;
    // Where it starts: the pieces of the parent that make the call.
    std::set<unsigned> spawnAt, interval;
    for (unsigned ai : actsOf(root.callerFn, root.parent))
      for (unsigned p : piecesAt(acts_[ai], rec.segs))
        spawnAt.insert(p);
    root.spawnAt.assign(spawnAt.begin(), spawnAt.end());
    if (root.mode != Mode::Spawn || !rec.join)
      continue;
    auto join = joins_.find(rec.join);
    // An error there, of a task or of the code that starts them, must end
    // the program; otherwise code could go on while some still run.
    if (join == joins_.end() || !rejectionEnds(rec.join, 0))
      continue;
    // The pieces of the parent while it runs: those where the Promise.all
    // waits.
    bool found = false;
    for (unsigned ai : actsOf(join->second.fn, root.parent)) {
      found = true;
      for (unsigned p : piecesAt(acts_[ai], join->second.segs))
        interval.insert(p);
    }
    if (!found)
      continue;
    root.join = rec.join;
    root.interval.assign(interval.begin(), interval.end());
  }
  // The chains, parents first: a task is made after its parent.
  for (size_t i = 1; i < roots_.size(); ++i) {
    Root &root = roots_[i];
    if (root.parent < 0 || size_t(root.parent) >= i)
      continue;
    const Root &parent = roots_[root.parent];
    if (root.join) {
      root.chain.push_back({root.parent, root.interval});
      root.chain.insert(root.chain.end(), parent.chain.begin(), parent.chain.end());
      continue;
    }
    // Nothing waits for it: it may run on after its parent, and after the
    // task that waits for its parent.
    root.chain.push_back({root.parent, reach(root.parent, root.spawnAt)});
    for (const auto &[up, pieces] : parent.chain)
      root.chain.push_back({up, reach(up, pieces)});
  }
}

// The pieces of each task that may follow each other: within a function
// from await to await, and around an awaited call, whose pieces come after
// the piece that calls it and before the pieces that follow.
void Analysis::buildFlow() {
  flow_.assign(roots_.size(), {});
  for (const Activation &act : acts_) {
    const FnInfo &f = fns_[act.fn];
    for (const auto &[a, b] : f.succ)
      for (unsigned x : piecesAt(act, {a}))
        for (unsigned y : piecesAt(act, {b}))
          flow_[act.root][x].push_back(y);
  }
  for (const auto &[caller, r, callee] : inlineEdges_) {
    const Activation &x = acts_[caller];
    if (acts_[callee].root != x.root || actPieces_[callee].empty())
      continue;
    const FnInfo &f = fns_[x.fn];
    const CallRec &rec = f.calls[r];
    std::vector<unsigned> after;
    for (const auto &[a, b] : f.succ)
      if (llvm::is_contained(rec.segs, a))
        for (unsigned n : piecesAt(x, {b}))
          after.push_back(n);
    for (unsigned p : actPieces_[callee]) {
      std::vector<unsigned> &next = flow_[x.root][p];
      next.insert(next.end(), after.begin(), after.end());
    }
  }
  for (auto &graph : flow_)
    for (auto &[node, next] : graph) {
      std::sort(next.begin(), next.end());
      next.erase(std::unique(next.begin(), next.end()), next.end());
    }
}

// The pieces of the task that may run from those of `from` on, them
// included.
std::vector<unsigned> Analysis::reach(int root, const std::vector<unsigned> &from) const {
  std::set<unsigned> seen(from.begin(), from.end());
  std::vector<unsigned> pending(from.begin(), from.end());
  const auto &graph = flow_[root];
  while (!pending.empty()) {
    unsigned n = pending.back();
    pending.pop_back();
    auto found = graph.find(n);
    if (found == graph.end())
      continue;
    for (unsigned m : found->second)
      if (seen.insert(m).second)
        pending.push_back(m);
  }
  return std::vector<unsigned>(seen.begin(), seen.end());
}

bool Analysis::concurrent(unsigned a, unsigned b) const {
  const PieceInst &pa = pieces_[a], &pb = pieces_[b];
  if (pa.root == pb.root)
    return roots_[pa.root].multi;
  auto contains = [](const std::vector<unsigned> &v, unsigned x) {
    return std::binary_search(v.begin(), v.end(), x);
  };
  // One task runs only while the other is at some of its pieces.
  for (const auto &[r, interval] : roots_[pa.root].chain)
    if (r == pb.root)
      return roots_[r].multi || contains(interval, b);
  for (const auto &[r, interval] : roots_[pb.root].chain)
    if (r == pa.root)
      return roots_[r].multi || contains(interval, a);
  // Both run while a task up both chains is at some of its pieces.
  for (const auto &[ra, ia] : roots_[pa.root].chain)
    for (const auto &[rb, ib] : roots_[pb.root].chain)
      if (ra == rb) {
        if (roots_[ra].multi)
          return true;
        for (unsigned x : ia)
          if (contains(ib, x))
            return true;
        return false;
      }
  return true;
}

std::vector<Touch> Analysis::touches(const Effects &effects) const {
  std::vector<Touch> out;
  std::map<Loc, size_t> index;
  for (const Access &a : effects) {
    auto [it, inserted] = index.try_emplace(a.loc, out.size());
    if (inserted) {
      out.emplace_back();
      out.back().loc = a.loc;
    }
    Touch &t = out[it->second];
    (a.write ? t.write : t.read) = true;
    auto at = std::make_pair(a.at, a.via);
    if (!llvm::is_contained(t.ats, at))
      t.ats.push_back(at);
    auto &mine = a.write ? t.writeAts : t.readAts;
    if (!llvm::is_contained(mine, at))
      mine.push_back(at);
  }
  return out;
}

// The bits of a location for the quick test of decide(): output, input and
// the other resources each have one; variables and named fields one by a
// hash; the elements of objects one. A location that may be any field, or
// anything inside an object, has the bits of all that it may meet.
uint64_t Analysis::maskOf(const Loc &loc) const {
  constexpr uint64_t kElements = uint64_t(1) << 8, kAnyField = uint64_t(1) << 9;
  constexpr uint64_t kFieldBits = ~uint64_t(0) << 40;
  auto hash = [](const void *p, unsigned first, unsigned count) {
    return uint64_t(1) << (first + unsigned((reinterpret_cast<uintptr_t>(p) >> 4) % count));
  };
  switch (loc.kind) {
  case LocKind::Resource:
    return uint64_t(1) << (loc.res == Res::Stderr ? unsigned(Res::Stdout) : unsigned(loc.res));
  case LocKind::Var:
    return hash(loc.key, 10, 30);
  case LocKind::Field:
    return loc.key == star_ ? kAnyField | kFieldBits : hash(loc.key, 40, 24);
  case LocKind::Elements:
  case LocKind::OwnCell:
    return kElements;
  case LocKind::Contents:
    return kElements | kAnyField | kFieldBits;
  }
  return ~uint64_t(0);
}

void Analysis::decide() {
  pieceTouches_.resize(pieces_.size());
  for (unsigned i = 0; i < pieces_.size(); ++i) {
    const PieceInst &p = pieces_[i];
    const FnInfo &f = fns_[p.fn];
    Effects effects;
    for (const Access &a : f.segs[p.seg].effects) {
      Access b = a;
      if (b.loc.ref.kind == RefKind::This) {
        auto *owner = static_cast<const FunctionDecl *>(b.loc.ref.ptr);
        if (owner == f.thisOwner && p.self) {
          if (p.self->kind == RefKind::Local)
            continue;
          b.loc.ref = *p.self;
        } else {
          b.loc.ref = classRef(owner);
        }
      }
      effects.insert(b);
    }
    pieceTouches_[i] = touches(effects);
  }
  decisions_.assign(pieces_.size(), {});
  // What each piece touches and writes, as bits: two pieces may conflict
  // only where the writes of one meet the touches of the other.
  std::vector<uint64_t> touched(pieces_.size(), 0), written(pieces_.size(), 0);
  for (unsigned i = 0; i < pieces_.size(); ++i)
    for (const Touch &t : pieceTouches_[i]) {
      uint64_t bits = maskOf(t.loc);
      touched[i] |= bits;
      if (t.write)
        written[i] |= bits;
    }
  for (unsigned i = 0; i < pieces_.size(); ++i)
    for (unsigned j = i; j < pieces_.size(); ++j) {
      bool maybe = (written[i] & touched[j]) || (written[j] & touched[i]);
      // Whether two pieces meet matters only for a conflict, or to know that
      // a piece runs alone.
      if (!maybe && !decisions_[i].alone && !decisions_[j].alone)
        continue;
      if (!concurrent(i, j))
        continue;
      decisions_[i].alone = decisions_[j].alone = false;
      if (!maybe)
        continue;
      std::vector<Loc> shared;
      std::vector<const Touch *> mineI, mineJ;
      for (const Touch &x : pieceTouches_[i])
        for (const Touch &y : pieceTouches_[j]) {
          if (!(x.write || y.write))
            continue;
          // The element of its own index that each copy of a task writes.
          if (x.loc.kind == LocKind::OwnCell && y.loc.kind == LocKind::OwnCell &&
              x.loc.key == y.loc.key && pieces_[i].root == pieces_[j].root &&
              roots_[pieces_[i].root].ownCell == x.loc.key)
            continue;
          Loc lx = x.loc, ly = y.loc;
          if (lx.kind == LocKind::OwnCell)
            lx.kind = LocKind::Elements;
          if (ly.kind == LocKind::OwnCell)
            ly.kind = LocKind::Elements;
          if (!locsMayMeet(lx, ly))
            continue;
          if (!llvm::is_contained(shared, x.loc))
            shared.push_back(x.loc);
          mineI.push_back(&x);
          mineJ.push_back(&y);
        }
      if (shared.empty())
        continue;
      auto extend = [&](Decision &d, const std::vector<const Touch *> &mine) {
        for (const Touch *t : mine)
          for (const auto &[at, via] : t->ats) {
            if (!d.from.isValid() || posOf(at) < posOf(d.from))
              d.from = at;
            if (!d.to.isValid() || posOf(d.to) < posOf(at))
              d.to = at;
            if (!llvm::is_contained(d.ats, at))
              d.ats.push_back(at);
          }
      };
      decisions_[i].partners.push_back({j, shared});
      extend(decisions_[i], mineI);
      if (j != i) {
        std::vector<Loc> theirs;
        for (const Touch *t : mineJ)
          if (!llvm::is_contained(theirs, t->loc))
            theirs.push_back(t->loc);
        decisions_[j].partners.push_back({i, theirs});
        extend(decisions_[j], mineJ);
      }
    }
}

void Analysis::run() {
  star_ = intern("*");
  length_ = intern("%length");
  inner_ = intern("%inner");
  state_ = intern("%state");
  listenersKey_ = intern("%listeners");
  discover(nullptr); // the top-level code
  resolve();
  fold();
  explore();
  findPieces();
  findHosts();
  buildFlow();
  findJoins();
  decide();
}

//===----------------------------------------------------------------------===//
// Explaining
//===----------------------------------------------------------------------===//

Analysis::Pos Analysis::posOf(llvm::SMLoc loc) const {
  Pos pos;
  if (!loc.isValid())
    return pos;
  pos.buffer = sources_.FindBufferContainingLoc(loc);
  if (pos.buffer)
    pos.offset =
        unsigned(loc.getPointer() - sources_.getMemoryBuffer(pos.buffer)->getBufferStart());
  return pos;
}

unsigned Analysis::lineOf(llvm::SMLoc loc) const {
  if (!loc.isValid())
    return 0;
  return sources_.FindLineNumber(loc);
}

// "12" in the entry file, "lib/math.tz:12" in another.
std::string Analysis::where(llvm::SMLoc loc) const {
  if (!loc.isValid())
    return "?";
  unsigned buffer = sources_.FindBufferContainingLoc(loc);
  std::string line = std::to_string(sources_.FindLineNumber(loc, buffer));
  if (!buffer)
    return line;
  llvm::StringRef id = sources_.getMemoryBuffer(buffer)->getBufferIdentifier();
  for (const Module *module : program_.modules)
    if (module->path == id)
      return module->isEntry ? line : module->name + ":" + line;
  return line;
}

std::string Analysis::typeText(const Type *t) const {
  if (!t)
    return "object";
  return t->nonOptional()->name().str();
}

std::string Analysis::fnName(const FnInfo &f) const {
  const FunctionDecl *fn = f.decl;
  if (!fn)
    return "the top-level code";
  std::string name = fn->name.str();
  if (fn->cls) {
    std::string owner = fn->cls->literal ? "the object " + atText(where(fn->cls->literal->loc()))
                                         : fn->cls->name.str();
    if (fn->isConstructor)
      return "constructor of " + owner;
    if (fn == fn->cls->init)
      return "fields of " + owner;
    return fn->cls->literal ? name + " of " + owner : owner + "." + name;
  }
  if (fn->isInstance && !fn->typeArgs.empty()) {
    name += "<";
    for (size_t i = 0; i < fn->typeArgs.size(); ++i)
      name += (i ? ", " : "") + typeText(fn->typeArgs[i]);
    name += ">";
  }
  if (fn->isArrow || fn->isExpression) {
    if (!name.empty())
      return name;
    name = (fn->isArrow ? "the arrow " : "the function ") + atText(where(fn->loc()));
    // Each copy of a generic function has a copy of it.
    for (const FunctionDecl *up = fn->parent; up; up = up->parent)
      if (up->isInstance) {
        FnInfo info;
        info.decl = up;
        return name + " in " + fnName(info);
      }
  }
  return name;
}

std::string Analysis::fnTitle(const FnInfo &f) const {
  const FunctionDecl *fn = f.decl;
  if (!fn)
    return "top-level code";
  std::string kind = f.asyncGen ? "async generator" : f.async ? "async" : "";
  std::string what = fn->cls            ? (fn->isConstructor ? "" : "method")
                     : fn->isArrow      ? "arrow"
                     : fn->isExpression ? "function expression"
                                        : "function";
  std::string title = kind;
  if (!what.empty())
    title += (title.empty() ? "" : " ") + what;
  if (title.empty())
    title = "callback";
  if (fn->isArrow || fn->isExpression) {
    if (!fn->name.empty())
      title += " " + fn->name.str();
  } else {
    title += " " + fnName(f);
  }
  title += ", " + lineText(where(fn->loc()));
  for (const FunctionDecl *up = fn->parent; up && (fn->isArrow || fn->isExpression);
       up = up->parent)
    if (up->isInstance) {
      FnInfo info;
      info.decl = up;
      return title + ", in " + fnName(info);
    }
  return title;
}

std::string Analysis::segTitle(const FnInfo &f, unsigned seg) const {
  const Segment &s = f.segs[seg];
  if (s.kind == SegKind::Start)
    return "start";
  // Two awaits on one line are told apart by their column.
  std::string at = where(s.loc);
  unsigned line = lineOf(s.loc), same = 0;
  for (const Segment &o : f.segs)
    if (o.kind != SegKind::Start && lineOf(o.loc) == line &&
        posOf(o.loc).buffer == posOf(s.loc).buffer)
      ++same;
  if (same > 1)
    at += ":" + std::to_string(sources_.getLineAndColumn(s.loc).second);
  switch (s.kind) {
  case SegKind::Await:
    return (s.delegate ? "after yield* " : "after await ") + atText(at);
  case SegKind::ForAwait:
    return "after for await " + atText(at);
  case SegKind::Yield:
    return "after yield " + atText(at);
  case SegKind::Start:
    break;
  }
  return "start";
}

std::string Analysis::calleeText(const Expr *e) const {
  e = strip(e);
  if (auto *n = dyn_cast_or_null<NameExpr>(e))
    return n->name.str();
  if (auto *m = dyn_cast_or_null<MemberExpr>(e))
    return calleeText(m->object) + "." + m->name.str();
  if (auto *c = dyn_cast_or_null<CallExpr>(e))
    return calleeText(c->callee) + "()";
  if (auto *ix = dyn_cast_or_null<IndexExpr>(e))
    return calleeText(ix->object) + "[...]";
  return "(...)";
}

// The text of a call as written, `a.run(2)`; a shorter one for a long call.
std::string Analysis::callText(const CallExpr *c) const {
  std::string fallback = calleeText(c->callee) + "()";
  const Expr *first = c->callee;
  while (true) {
    if (auto *m = dyn_cast<MemberExpr>(first))
      first = m->object;
    else if (auto *call = dyn_cast<CallExpr>(first))
      first = call->callee;
    else if (auto *ix = dyn_cast<IndexExpr>(first))
      first = ix->object;
    else if (auto *conv = dyn_cast<CastExpr>(first); conv && conv->isImplicit())
      first = conv->operand;
    else
      break;
  }
  const char *start = first->loc().getPointer(), *p = c->loc().getPointer();
  if (!start || !p || start > p)
    return fallback;
  while (*p && *p != '(' && *p != '\n' && *p != ';')
    ++p;
  if (*p != '(')
    return fallback;
  int depth = 0;
  for (; *p && *p != '\n'; ++p) {
    if (*p == '(')
      ++depth;
    else if (*p == ')' && --depth == 0)
      break;
  }
  if (*p != ')')
    return fallback;
  std::string text(start, size_t(p - start + 1));
  return text.size() <= 40 ? text : fallback;
}

// How the explanation names a task; two tasks started on one line by calls
// of the same text are told apart by the column.
std::string Analysis::rootTitle(int r) const {
  if (rootTitles_.empty()) {
    // Only tasks of one function need names apart: the explanation shows
    // tasks under the pieces of their function.
    auto key = [&](size_t i, const std::string &title) {
      return title + "|" + std::to_string(roots_[i].fn);
    };
    // First by the column, where that tells them apart.
    std::map<std::string, std::set<std::string>> columns;
    for (size_t i = 0; i < roots_.size(); ++i)
      columns[key(i, rootTitleAt(int(i), false))].insert(rootTitleAt(int(i), true));
    std::vector<std::string> titles;
    for (size_t i = 0; i < roots_.size(); ++i) {
      std::string title = rootTitleAt(int(i), false);
      titles.push_back(columns[key(i, title)].size() > 1 ? rootTitleAt(int(i), true) : title);
    }
    std::map<std::string, unsigned> count;
    // Then by the task that starts them (parents come first), and last by
    // their order.
    std::map<std::string, unsigned> same, seen;
    for (size_t i = 0; i < roots_.size(); ++i)
      ++same[key(i, titles[i])];
    for (size_t i = 0; i < roots_.size(); ++i) {
      std::string title = titles[i];
      int parent = roots_[i].parent;
      if (same[key(i, titles[i])] > 1 && parent >= 0 && size_t(parent) < i)
        title += " from " + rootTitles_[size_t(parent)];
      rootTitles_.push_back(title);
    }
    count.clear();
    for (size_t i = 0; i < roots_.size(); ++i)
      ++count[key(i, rootTitles_[i])];
    for (size_t i = 0; i < roots_.size(); ++i)
      if (count[key(i, rootTitles_[i])] > 1)
        rootTitles_[i] += " #" + std::to_string(++seen[key(i, rootTitles_[i])]);
  }
  return rootTitles_[size_t(r)];
}

std::string Analysis::rootTitleAt(int r, bool column) const {
  const Root &root = roots_[r];
  if (r == 0)
    return "the top-level code";
  const CallRec &rec = fns_[root.callerFn].calls[root.rec];
  std::string place = where(rec.loc);
  if (column)
    place += ":" + std::to_string(sources_.getLineAndColumn(rec.loc).second);
  std::string at = " " + atText(place);
  if (root.mode == Mode::Callback)
    return (root.multi ? "the callbacks" : "the callback") + at;
  if (rec.kind == CallKind::Inline) {
    // A callback that map (or the like) runs for each element.
    const FunctionDecl *fn = fns_[root.fn].decl;
    std::string text = (root.multi ? "the tasks of " : "the task of ") + fnName(fns_[root.fn]);
    if (fn && !fn->name.empty())
      text += at;
    return text;
  }
  std::string text = rec.call ? callText(rec.call) : fnName(fns_[root.fn]) + "()";
  return (root.multi ? "the tasks " : "the task ") + text + at;
}

std::string Analysis::contextOf(unsigned piece) const {
  const PieceInst &p = pieces_[piece];
  std::string text = rootTitle(p.root);
  // Pieces of one function in one task with different objects as `this`.
  if (sameTask_.at(std::make_tuple(p.fn, p.seg, p.root)) > 1 && p.self)
    text += ", this = " + refName(*p.self);
  return text;
}

std::string Analysis::pieceTitle(unsigned piece, bool withContext) const {
  const PieceInst &p = pieces_[piece];
  const FnInfo &f = fns_[p.fn];
  std::string text = p.seg != 0          ? fnName(f) + " " + segTitle(f, p.seg)
                     : f.segs.size() > 1 ? "the start of " + fnName(f)
                                         : fnName(f);
  if (withContext)
    text += " in " + contextOf(piece);
  return text;
}

std::string Analysis::refName(Ref r) const {
  switch (r.kind) {
  case RefKind::This:
    return "this";
  case RefKind::Local:
    return "its own object";
  case RefKind::Site: {
    auto *e = static_cast<const Expr *>(r.ptr);
    auto found = siteName_.find(e);
    if (found != siteName_.end())
      return found->second.str();
    const Type *t = e->type ? e->type->nonOptional() : nullptr;
    std::string what = !t                ? "the object"
                       : t->isArray()    ? "the array"
                       : t->isMap()      ? "the map"
                       : t->isSet()      ? "the set"
                       : t->isDict()     ? "the record"
                       : t->isIterator() ? "the iterator"
                                         : "the " + typeText(t);
    return what + " made " + atText(where(e->loc()));
  }
  case RefKind::Type:
    return "any " + typeText(static_cast<const Type *>(r.ptr));
  }
  return "?";
}

std::string Analysis::locName(const Loc &loc) const {
  switch (loc.kind) {
  case LocKind::Var: {
    // The compiler's own variables: what a decorator put in a method's
    // place ("%Calc.add.0"), and the others.
    llvm::StringRef name = static_cast<const VarSymbol *>(loc.key)->name;
    if (!name.consume_front("%"))
      return name.str();
    if (name.count('.') == 2)
      return "the method " + name.rsplit('.').first.str() + " as decorated";
    return "a variable the compiler made";
  }
  case LocKind::Field: {
    llvm::StringRef field = static_cast<const char *>(loc.key);
    std::string object = refName(loc.ref);
    if (field == "*")
      return "the fields of " + object;
    if (field == "%time")
      return "the time of " + object;
    if (field == "%state")
      return "the state of " + object;
    if (field == "%listeners")
      return "the listeners of " + object;
    if (field == "%length")
      return "the length of " + object;
    if (loc.ref.kind == RefKind::Type ||
        (loc.ref.kind == RefKind::Site && !siteName_.count(static_cast<const Expr *>(loc.ref.ptr))))
      return field.str() + " of " + object;
    return object + "." + field.str();
  }
  case LocKind::Elements: {
    const Type *t = typeOfRef(loc.ref);
    std::string what = t && t->isDict()                                 ? "entries"
                       : t && (t->isIterator() || t->isAsyncIterator()) ? "position"
                       : t && t->isBytes()                              ? "bytes"
                       : t && t->isJson()                               ? "contents"
                                                                        : "elements";
    return what + " of " + refName(loc.ref);
  }
  case LocKind::OwnCell:
    return "elements of " + refName(loc.ref) + ", each task its own";
  case LocKind::Contents:
    if (loc.key == inner_)
      return "the objects inside its own " + typeText(typeOfRef(loc.ref));
    return "everything in " + refName(loc.ref);
  case LocKind::Resource:
    switch (loc.res) {
    case Res::Stdout:
      return "stdout";
    case Res::Stderr:
      return "stderr";
    case Res::Stdin:
      return "stdin";
    case Res::Files:
      return "the files";
    case Res::Process:
      return "the exit code and environment";
    case Res::Signals:
      return "the abort signals";
    case Res::Metadata:
      return "the metadata";
    case Res::Runtime:
      return "the runtime";
    }
  }
  return "?";
}

std::string
Analysis::positions(const std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> &ats) const {
  std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> sorted = ats;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [&](const auto &a, const auto &b) { return posOf(a.first) < posOf(b.first); });
  std::vector<std::string> parts;
  bool entryFile = false;
  for (const auto &[at, via] : sorted) {
    std::string text = where(at);
    entryFile |= !inOtherFile(text);
    if (via) {
      FnInfo info;
      info.decl = via;
      text += " in " + fnName(info);
    }
    if (!llvm::is_contained(parts, text))
      parts.push_back(text);
  }
  constexpr size_t kShown = 6;
  std::string out;
  if (entryFile)
    out = parts.size() == 1 ? "line " : "lines ";
  for (size_t i = 0; i < parts.size() && i < kShown; ++i)
    out += (i ? ", " : "") + parts[i];
  if (parts.size() > kShown)
    out += " and " + std::to_string(parts.size() - kShown) + " more";
  return out;
}

std::vector<std::string> Analysis::decisionLines(unsigned piece) const {
  const Decision &d = decisions_[piece];
  std::vector<std::string> lines;
  if (d.alone)
    return {"runs alone"};
  if (d.partners.empty())
    return {"parallel"};
  std::vector<std::pair<std::string, std::string>> partners;
  for (const Partner &partner : d.partners) {
    std::string who;
    if (partner.piece == piece) {
      who = "the same piece in other tasks";
    } else {
      // The partner's task, if its piece runs in more than one.
      const PieceInst &p = pieces_[partner.piece];
      who = pieceTitle(partner.piece, segPieces_.at({p.fn, p.seg}).size() > 1);
    }
    std::vector<std::string> names;
    for (const Loc &loc : partner.locs)
      names.push_back(locName(loc));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string what;
    for (size_t i = 0; i < names.size(); ++i)
      what += (i ? ", " : "") + names[i];
    partners.push_back({who, what});
  }
  std::sort(partners.begin(), partners.end());
  partners.erase(std::unique(partners.begin(), partners.end()), partners.end());
  for (const auto &[who, what] : partners)
    lines.push_back("in order with " + who + ": " + what);
  std::string from = where(d.from), to = where(d.to);
  if (from == to) {
    lines.push_back("in order: " + lineText(from));
  } else if (inOtherFile(from) || inOtherFile(to)) {
    // In another file: "lib.tz:3-5" within it.
    llvm::StringRef a(from), b(to);
    if (a.rsplit(':').first == b.rsplit(':').first)
      lines.push_back("in order: " + from + "-" + b.rsplit(':').second.str());
    else
      lines.push_back("in order: " + from + "-" + to);
  } else {
    lines.push_back("in order: lines " + from + "-" + to);
  }
  return lines;
}

void Analysis::print(llvm::raw_ostream &out) {
  if (!program_.usesAsync) {
    out << "no async code: the program runs as one piece\n";
    return;
  }
  // The functions with pieces, in the order of the source: the top-level
  // code first.
  std::vector<unsigned> order;
  std::vector<bool> shown(fns_.size(), false);
  for (const PieceInst &p : pieces_)
    shown[p.fn] = true;
  // An async function whose start runs inside its callers is shown too.
  for (const Activation &act : acts_)
    if (fns_[act.fn].async || fns_[act.fn].asyncGen)
      shown[act.fn] = true;
  for (unsigned fi = 0; fi < fns_.size(); ++fi)
    if (shown[fi])
      order.push_back(fi);
  std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
    if (!fns_[a].decl || !fns_[b].decl)
      return !fns_[a].decl && fns_[b].decl;
    return posOf(fns_[a].decl->loc()) < posOf(fns_[b].decl->loc());
  });
  bool first = true;
  for (unsigned fi : order) {
    const FnInfo &f = fns_[fi];
    if (!first)
      out << "\n";
    first = false;
    out << fnTitle(f) << "\n";
    std::vector<unsigned> segs;
    for (unsigned s = 0; s < f.segs.size(); ++s)
      segs.push_back(s);
    std::stable_sort(segs.begin(), segs.end(), [&](unsigned a, unsigned b) {
      if (a == 0 || b == 0)
        return a == 0 && b != 0;
      return posOf(f.segs[a].loc) < posOf(f.segs[b].loc);
    });
    for (unsigned s : segs) {
      std::vector<unsigned> mine;
      if (auto found = segPieces_.find({fi, s}); found != segPieces_.end())
        mine = found->second;
      std::vector<std::pair<std::string, unsigned>> byContext;
      for (unsigned i : mine)
        byContext.push_back({contextOf(i), i});
      std::sort(byContext.begin(), byContext.end());
      for (size_t k = 0; k < mine.size(); ++k)
        mine[k] = byContext[k].second;
      std::string title = segTitle(f, s);
      if (mine.empty()) {
        if (!f.isInline(s))
          continue; // never runs
        title +=
            f.asyncGen ? ": runs inside the loop that iterates it" : ": runs inside its caller";
      } else if (mine.size() == 1 && pieces_[mine[0]].root != 0) {
        title += " (in " + contextOf(mine[0]) + ")";
      }
      out << "  " << title << "\n";
      // What it does, with `this` as the code writes it.
      std::vector<Touch> list = touches(f.segs[s].effects);
      std::stable_sort(list.begin(), list.end(), [&](const Touch &a, const Touch &b) {
        Pos pa = posOf(a.ats.front().first), pb = posOf(b.ats.front().first);
        for (const auto &[at, via] : a.ats)
          if (posOf(at) < pa)
            pa = posOf(at);
        for (const auto &[at, via] : b.ats)
          if (posOf(at) < pb)
            pb = posOf(at);
        if (pa < pb || pb < pa)
          return pa < pb;
        return locName(a.loc) < locName(b.loc);
      });
      for (const Touch &t : list) {
        if (t.read)
          out << "    reads  " << locName(t.loc) << " (" << positions(t.readAts) << ")\n";
        if (t.write)
          out << "    writes " << locName(t.loc) << " (" << positions(t.writeAts) << ")\n";
      }
      if (mine.size() == 1) {
        for (const std::string &line : decisionLines(mine[0]))
          out << "    " << line << "\n";
        continue;
      }
      for (unsigned i : mine) {
        out << "    in " << contextOf(i) << ":\n";
        for (const std::string &line : decisionLines(i))
          out << "      " << line << "\n";
      }
    }
  }
}

ParallelPlan Analysis::plan() const {
  ParallelPlan plan;
  for (size_t r = 0; r < roots_.size(); ++r) {
    const Root &root = roots_[r];
    ParallelPlan::Task task;
    task.fn = fns_[root.fn].decl;
    task.parent = root.parent;
    task.several = root.multi;
    task.joinedAt = root.join;
    if (root.rec >= 0)
      task.site = fns_[root.callerFn].calls[root.rec].site;
    plan.tasks.push_back(task);
  }
  for (unsigned i = 0; i < pieces_.size(); ++i) {
    const PieceInst &p = pieces_[i];
    ParallelPlan::Piece piece;
    piece.fn = fns_[p.fn].decl;
    piece.start = fns_[p.fn].segs[p.seg].at;
    piece.task = unsigned(p.root);
    piece.alone = decisions_[i].alone;
    for (const Partner &partner : decisions_[i].partners)
      piece.inOrderWith.push_back(partner.piece);
    piece.orderedFrom = decisions_[i].from;
    piece.orderedTo = decisions_[i].to;
    piece.orderedAt = decisions_[i].ats;
    plan.pieces.push_back(std::move(piece));
  }
  return plan;
}

} // namespace

ParallelPlan planParallel(const SourceFile &program, const llvm::SourceMgr &sources) {
  Analysis analysis(program, sources);
  analysis.run();
  return analysis.plan();
}

void explainParallel(const SourceFile &program, const llvm::SourceMgr &sources,
                     llvm::raw_ostream &out) {
  Analysis analysis(program, sources);
  analysis.run();
  analysis.print(out);
}

} // namespace tz
