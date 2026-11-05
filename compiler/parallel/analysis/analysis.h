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
class Ownership;

class Analysis {
public:
  Analysis(const SourceFile &sourceFile, const llvm::SourceMgr &sourceManager);
  void run();
  ParallelPlan plan() const;

private:
  bool isLibrary(const FunctionDecl *function) const;
  void resource(Res res, bool write, llvm::SMLoc at);
  const char *intern(llvm::StringRef name) { return this->names.insert(name).first->getKeyData(); }
  bool shared(const VarSymbol *v) const { return (v->isGlobal || v->captured) && v->reassigned; }
  using Escapes = llvm::DenseSet<const VarSymbol *>;
  const FunctionDecl *thisOwnerOf(const FunctionDecl *function) const;
  std::optional<Ref> classRefOrNone(const FnInfo &f) const;
  unsigned startNode(int root) const {
    return unsigned(this->pieceInstances.size()) + unsigned(root);
  }
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
  const SourceFile &program;
  const llvm::SourceMgr &sources;
  std::deque<FnInfo> fns;
  llvm::DenseMap<const FunctionDecl *, unsigned> fnIndex;
  std::deque<unsigned> worklist;
  llvm::StringSet<> names;
  const char *star = nullptr;
  const char *length = nullptr;
  const char *inner = nullptr;
  const char *state = nullptr;
  const char *listenersKey = nullptr;
  llvm::DenseMap<const VarSymbol *, const Expr *> declInit;
  llvm::DenseMap<const Expr *, llvm::StringRef> siteName;
  std::map<const Type *, std::vector<unsigned>> values;
  llvm::DenseSet<unsigned> valueFns;
  llvm::DenseSet<unsigned> unknownFns;
  bool unknownAnyFn = false;
  std::vector<std::pair<const char *, const char *>> libRanges;
  std::vector<unsigned> listeners[ChannelCount];
  std::vector<const Type *> listenerTypes[ChannelCount];
  llvm::DenseSet<const VarSymbol *> resolvers;
  llvm::DenseSet<const VarSymbol *> ownIndex;
  std::deque<OwnedRef> ownedRefs;
  std::map<std::tuple<const VarSymbol *, const Type *, std::vector<FieldKey>, bool>,
           const OwnedRef *>
      ownedRefIndex;
  llvm::DenseSet<const VarSymbol *> ownedParams;
  std::vector<std::pair<const Type *, const FunctionDecl *>> poolCreates;
  bool poolOpen = false;
  llvm::DenseMap<const FunctionDecl *, const AwaitExpr *> joinReturns;
  struct AwaitInfo {
    unsigned fn = 0;
  };
  llvm::DenseMap<const AwaitExpr *, AwaitInfo> awaits;
  struct JoinInfo {
    unsigned fn = 0;
    Active segs;
  };
  llvm::DenseMap<const AwaitExpr *, JoinInfo> joins;
  std::vector<const ClassDecl *> classes;
  std::vector<const ClassDecl *> programClasses;
  llvm::DenseSet<const ClassDecl *> classSeen;
  std::map<const FunctionDecl *, Escapes> escapes;
  llvm::DenseMap<const ClassDecl *, bool> leaks;
  std::map<std::pair<const Type *, const Type *>, bool> alias;
  std::map<const Type *, std::vector<const Type *>> reach;
  std::map<std::pair<unsigned, unsigned>, bool> fnOnce;
  std::map<unsigned, bool> rejectionEnds;
  unsigned fi = 0;
  FnInfo *fn = nullptr;
  Active cur;
  struct Target {
    llvm::StringRef label;
    bool loop = false;
    bool block = false;
    Active breaks, continues;
  };
  std::vector<Target> targets;
  unsigned loops = 0;
  llvm::StringRef label;
  std::vector<const Stmt *> anchors;
  llvm::DenseSet<const char *> partedAnchors;
  struct OpenJoin {
    const AwaitExpr *await;
    Active active;
  };
  std::vector<OpenJoin> openJoins;
  std::vector<Activation> acts;
  std::map<std::tuple<unsigned, int, std::optional<Ref>, bool>, unsigned> actIndex;
  std::vector<std::tuple<unsigned, unsigned, unsigned>> inlineEdges;
  std::vector<std::tuple<unsigned, unsigned, unsigned>> edges;
  std::vector<std::map<const VarSymbol *, std::set<const FunctionDecl *>>> bindings;
  std::vector<bool> reached;
  std::vector<Root> roots;
  std::vector<PieceInst> pieceInstances;
  std::map<std::tuple<unsigned, unsigned, int, std::optional<Ref>>, unsigned> pieceIndex;
  std::vector<std::vector<unsigned>> actPieces;
  std::map<std::pair<unsigned, int>, std::vector<unsigned>> actsByTask;
  const std::vector<unsigned> &actsOf(unsigned function, int root) const {
    static const std::vector<unsigned> none;
    auto found = this->actsByTask.find({function, root});

    return found == this->actsByTask.end() ? none : found->second;
  }
  std::map<std::tuple<int, unsigned, std::optional<Ref>, unsigned, unsigned>, int> rootIndex;
  std::map<std::pair<unsigned, unsigned>, std::vector<unsigned>> segPieces;
  std::map<std::tuple<unsigned, unsigned, int>, unsigned> sameTask;
  std::vector<std::map<unsigned, std::vector<unsigned>>> flow;
  struct Entry {
    llvm::SMLoc first, last;
    bool open = false;
  };
  std::map<std::pair<unsigned, unsigned>, Entry> entries;
  std::vector<std::map<std::pair<unsigned, unsigned>, llvm::SMLoc>> flowFrom;
  std::map<unsigned, std::vector<Pos>> steps;
  struct Partner {
    unsigned piece;
    std::vector<Loc> locs;
  };
  struct Decision {
    bool alone = true;
    std::vector<Partner> partners;
    llvm::SMLoc from, to;
    std::vector<llvm::SMLoc> ats;
    std::vector<WaitPlace> places;
    std::vector<llvm::SMLoc> waits;
    std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> inside;
  };
  std::vector<std::vector<Touch>> pieceTouches;
  std::vector<Decision> decisions;
  mutable std::vector<std::string> rootTitles;

public:
  ~Analysis();

