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

const FunctionDecl *Analysis::thisOwnerOf(const FunctionDecl *fn) const {
  for (; fn; fn = fn->parent) {
    if (fn->takesThis()) {
      return fn;
    }
    if (!fn->isArrow) {
      return nullptr;
    }
  }

  return nullptr;
}

void Analysis::run() {
  star_ = intern("*");
  length_ = intern("%length");
  inner_ = intern("%inner");
  state_ = intern("%state");
  listenersKey_ = intern("%listeners");
  codeWalk().discover(nullptr); // the top-level code
  callResolver().resolve();
  callResolver().fold();
  pieces().explore();
  pieces().findPieces();
  pieces().findHosts();
  pieces().buildFlow();
  pieces().findJoins();
  conflicts().decide();
}

ParallelPlan Analysis::plan() const {
  ParallelPlan plan;
  for (size_t r = 0; r < roots_.size(); ++r) {
    const Root &root = roots_[r];
    ParallelPlan::Task task;
    task.fn = fns_[root.fn].decl;
    task.parent = root.parent;
    task.several = root.multi;
    task.joinedAt = root.join;
    if (root.rec >= 0) {
      task.site = fns_[root.callerFn].calls[root.rec].site;
    }
    plan.tasks.push_back(task);
  }
  for (unsigned i = 0; i < pieces_.size(); ++i) {
    const PieceInst &p = pieces_[i];
    ParallelPlan::Piece piece;
    piece.fn = fns_[p.fn].decl;
    piece.start = fns_[p.fn].segs[p.seg].at;
    piece.task = unsigned(p.root);
    piece.alone = decisions_[i].alone;
    for (const Partner &partner : decisions_[i].partners) {
      piece.inOrderWith.push_back(partner.piece);
    }
    piece.orderedFrom = decisions_[i].from;
    piece.orderedTo = decisions_[i].to;
    piece.orderedAt = decisions_[i].ats;
    plan.pieces.push_back(std::move(piece));
  }

  return plan;
}

Analysis::Analysis(const SourceFile &program, const llvm::SourceMgr &sources)
    : program_(program), sources_(sources) {
  codeWalkPart_ = std::make_unique<CodeWalk>(*this);
  exprWalkPart_ = std::make_unique<ExprWalk>(*this);
  callWalkPart_ = std::make_unique<CallWalk>(*this);
  libraryEffectsPart_ = std::make_unique<LibraryEffects>(*this);
  builtinCallsPart_ = std::make_unique<BuiltinEffects>(*this);
  aliasAnalysisPart_ = std::make_unique<AliasAnalysis>(*this);
  escapeScanPart_ = std::make_unique<EscapeScan>(*this);
  callResolverPart_ = std::make_unique<CallResolver>(*this);
  piecesPart_ = std::make_unique<Pieces>(*this);
  conflictsPart_ = std::make_unique<Conflicts>(*this);
  explainerPart_ = std::make_unique<Explainer>(*this);
  explainNamesPart_ = std::make_unique<ExplainNames>(*this);
  explainTitlesPart_ = std::make_unique<ExplainTitles>(*this);
  for (const Module *module : program.modules) {
    if (!module->builtin) {
      continue;
    }
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = sources.getMemoryBuffer(id);
      libRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
    }
  }
  for (const Stmt *item : program.items) {
    if (auto *cls = dyn_cast<ClassDecl>(item);
        cls && !cls->builtin && !libraryEffects().inLibrary(cls->loc())) {
      programClasses_.push_back(cls);
    }
  }
}

bool Analysis::isLibrary(const FunctionDecl *fn) const {
  return fn && libraryEffects().inLibrary(fn->loc());
}

void Analysis::resource(Res res, bool write, llvm::SMLoc at) {
  codeWalk().touch(LocKind::Resource, Ref(), nullptr, write, at, res);
}

std::optional<Ref> Analysis::classRefOrNone(const FnInfo &f) const {
  return f.thisOwner ? std::optional<Ref>(callWalk().classRef(f.thisOwner)) : std::nullopt;
}

Analysis::~Analysis() = default;
} // namespace tz::parallel
