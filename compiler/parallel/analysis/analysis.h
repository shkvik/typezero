#pragma once

#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis_types.h"
#include "compiler/parallel/builtin_effects/builtin_effects.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/escape_scan/escape_scan.h"
#include "compiler/parallel/explain_names/explain_names.h"
#include "compiler/parallel/explain_titles/explain_titles.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/ownership/ownership.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace tz::parallel {

class Analysis {
public:
  Analysis(const SourceFile &sourceFile, const llvm::SourceMgr &sourceManager);
  Analysis(const Analysis &) = delete;
  Analysis &operator=(const Analysis &) = delete;
  void run();
  ParallelPlan plan() const;
  void explain(llvm::raw_ostream &out);

private:
  bool isLibrary(const FunctionDecl *function) const;
  void resource(Res res, bool write, llvm::SMLoc at);
  const char *intern(llvm::StringRef name);
  bool shared(const VarSymbol *v) const;
  const FunctionDecl *thisOwnerOf(const FunctionDecl *function) const;
  std::optional<Ref> classRefOrNone(const FnInfo &f) const;
  unsigned startNode(int root) const;
  static bool inOtherFile(const std::string &where);
  static std::string atText(const std::string &where);
  static std::string lineText(const std::string &where);
  const SourceFile &program;
  const llvm::SourceMgr &sources;
  FunctionTable fns;
  llvm::DenseMap<const FunctionDecl *, FunctionId> fnIndex;
  std::deque<FunctionId> worklist;
  llvm::StringSet<> names;
  const char *star = nullptr;
  const char *length = nullptr;
  const char *inner = nullptr;
  const char *state = nullptr;
  const char *listenersKey = nullptr;
  llvm::DenseMap<const VarSymbol *, const Expr *> declInit;
  llvm::DenseMap<const Expr *, llvm::StringRef> siteName;
  std::map<const Type *, std::vector<FunctionId>> values;
  llvm::DenseSet<FunctionId> valueFns;
  llvm::DenseSet<FunctionId> unknownFns;
  bool unknownAnyFn = false;
  std::vector<std::pair<const char *, const char *>> libRanges;
  std::vector<FunctionId> listeners[ChannelCount];
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
  llvm::DenseMap<const AwaitExpr *, AwaitInfo> awaits;
  llvm::DenseMap<const AwaitExpr *, JoinInfo> joins;
  std::vector<const ClassDecl *> classes;
  std::vector<const ClassDecl *> programClasses;
  llvm::DenseSet<const ClassDecl *> classSeen;
  std::map<const FunctionDecl *, Escapes> escapes;
  llvm::DenseMap<const ClassDecl *, bool> leaks;
  std::map<std::pair<const Type *, const Type *>, bool> alias;
  std::map<const Type *, std::vector<const Type *>> reach;
  std::map<std::pair<FunctionId, FunctionId>, bool> fnOnce;
  std::map<FunctionId, bool> rejectionEnds;
  FunctionId fi = kModuleFunction;
  FnInfo *fn = nullptr;
  Active cur;
  std::vector<Target> targets;
  unsigned loops = 0;
  llvm::StringRef label;
  std::vector<const Stmt *> anchors;
  llvm::DenseSet<const char *> partedAnchors;
  std::vector<OpenJoin> openJoins;
  std::vector<Activation> acts;
  std::map<std::tuple<FunctionId, int, std::optional<Ref>, bool>, unsigned> actIndex;
  std::vector<std::tuple<unsigned, unsigned, unsigned>> inlineEdges;
  std::vector<std::tuple<unsigned, unsigned, unsigned>> edges;
  std::vector<std::map<const VarSymbol *, std::set<const FunctionDecl *>>> bindings;
  std::vector<bool> reached;
  std::vector<Root> roots;
  std::vector<PieceInst> pieceInstances;
  std::map<std::tuple<FunctionId, unsigned, int, std::optional<Ref>>, unsigned> pieceIndex;
  std::vector<std::vector<unsigned>> actPieces;
  std::map<std::pair<FunctionId, int>, std::vector<unsigned>> actsByTask;
  const std::vector<unsigned> &actsOf(FunctionId function, int root) const;
  std::map<std::tuple<int, FunctionId, std::optional<Ref>, FunctionId, unsigned>, int> rootIndex;
  std::map<std::pair<FunctionId, unsigned>, std::vector<unsigned>> segPieces;
  std::map<std::tuple<FunctionId, unsigned, int>, unsigned> sameTask;
  std::vector<std::map<unsigned, std::vector<unsigned>>> flow;
  std::map<std::pair<unsigned, unsigned>, Entry> entries;
  std::vector<std::map<std::pair<unsigned, unsigned>, llvm::SMLoc>> flowFrom;
  std::map<FunctionId, std::vector<Pos>> steps;
  std::vector<std::vector<Touch>> pieceTouches;
  std::vector<Decision> decisions;
  mutable std::vector<std::string> rootTitles;
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
  CodeWalk codeWalk;
  ExprWalk exprWalk;
  CallWalk callWalk;
  LibraryEffects libraryEffects;
  BuiltinEffects builtinCalls;
  AliasAnalysis aliasAnalysis;
  EscapeScan escapeScan;
  CallResolver callResolver;
  Pieces pieces;
  Conflicts conflicts;
  Explainer explainer;
  ExplainNames explainNames;
  ExplainTitles explainTitles;
  Ownership ownership;
};

}
