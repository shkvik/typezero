#pragma once

#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/types/types.h"
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
#include "compiler/program/ast/ast.h"
#include "llvm/Support/SMLoc.h"

namespace tz::parallel {

// What the analysis records: references and places, accesses, segments of code,
// calls and functions, tasks and pieces.

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
    if (kind != o.kind) {
      return kind < o.kind;
    }
    if (ref != o.ref) {
      return ref < o.ref;
    }
    if (key != o.key) {
      return std::less<const void *>()(key, o.key);
    }
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
    if (!(loc == o.loc)) {
      return loc < o.loc;
    }
    if (write != o.write) {
      return write < o.write;
    }
    return std::less<const char *>()(at.getPointer(), o.at.getPointer());
  }
};

using Effects = std::set<Access>;

// The pieces active at a point of a function, by the number of the segment
// where each starts; kept sorted.
using Active = llvm::SmallVector<unsigned, 4>;

void merge(Active &into, llvm::ArrayRef<unsigned> from);

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

} // namespace tz::parallel
