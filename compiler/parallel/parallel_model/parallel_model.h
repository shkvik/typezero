#pragma once

#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SMLoc.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz::parallel {

enum class RefKind : uint8_t {
  Local,
  This,
  Site,
  Type,
  Owned,
};

struct OwnedRef {
  const VarSymbol *root = nullptr;
  const Type *type = nullptr;
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
  Var,
  Field,
  Elements,
  OwnCell,
  Contents,
  Resource,
};

enum class Res : uint8_t {
  Stdout,
  Stderr,
  Stdin,
  Files,
  Process,
  Metadata,
  Runtime,
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
  llvm::SMLoc at;
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

using Active = llvm::SmallVector<unsigned, 4>;

void merge(Active &into, llvm::ArrayRef<unsigned> from);

enum class SegKind : uint8_t {
  Start,
  Await,
  ForAwait,
  Yield,
};

struct Segment {
  SegKind kind = SegKind::Start;
  const Node *at = nullptr;
  llvm::SMLoc loc;
  bool delegate = false;
  Effects effects;
};

enum class CallKind : uint8_t {
  Call,
  Inline,
  Later,
  IterSync,
  IterAsync,
};

enum Channel : int { EmitterListeners, TargetListeners, ChannelCount };

enum class Mode : uint8_t {
  Sync,
  Await,
  Spawn,
  Callback,
  AsyncIter,
};

struct Callee {
  unsigned fn = 0;
  std::optional<Ref> self;
  Mode mode = Mode::Sync;
  bool byType = false;
  bool operator==(const Callee &o) const { return fn == o.fn && self == o.self && mode == o.mode; }
};

struct CallRec {
  CallKind kind = CallKind::Call;
  const Node *site = nullptr;
  const CallExpr *call = nullptr;
  llvm::SMLoc loc;
  Active segs;
  std::vector<Callee> callees;
  const Type *byType = nullptr;
  llvm::StringRef byName;
  const Type *byObject = nullptr;
  std::optional<Ref> bySelf;
  const AwaitExpr *await = nullptr;
  bool returned = false;
  bool voided = false;
  const AwaitExpr *join = nullptr;
  bool repeated = false;
  bool inLoop = false;
  bool interval = false;
  bool handled = false;
  bool served = false;
  int channel = -1;
  enum class Reflect : uint8_t { None, Apply, Construct } reflect = Reflect::None;
};

struct FnInfo {
  const FunctionDecl *decl = nullptr;
  const FunctionDecl *thisOwner = nullptr;
  bool async = false, asyncGen = false, syncGen = false;
  std::vector<Segment> segs;
  std::vector<CallRec> calls;
  llvm::DenseMap<const Node *, unsigned> segOf;
  llvm::DenseMap<std::pair<const void *, unsigned>, unsigned> callOf;
  Effects inlineEffects;
  std::vector<std::pair<unsigned, unsigned>> callers;
  std::vector<std::pair<unsigned, unsigned>> named;
  std::set<std::pair<unsigned, unsigned>> succ;
  bool isInline(unsigned seg) const { return seg == 0 || segs[seg].kind == SegKind::Yield; }
};

struct Use {
  const AwaitExpr *awaited = nullptr;
  bool returned = false;
  bool voided = false;
  const AwaitExpr *join = nullptr;
  bool lambda = false;
};

struct Activation {
  unsigned fn = 0;
  int root = 0;
  std::optional<Ref> self;
  bool callbackStart = false;
  bool rootStart = false;
  std::set<unsigned> hosts;
};

struct Root {
  unsigned fn = 0;
  std::optional<Ref> self;
  int parent = -1;
  unsigned callerFn = 0;
  int rec = -1;
  Mode mode = Mode::Callback;
  bool multi = false;
  bool recursive = false;
  const AwaitExpr *join = nullptr;
  std::vector<unsigned> spawnAt, interval;
  std::vector<std::pair<int, std::vector<unsigned>>> chain;
  const FunctionDecl *ownCell = nullptr;
};

struct PieceInst {
  unsigned fn = 0;
  unsigned seg = 0;
  int root = 0;
  std::optional<Ref> self;
};

struct Touch {
  Loc loc;
  bool read = false, write = false;
  std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> ats, readAts, writeAts;
};

}
