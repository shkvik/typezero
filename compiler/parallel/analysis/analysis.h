#pragma once

#include <memory>
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
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/parallel/parallel_model/parallel_model.h"

namespace tz::parallel {

class CodeWalk;
class ExprWalk;
class CallWalk;
class LibraryEffects;
class AliasAnalysis;
class CallResolver;
class Pieces;
class Conflicts;
class Explainer;

class Analysis {
public:
  Analysis(const SourceFile &program, const llvm::SourceMgr &sources);
  void run();
  ParallelPlan plan() const;
private:
  bool isLibrary(const FunctionDecl *fn) const;
  void resource(Res res, bool write, llvm::SMLoc at);
  const char *intern(llvm::StringRef name) { return names_.insert(name).first->getKeyData(); }
  bool shared(const VarSymbol *v) const { return (v->isGlobal || v->captured) && v->reassigned; }
  using Escapes = llvm::DenseSet<const VarSymbol *>;
  const FunctionDecl *thisOwnerOf(const FunctionDecl *fn) const;
  std::optional<Ref> classRefOrNone(const FnInfo &f) const;
  unsigned startNode(int root) const { return unsigned(pieces_.size()) + unsigned(root); }
  struct Pos {
    unsigned buffer = 0;
    unsigned offset = 0;
    bool operator<(const Pos &o) const {
      return buffer != o.buffer ? buffer < o.buffer : offset < o.offset;
    }
  };
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
  // The built-in modules written in tz (library/): the program's calls into
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

public:
  ~Analysis();

  CodeWalk &codeWalk() const { return *codeWalkPart_; }
  ExprWalk &exprWalk() const { return *exprWalkPart_; }
  CallWalk &callWalk() const { return *callWalkPart_; }
  LibraryEffects &libraryEffects() const { return *libraryEffectsPart_; }
  AliasAnalysis &aliasAnalysis() const { return *aliasAnalysisPart_; }
  CallResolver &callResolver() const { return *callResolverPart_; }
  Pieces &pieces() const { return *piecesPart_; }
  Conflicts &conflicts() const { return *conflictsPart_; }
  Explainer &explainer() const { return *explainerPart_; }

private:
  friend class CodeWalk;
  friend class ExprWalk;
  friend class CallWalk;
  friend class LibraryEffects;
  friend class AliasAnalysis;
  friend class CallResolver;
  friend class Pieces;
  friend class Conflicts;
  friend class Explainer;
  std::unique_ptr<CodeWalk> codeWalkPart_;
  std::unique_ptr<ExprWalk> exprWalkPart_;
  std::unique_ptr<CallWalk> callWalkPart_;
  std::unique_ptr<LibraryEffects> libraryEffectsPart_;
  std::unique_ptr<AliasAnalysis> aliasAnalysisPart_;
  std::unique_ptr<CallResolver> callResolverPart_;
  std::unique_ptr<Pieces> piecesPart_;
  std::unique_ptr<Conflicts> conflictsPart_;
  std::unique_ptr<Explainer> explainerPart_;
};

} // namespace tz::parallel