  CodeWalk &codeWalk() const { return *this->codeWalkPart; }
  ExprWalk &exprWalk() const { return *this->exprWalkPart; }
  CallWalk &callWalk() const { return *this->callWalkPart; }
  LibraryEffects &libraryEffects() const { return *this->libraryEffectsPart; }
  BuiltinEffects &builtinCalls() const { return *this->builtinCallsPart; }
  AliasAnalysis &aliasAnalysis() const { return *this->aliasAnalysisPart; }
  EscapeScan &escapeScan() const { return *this->escapeScanPart; }
  CallResolver &callResolver() const { return *this->callResolverPart; }
  Pieces &pieces() const { return *this->piecesPart; }
  Conflicts &conflicts() const { return *this->conflictsPart; }
  Explainer &explainer() const { return *this->explainerPart; }
  ExplainNames &explainNames() const { return *this->explainNamesPart; }
  ExplainTitles &explainTitles() const { return *this->explainTitlesPart; }
  Ownership &ownership() const { return *this->ownershipPart; }

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
  friend class Ownership;
  std::unique_ptr<CodeWalk> codeWalkPart;
  std::unique_ptr<ExprWalk> exprWalkPart;
  std::unique_ptr<CallWalk> callWalkPart;
  std::unique_ptr<LibraryEffects> libraryEffectsPart;
  std::unique_ptr<BuiltinEffects> builtinCallsPart;
  std::unique_ptr<AliasAnalysis> aliasAnalysisPart;
  std::unique_ptr<EscapeScan> escapeScanPart;
  std::unique_ptr<CallResolver> callResolverPart;
  std::unique_ptr<Pieces> piecesPart;
  std::unique_ptr<Conflicts> conflictsPart;
  std::unique_ptr<Explainer> explainerPart;
  std::unique_ptr<ExplainNames> explainNamesPart;
  std::unique_ptr<ExplainTitles> explainTitlesPart;
  std::unique_ptr<Ownership> ownershipPart;
};

}
