#pragma once

#include "compiler/overflow/value_range/value_range.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include <memory>

namespace tz {

// Warns where int arithmetic may overflow (docs/small.md, section 6). An int
// wraps around at run time, where JavaScript's numbers go on growing, so the
// compiler points at the operations whose values it cannot keep within the
// int range: values from input, files, JSON or parseInt and what is computed
// from them, and counters of loops without a bound. It follows the ranges of
// values through the whole program checked by Checker; the warnings do not stop
// the build.
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
    // The function, run on its own, that declares the variable; null for
    // main's code.
    const FunctionDecl *owner = nullptr;
    bool declared = false;
    // Another function uses it, or assigns to it: then its owner reads it
    // from the summary too.
    bool shared = false;
    bool writtenElsewhere = false;
    // An array that may have other names: what is stored through those
    // reaches it too.
    bool aliased = false;
    const VarDeclarator *decl = nullptr;
    bool isConst = false;
  };
  SourceFile &program_;
  Diagnostics &diags_;
  // Whether the pre-pass is declaring (first walk) or using (second walk).
  bool declaring_ = true;
  const FunctionDecl *scanOwner_ = nullptr;
  std::vector<const FunctionDecl *> functions_;
  llvm::DenseSet<const FunctionDecl *> functionSet_;
  llvm::DenseSet<const FunctionDecl *> inlined_;
  // Functions used as values: any code may call them, with anything.
  llvm::DenseSet<const FunctionDecl *> unknownCallers_;
  llvm::DenseMap<const VarSymbol *, VarInfo> vars_;
  // Constants whose initializers are constants: Checker computes and checks
  // the operations of those.
  llvm::DenseSet<const VarSymbol *> constants_;
  // For hints: the declarator an expression initializes, and the variable
  // that `x = x + ...` assigns its sum to.
  llvm::DenseMap<const Expr *, std::pair<const VarDeclarator *, bool>> inits_;
  llvm::DenseMap<const Expr *, const VarSymbol *> selfAssigned_;
  llvm::DenseSet<const VarSymbol *> accumulators_;
  struct Slot {
    Val val;
    unsigned growths = 0;
    const Type *type = nullptr; // of a field
  };
  struct Pred {
    std::vector<Num> whenTrue, whenFalse;
  };
  struct Summaries {
    llvm::DenseMap<const FunctionDecl *, std::vector<Slot>> params;
    llvm::DenseMap<const FunctionDecl *, Slot> rets;
    llvm::DenseMap<const VarSymbol *, Slot> vars;
    std::map<std::pair<const Type *, std::string>, Slot> fields;
    llvm::DenseMap<const Type *, Slot> elements; // by element type
    llvm::DenseMap<const Type *, Slot> dicts;    // the values, by dictionary type
    llvm::DenseMap<const FunctionDecl *, Pred> preds;
    // The calls that pass a parameter a dangerous value, and its reason.
    std::map<std::pair<const FunctionDecl *, size_t>,
             std::vector<std::pair<llvm::SMLoc, const Reason *>>>
        dangerousCalls;
  };
  // Adds `v` to the summary `slot` of the round being run.
  void contribute(Slot &slot, const Val &v) {
    if (!quiet_) {
      slot.val = join(slot.val, v);
    }
  }
  // The value of a summary: not known in the first round, which only finds
  // which summaries get values; nothing yet for one that gets them later.
  template <typename Map, typename Key>
  std::optional<Val> summary(const Map &map, const Key &key) const {
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
  // Where control goes: a frame's returns, a loop's breaks and continues, a
  // try's catch. What a pass of a loop inside that is not the loop's last
  // pass sends there does not count (see quiet_): those passes run with
  // values that only find the loop's head.
  struct Frame {
    const FunctionDecl *fn = nullptr; // null: main
    bool callback = false;            // a callback run where it is passed
    State returned;                   // callback: the states at its returns
    Val result;                       // callback: what they give
    size_t targets = 0;               // its jump targets start there
    unsigned quiet = 0;
  };
  struct Target {
    bool loop = false;
    bool block = false; // a labeled statement that is no loop
    llvm::SmallVector<llvm::StringRef, 1> labels;
    State breaks = deadState(), conts = deadState();
    unsigned quiet = 0;
  };
  struct Try {
    State *thrown;
    unsigned quiet;
  };
  // A state that may go to the catch around, as where something throws.
  void mayThrow(const State &st) {
    if (!tries_.empty() && tries_.back().quiet == quiet_) {
      *tries_.back().thrown = join(*tries_.back().thrown, st);
    }
  }
  struct LoopSpec {
    llvm::SMLoc loc;
    llvm::SmallVector<llvm::StringRef, 1> labels;
    const Expr *cond = nullptr;      // tested before each pass
    const Expr *condAfter = nullptr; // tested after each pass (do-while)
    const Expr *step = nullptr;
    bool exitAtHead = false; // for-of and callbacks: may stop before any pass
    std::function<void(State &)> body;
    // A bound on the states at the head: the passes, and the state the loop
    // ends in after the last; empty: none.
    Num passes;
    // A loop that nothing in the program's data has to end: no condition,
    // or one that reads the input. Its counters may grow without a bound.
    bool openEnded = false;
    // No loop inside: a pass costs little, and the loop may run pass by
    // pass.
    bool simple = false;
  };
  // Where a value is stored: a variable, a field, an element of an array.
  struct Place {
    enum class Kind { None, Var, Field, Element, Length } kind = Kind::None;
    const VarSymbol *var = nullptr; // Var; the array of Element and Length, if a variable
    const Type *objectType = nullptr;
    std::string field;
    const Type *type = nullptr;
    bool append = false;
  };
  // A variable or the length of one that a check can narrow: the value
  // checked is `var * scale + offset`, or with `square` var * var.
  struct Narrowable {
    const VarSymbol *var = nullptr;
    bool length = false;
    bool integer = false;
    double scale = 1, offset = 0;
    bool square = false;
  };
  // Passes of loops that are not the last: nothing is recorded in them.
  unsigned quiet_ = 0;
  // The passes of loop bodies run in this round: past a budget, loops get
  // fewer of them, so that a program of many nested loops compiles in time.
  uint64_t work_ = 0;
  bool overBudget() const { return work_ > 2000000; }
  const FunctionDecl *owner_ = nullptr;
  std::vector<Frame> frames_;
  std::vector<Target> targets_;
  std::vector<Try> tries_;
  uint64_t bases_ = 0;
  std::deque<Reason> reasons_;
  std::map<std::pair<const char *, std::string>, const Reason *> reasonIndex_;
  struct Warning {
    llvm::SMLoc loc;
    std::string message;
  };
  std::vector<Warning> warnings_;

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

} // namespace tz
