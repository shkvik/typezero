#pragma once

#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
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
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz::parallel {

class CodeWalk;
class ExprWalk;
class CallWalk;
class LibraryEffects;
class BuiltinEffects;
class AliasAnalysis;
class EscapeScan;
class CallResolver;
class Pieces;
class Conflicts;
class Explainer;
class ExplainNames;
class ExplainTitles;

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
  std::map<const Type *, std::vector<unsigned>> values_;
  llvm::DenseSet<unsigned> valueFns_;
  std::vector<std::pair<const char *, const char *>> libRanges_;
  std::vector<unsigned> listeners_[ChannelCount];
  std::vector<const Type *> listenerTypes_[ChannelCount];
  llvm::DenseSet<const VarSymbol *> resolvers_;
  llvm::DenseSet<const VarSymbol *> ownIndex_;
  llvm::DenseMap<const FunctionDecl *, const AwaitExpr *> joinReturns_;
  struct AwaitInfo {
    unsigned fn = 0;
  };
  llvm::DenseMap<const AwaitExpr *, AwaitInfo> awaits_;
  struct JoinInfo {
    unsigned fn = 0;
    Active segs;
  };
  llvm::DenseMap<const AwaitExpr *, JoinInfo> joins_;
  std::vector<const ClassDecl *> classes_;
  std::vector<const ClassDecl *> programClasses_;
  llvm::DenseSet<const ClassDecl *> classSeen_;
  std::map<const FunctionDecl *, Escapes> escapes_;
  llvm::DenseMap<const ClassDecl *, bool> leaks_;
  std::map<std::pair<const Type *, const Type *>, bool> alias_;
  std::map<const Type *, std::vector<const Type *>> reach_;
  std::map<std::pair<unsigned, unsigned>, bool> fnOnce_;
  std::map<unsigned, bool> rejectionEnds_;
  unsigned fi_ = 0;
  FnInfo *fn_ = nullptr;
  Active cur_;
  struct Target {
    llvm::StringRef label;
    bool loop = false;
    bool block = false;
    Active breaks, continues;
  };
  std::vector<Target> targets_;
  unsigned loops_ = 0;
  llvm::StringRef label_;
  struct OpenJoin {
    const AwaitExpr *await;
    Active active;
  };
  std::vector<OpenJoin> openJoins_;
  std::vector<Activation> acts_;
  std::map<std::tuple<unsigned, int, std::optional<Ref>, bool>, unsigned> actIndex_;
  std::vector<std::tuple<unsigned, unsigned, unsigned>> inlineEdges_;
  std::vector<Root> roots_;
  std::vector<PieceInst> pieces_;
  std::map<std::tuple<unsigned, unsigned, int, std::optional<Ref>>, unsigned> pieceIndex_;
  std::vector<std::vector<unsigned>> actPieces_;
  std::map<std::pair<unsigned, int>, std::vector<unsigned>> actsByTask_;
  const std::vector<unsigned> &actsOf(unsigned fn, int root) const {
    static const std::vector<unsigned> none;
    auto found = actsByTask_.find({fn, root});

    return found == actsByTask_.end() ? none : found->second;
  }
  std::map<std::tuple<int, unsigned, std::optional<Ref>, unsigned, unsigned>, int> rootIndex_;
  std::map<std::pair<unsigned, unsigned>, std::vector<unsigned>> segPieces_;
  std::map<std::tuple<unsigned, unsigned, int>, unsigned> sameTask_;
  std::vector<std::map<unsigned, std::vector<unsigned>>> flow_;
  struct Partner {
    unsigned piece;
    std::vector<Loc> locs;
  };
  struct Decision {
    bool alone = true;
    std::vector<Partner> partners;
    llvm::SMLoc from, to;
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
  BuiltinEffects &builtinCalls() const { return *builtinCallsPart_; }
  AliasAnalysis &aliasAnalysis() const { return *aliasAnalysisPart_; }
  EscapeScan &escapeScan() const { return *escapeScanPart_; }
  CallResolver &callResolver() const { return *callResolverPart_; }
  Pieces &pieces() const { return *piecesPart_; }
  Conflicts &conflicts() const { return *conflictsPart_; }
  Explainer &explainer() const { return *explainerPart_; }
  ExplainNames &explainNames() const { return *explainNamesPart_; }
  ExplainTitles &explainTitles() const { return *explainTitlesPart_; }

private:
  friend class CodeWalk;
  friend class ExprWalk;
  friend class CallWalk;
  friend class LibraryEffects;
  friend class BuiltinEffects;
  friend class AliasAnalysis;
  friend class EscapeScan;
  friend class CallResolver;
  friend class Pieces;
  friend class Conflicts;
  friend class Explainer;
  friend class ExplainNames;
  friend class ExplainTitles;
  std::unique_ptr<CodeWalk> codeWalkPart_;
  std::unique_ptr<ExprWalk> exprWalkPart_;
  std::unique_ptr<CallWalk> callWalkPart_;
  std::unique_ptr<LibraryEffects> libraryEffectsPart_;
  std::unique_ptr<BuiltinEffects> builtinCallsPart_;
  std::unique_ptr<AliasAnalysis> aliasAnalysisPart_;
  std::unique_ptr<EscapeScan> escapeScanPart_;
  std::unique_ptr<CallResolver> callResolverPart_;
  std::unique_ptr<Pieces> piecesPart_;
  std::unique_ptr<Conflicts> conflictsPart_;
  std::unique_ptr<Explainer> explainerPart_;
  std::unique_ptr<ExplainNames> explainNamesPart_;
  std::unique_ptr<ExplainTitles> explainTitlesPart_;
};

}
