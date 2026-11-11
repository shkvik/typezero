#pragma once

#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/program/library_ranges/library_ranges.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/MapVector.h"
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace tz {

struct Program;

class ProgramScan;
class Slots;
class CallRanges;
class Executor;
class LoopRanges;
class Narrowing;
class Evaluator;
class AssignEvaluator;
class BuiltinEvaluator;
class OverflowWarnings;

class OverflowCheck {
public:
  using ResultOverflowChecked = Result<void, std::vector<Diagnostic>>;

  OverflowCheck(Program &checked, const LibraryRanges &library);
  ResultOverflowChecked run();

private:
  struct VarInfo {
    const FunctionDecl *owner = nullptr;
    bool declared = false;
    bool shared = false;
    bool writtenElsewhere = false;
    bool aliased = false;
    const VarDeclarator *decl = nullptr;
    bool isConst = false;
  };
  SourceFile &program_;
  llvm::SourceMgr &sourceMgr;
  const LibraryRanges &library;
  bool declaring = true;
  const FunctionDecl *scanOwner = nullptr;
  std::vector<const FunctionDecl *> functions_;
  llvm::DenseSet<const FunctionDecl *> functionSet;
  llvm::DenseSet<const FunctionDecl *> inlined;
  llvm::DenseSet<const FunctionDecl *> unknownCallers;
  llvm::DenseMap<const VarSymbol *, VarInfo> vars_;
  llvm::DenseSet<const VarSymbol *> constants;
  llvm::DenseMap<const Expr *, std::pair<const VarDeclarator *, bool>> inits;
  llvm::DenseMap<const Expr *, const VarSymbol *> selfAssigned;
  llvm::DenseSet<const VarSymbol *> accumulators;
  struct Slot {
    Val val;
    unsigned growths = 0;
    const Type *type = nullptr;
  };
  struct Pred {
    std::vector<Num> whenTrue, whenFalse;
  };
  struct Summaries {
    llvm::DenseMap<const FunctionDecl *, std::vector<Slot>> params;
    llvm::DenseMap<const FunctionDecl *, Slot> rets;
    llvm::DenseMap<const VarSymbol *, Slot> vars;
    std::map<std::pair<const Type *, std::string>, Slot> fields;
    llvm::DenseMap<const Type *, Slot> elements;
    llvm::DenseMap<const Type *, Slot> dicts;
    llvm::DenseMap<const FunctionDecl *, Pred> preds;
    std::map<std::pair<const FunctionDecl *, size_t>,
             std::vector<std::pair<llvm::SMLoc, const Reason *>>>
        dangerousCalls;
  };
  void contribute(Slot &slot, const Val &v) {
    if (!this->quiet) {
      slot.val = ValueRange::join(slot.val, v);
    }
  }
  struct SummaryKey {
    const void *map = nullptr;
    const void *key = nullptr;
    std::string field;

    bool operator<(const SummaryKey &other) const {
      return std::tie(map, key, field) < std::tie(other.map, other.key, other.field);
    }
  };
  struct FunctionMemo {
    Summaries made;
    std::set<SummaryKey> reads;
    uint64_t work = 0;
    bool valid = false;
  };
  std::vector<FunctionMemo> memos;
  FunctionMemo *reading = nullptr;
  std::set<SummaryKey> changed;
  void noteRead(const void *map, const void *key) {
    if (this->reading) {
      this->reading->reads.insert({map, key, ""});
    }
  }
  void noteRead(const void *map, const std::pair<const Type *, std::string> &key) {
    if (this->reading) {
      this->reading->reads.insert({map, key.first, key.second});
    }
  }
  bool readsChanged(const FunctionMemo &memo) const {
    for (const SummaryKey &key : memo.reads) {
      if (this->changed.count(key) != 0) {
        return true;
      }
    }

    return false;
  }
  void joinInto(Summaries &into, const Summaries &from) const;
  template <typename Map, typename Key> std::optional<Val> summary(const Map &map, const Key &key) {
    noteRead(&map, key);
    if (this->round == 0) {
      return std::nullopt;
    }
    auto found = map.find(key);
    if (found == map.end()) {
      return std::nullopt;
    }

    return found->second.val;
  }
  Summaries cur, next;
  unsigned round = 0;
  bool reporting = false;
  struct Frame {
    const FunctionDecl *fn = nullptr;
    bool callback = false;
    State returned;
    Val result;
    size_t targets = 0;
    unsigned quiet = 0;
  };
  struct Target {
    bool loop = false;
    bool block = false;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    State breaks = ValueRange::deadState(), conts = ValueRange::deadState();
    unsigned quiet = 0;
  };
  struct LoopSpec {
    const Stmt *stmt = nullptr;
    llvm::SMLoc loc;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    const Expr *cond = nullptr;
    const Expr *condAfter = nullptr;
    const Expr *step = nullptr;
    bool exitAtHead = false;
    std::function<void(State &)> body;
    Num passes;
    bool openEnded = false;
    bool simple = false;
    std::optional<Val> source;
  };
  struct Place {
    enum class Kind { None, Var, Field, Element, Length } kind = Kind::None;
    const VarSymbol *var = nullptr;
    const Type *objectType = nullptr;
    std::string field;
    const Type *type = nullptr;
    bool append = false;
  };
  struct Narrowable {
    const VarSymbol *var = nullptr;
    bool length = false;
    bool integer = false;
    double scale = 1, offset = 0;
    bool square = false;
    bool abs = false;
  };
  unsigned quiet = 0;
  uint64_t work = 0;
  static constexpr uint64_t kWorkBudget = 2000000;
  bool overBudget() const {
    return this->work > kWorkBudget;
  }
  const FunctionDecl *owner = nullptr;
  std::vector<Frame> frames;
  std::vector<Target> targets;
  uint64_t bases_ = 0;
  std::deque<Reason> reasons;
  std::map<std::pair<const char *, std::string>, const Reason *> reasonIndex;
  struct Warning {
    llvm::SMLoc loc;
    std::string message;
  };
  std::vector<Warning> warnings;
  llvm::DenseMap<const Expr *, bool> proofs;
  struct LongLoop {
    bool bounded = true;
    const Reason *why = nullptr;
  };
  llvm::MapVector<const Stmt *, LongLoop> loops;
  struct Recursion {
    std::vector<const CallExpr *> self;
    int counter = -1;
    double step = 0;
  };
  llvm::MapVector<const FunctionDecl *, Recursion> recursions;
  struct CallArgs {
    const FunctionDecl *target = nullptr;
    std::vector<Num> args;
  };
  llvm::MapVector<const CallExpr *, CallArgs> recursiveCalls;

public:
  ~OverflowCheck();

  ProgramScan &programScan() const {
    return *this->programScanPart;
  }
  Slots &slots() const {
    return *this->slotsPart;
  }
  CallRanges &callRanges() const {
    return *this->callRangesPart;
  }
  Executor &executor() const {
    return *this->executorPart;
  }
  LoopRanges &loopRanges() const {
    return *this->loopRangesPart;
  }
  Narrowing &narrowing() const {
    return *this->narrowingPart;
  }
  Evaluator &evaluator() const {
    return *this->evaluatorPart;
  }
  AssignEvaluator &assignEvaluator() const {
    return *this->assignEvaluatorPart;
  }
  BuiltinEvaluator &builtinEvaluator() const {
    return *this->builtinEvaluatorPart;
  }
  OverflowWarnings &overflowWarnings() const {
    return *this->overflowWarningsPart;
  }

private:
  friend class ProgramScan;
  friend class Slots;
  friend class CallRanges;
  friend class Executor;
  friend class LoopRanges;
  friend class Narrowing;
  friend class Evaluator;
  friend class AssignEvaluator;
  friend class BuiltinEvaluator;
  friend class OverflowWarnings;
  std::unique_ptr<ProgramScan> programScanPart;
  std::unique_ptr<Slots> slotsPart;
  std::unique_ptr<CallRanges> callRangesPart;
  std::unique_ptr<Executor> executorPart;
  std::unique_ptr<LoopRanges> loopRangesPart;
  std::unique_ptr<Narrowing> narrowingPart;
  std::unique_ptr<Evaluator> evaluatorPart;
  std::unique_ptr<AssignEvaluator> assignEvaluatorPart;
  std::unique_ptr<BuiltinEvaluator> builtinEvaluatorPart;
  std::unique_ptr<OverflowWarnings> overflowWarningsPart;
};

}
