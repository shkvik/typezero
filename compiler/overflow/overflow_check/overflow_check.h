#pragma once

#include "compiler/overflow/value_range/value_range.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace tz {

void checkIntOverflow(SourceFile &program, Diagnostics &diags);

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
  OverflowCheck(SourceFile &program, Diagnostics &diags);
  void run();

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
  Diagnostics &diags_;
  bool declaring_ = true;
  const FunctionDecl *scanOwner_ = nullptr;
  std::vector<const FunctionDecl *> functions_;
  llvm::DenseSet<const FunctionDecl *> functionSet_;
  llvm::DenseSet<const FunctionDecl *> inlined_;
  llvm::DenseSet<const FunctionDecl *> unknownCallers_;
  llvm::DenseMap<const VarSymbol *, VarInfo> vars_;
  llvm::DenseSet<const VarSymbol *> constants_;
  llvm::DenseMap<const Expr *, std::pair<const VarDeclarator *, bool>> inits_;
  llvm::DenseMap<const Expr *, const VarSymbol *> selfAssigned_;
  llvm::DenseSet<const VarSymbol *> accumulators_;
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
    if (!quiet_) {
      slot.val = join(slot.val, v);
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
  std::vector<FunctionMemo> memos_;
  FunctionMemo *reading_ = nullptr;
  std::set<SummaryKey> changed_;
  void noteRead(const void *map, const void *key) {
    if (reading_) {
      reading_->reads.insert({map, key, ""});
    }
  }
  void noteRead(const void *map, const std::pair<const Type *, std::string> &key) {
    if (reading_) {
      reading_->reads.insert({map, key.first, key.second});
    }
  }
  bool readsChanged(const FunctionMemo &memo) const {
    for (const SummaryKey &key : memo.reads) {
      if (changed_.count(key) != 0) {
        return true;
      }
    }

    return false;
  }
  static void joinInto(Summaries &into, const Summaries &from);
  template <typename Map, typename Key> std::optional<Val> summary(const Map &map, const Key &key) {
    noteRead(&map, key);
    if (round_ == 0) {
      return std::nullopt;
    }
    auto found = map.find(key);
    if (found == map.end()) {
      return std::nullopt;
    }

    return found->second.val;
  }
  Summaries cur_, next_;
  unsigned round_ = 0;
  bool reporting_ = false;
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
    State breaks = deadState(), conts = deadState();
    unsigned quiet = 0;
  };
  struct LoopSpec {
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
  unsigned quiet_ = 0;
  uint64_t work_ = 0;
  static constexpr uint64_t kWorkBudget = 2000000;
  bool overBudget() const { return work_ > kWorkBudget; }
  const FunctionDecl *owner_ = nullptr;
  std::vector<Frame> frames_;
  std::vector<Target> targets_;
  uint64_t bases_ = 0;
  std::deque<Reason> reasons_;
  std::map<std::pair<const char *, std::string>, const Reason *> reasonIndex_;
  struct Warning {
    llvm::SMLoc loc;
    std::string message;
  };
  std::vector<Warning> warnings_;
  llvm::DenseMap<const Expr *, bool> proofs_;

public:
  ~OverflowCheck();

  ProgramScan &programScan() const { return *programScanPart_; }
  Slots &slots() const { return *slotsPart_; }
  CallRanges &callRanges() const { return *callRangesPart_; }
  Executor &executor() const { return *executorPart_; }
  LoopRanges &loopRanges() const { return *loopRangesPart_; }
  Narrowing &narrowing() const { return *narrowingPart_; }
  Evaluator &evaluator() const { return *evaluatorPart_; }
  AssignEvaluator &assignEvaluator() const { return *assignEvaluatorPart_; }
  BuiltinEvaluator &builtinEvaluator() const { return *builtinEvaluatorPart_; }
  OverflowWarnings &overflowWarnings() const { return *overflowWarningsPart_; }

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
  std::unique_ptr<ProgramScan> programScanPart_;
  std::unique_ptr<Slots> slotsPart_;
  std::unique_ptr<CallRanges> callRangesPart_;
  std::unique_ptr<Executor> executorPart_;
  std::unique_ptr<LoopRanges> loopRangesPart_;
  std::unique_ptr<Narrowing> narrowingPart_;
  std::unique_ptr<Evaluator> evaluatorPart_;
  std::unique_ptr<AssignEvaluator> assignEvaluatorPart_;
  std::unique_ptr<BuiltinEvaluator> builtinEvaluatorPart_;
  std::unique_ptr<OverflowWarnings> overflowWarningsPart_;
};

}
