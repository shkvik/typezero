#include "compiler/parallel/analysis/analysis.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
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
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
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

namespace tz::parallel {

using llvm::cast;

using llvm::dyn_cast;

using llvm::dyn_cast_or_null;

using llvm::isa;

const FunctionDecl *Analysis::thisOwnerOf(const FunctionDecl *function) const {
  for (; function; function = function->parent) {
    if (function->takesThis()) {
      return function;
    }
    if (!function->isArrow) {
      return nullptr;
    }
  }

  return nullptr;
}

void Analysis::run() {
  this->star = intern("*");
  this->length = intern("%length");
  this->inner = intern("%inner");
  this->state = intern("%state");
  this->listenersKey = intern("%listeners");
  codeWalk().discover(nullptr);
  callResolver().resolve();
  ownership().decide();
  callResolver().fold();
  ownership().rewrite();
  pieces().explore();
  pieces().findPieces();
  pieces().findHosts();
  pieces().buildFlow();
  pieces().findJoins();
  pieces().findBindings();
  conflicts().decide();
}

ParallelPlan Analysis::plan() const {
  ParallelPlan plan;
  for (size_t r = 0; r < this->roots.size(); ++r) {
    const Root &root = this->roots[r];
    ParallelPlan::Task task;
    task.fn = this->fns[root.fn].decl;
    task.parent = root.parent;
    task.several = root.multi;
    task.joinedAt = root.join;
    if (root.rec >= 0) {
      task.site = this->fns[root.callerFn].calls[root.rec].site;
    }
    plan.tasks.push_back(task);
  }
  for (unsigned i = 0; i < this->pieceInstances.size(); ++i) {
    const PieceInst &p = this->pieceInstances[i];
    ParallelPlan::Piece piece;
    piece.fn = this->fns[p.fn].decl;
    piece.start = this->fns[p.fn].segs[p.seg].at;
    piece.task = unsigned(p.root);
    piece.alone = this->decisions[i].alone;
    for (const Partner &partner : this->decisions[i].partners) {
      piece.inOrderWith.push_back(partner.piece);
    }
    piece.orderedFrom = this->decisions[i].from;
    piece.orderedTo = this->decisions[i].to;
    piece.orderedAt = this->decisions[i].waits;
    plan.pieces.push_back(std::move(piece));
  }
  plan.ownedParams.assign(this->ownedParams.begin(), this->ownedParams.end());

  return plan;
}

Analysis::Analysis(const SourceFile &sourceFile, const llvm::SourceMgr &sourceManager)
    : program(sourceFile), sources(sourceManager) {
  this->codeWalkPart = std::make_unique<CodeWalk>(*this);
  this->exprWalkPart = std::make_unique<ExprWalk>(*this);
  this->callWalkPart = std::make_unique<CallWalk>(*this);
  this->libraryEffectsPart = std::make_unique<LibraryEffects>(*this);
  this->builtinCallsPart = std::make_unique<BuiltinEffects>(*this);
  this->aliasAnalysisPart = std::make_unique<AliasAnalysis>(*this);
  this->escapeScanPart = std::make_unique<EscapeScan>(*this);
  this->callResolverPart = std::make_unique<CallResolver>(*this);
  this->piecesPart = std::make_unique<Pieces>(*this);
  this->conflictsPart = std::make_unique<Conflicts>(*this);
  this->explainerPart = std::make_unique<Explainer>(*this);
  this->explainNamesPart = std::make_unique<ExplainNames>(*this);
  this->explainTitlesPart = std::make_unique<ExplainTitles>(*this);
  this->ownershipPart = std::make_unique<Ownership>(*this);
  for (const Module *module : this->program.modules) {
    if (!module->builtin) {
      continue;
    }
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = this->sources.getMemoryBuffer(id);
      this->libRanges.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
    }
  }
  for (const Stmt *item : this->program.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item);
        cls && !cls->builtin && !libraryEffects().inLibrary(cls->loc())) {
      this->programClasses.push_back(cls);
    }
  }
}

bool Analysis::isLibrary(const FunctionDecl *function) const {
  return function && libraryEffects().inLibrary(function->loc());
}

void Analysis::resource(Res res, bool write, llvm::SMLoc at) {
  codeWalk().touch(LocKind::Resource, Ref(), nullptr, write, at, res);
}

std::optional<Ref> Analysis::classRefOrNone(const FnInfo &f) const {
  return f.thisOwner ? std::optional<Ref>(callWalk().classRef(f.thisOwner)) : std::nullopt;
}

Analysis::~Analysis() = default;
}
