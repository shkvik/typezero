#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explain_names/explain_names.h"
#include "compiler/parallel/explain_titles/explain_titles.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
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

Explainer::Explainer(Analysis &analysis)
    : analysis(analysis) {}

std::vector<std::string> Explainer::decisionLines(unsigned piece) const {
  const Decision &d = this->analysis.decisions[piece];
  std::vector<std::string> lines;
  if (d.alone) {
    return {"runs alone"};
  }
  if (d.partners.empty()) {
    return {"parallel"};
  }
  std::vector<std::pair<std::string, std::string>> partners;
  for (const Partner &partner : d.partners) {
    std::string who;
    if (partner.piece == piece) {
      who = "the same piece in other tasks";
    } else {
      const PieceInst &p = this->analysis.pieceInstances[partner.piece];
      who = this->analysis.explainTitles.pieceTitle(
          partner.piece, this->analysis.segPieces.at({p.fn, p.seg}).size() > 1);
    }
    std::vector<std::string> names;
    for (const Loc &loc : partner.locs) {
      names.push_back(this->analysis.explainNames.locName(loc));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string what;
    for (size_t i = 0; i < names.size(); ++i) {
      what += (i ? ", " : "") + names[i];
    }
    partners.push_back({who, what});
  }
  std::sort(partners.begin(), partners.end());
  partners.erase(std::unique(partners.begin(), partners.end()), partners.end());
  for (const auto &[who, what] : partners) {
    lines.push_back("in order with " + who + ": " + what);
  }
  std::string from = this->analysis.explainNames.where(d.from),
              to = this->analysis.explainNames.where(d.to);
  if (from == to) {
    lines.push_back("in order: " + Analysis::lineText(from));
  } else if (Analysis::inOtherFile(from) || Analysis::inOtherFile(to)) {
    llvm::StringRef a(from), b(to);
    if (a.rsplit(':').first == b.rsplit(':').first) {
      lines.push_back("in order: " + from + "-" + b.rsplit(':').second.str());
    } else {
      lines.push_back("in order: " + from + "-" + to);
    }
  } else {
    lines.push_back("in order: lines " + from + "-" + to);
  }
  if (!d.inside.empty()) {
    lines.push_back("waits for its turn inside: " +
                    this->analysis.explainNames.positions(d.inside));
  }

  return lines;
}

void Explainer::print(llvm::raw_ostream &out) {
  if (!this->analysis.program.inventory.usesAsync) {
    out << "no async code: the program runs as one piece\n";
    return;
  }
  std::vector<const FnInfo *> order;
  std::vector<bool> shown(this->analysis.byFunction.size(), false);
  for (const PieceInst &p : this->analysis.pieceInstances) {
    shown[p.fn] = true;
  }
  for (const Activation &act : this->analysis.acts) {
    const FnInfo &f = *this->analysis.byFunction[act.fn];
    if (f.async || f.asyncGen) {
      shown[act.fn] = true;
    }
  }
  for (const FnInfo &f : this->analysis.records) {
    if (shown[f.decl->id]) {
      order.push_back(&f);
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](const FnInfo *a, const FnInfo *b) {
    bool aModule = a->decl == this->analysis.module;
    bool bModule = b->decl == this->analysis.module;
    if (aModule || bModule) {
      return aModule && !bModule;
    }

    return this->analysis.explainNames.posOf(a->decl->loc()) <
           this->analysis.explainNames.posOf(b->decl->loc());
  });
  bool first = true;
  for (const FnInfo *function : order) {
    const FnInfo &f = *function;
    if (!first) {
      out << "\n";
    }
    first = false;
    out << this->analysis.explainTitles.fnTitle(f) << "\n";
    std::vector<unsigned> segs;
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      segs.push_back(s);
    }
    std::stable_sort(segs.begin(), segs.end(), [&](unsigned a, unsigned b) {
      if (a == 0 || b == 0) {
        return a == 0 && b != 0;
      }

      return this->analysis.explainNames.posOf(f.segs[a].loc) <
             this->analysis.explainNames.posOf(f.segs[b].loc);
    });
    for (unsigned s : segs) {
      std::vector<unsigned> mine;
      if (auto found = this->analysis.segPieces.find({f.decl->id, s});
          found != this->analysis.segPieces.end()) {
        mine = found->second;
      }
      std::vector<std::pair<std::string, unsigned>> byContext;
      for (unsigned i : mine) {
        byContext.push_back({this->analysis.explainTitles.contextOf(i), i});
      }
      std::sort(byContext.begin(), byContext.end());
      for (size_t k = 0; k < mine.size(); ++k) {
        mine[k] = byContext[k].second;
      }
      std::string title = this->analysis.explainTitles.segTitle(f, s);
      if (mine.empty()) {
        if (!f.isInline(s)) {
          continue;
        }
        title +=
            f.asyncGen ? ": runs inside the loop that iterates it" : ": runs inside its caller";
      } else if (mine.size() == 1 && this->analysis.pieceInstances[mine[0]].root != 0) {
        title += " (in " + this->analysis.explainTitles.contextOf(mine[0]) + ")";
      }
      out << "  " << title << "\n";
      std::vector<Touch> list = this->analysis.conflicts.touches(f.segs[s].effects);
      std::stable_sort(list.begin(), list.end(), [&](const Touch &a, const Touch &b) {
        Pos pa = this->analysis.explainNames.posOf(a.ats.front().first),
            pb = this->analysis.explainNames.posOf(b.ats.front().first);
        for (const auto &[at, via] : a.ats) {
          if (this->analysis.explainNames.posOf(at) < pa) {
            pa = this->analysis.explainNames.posOf(at);
          }
        }
        for (const auto &[at, via] : b.ats) {
          if (this->analysis.explainNames.posOf(at) < pb) {
            pb = this->analysis.explainNames.posOf(at);
          }
        }
        if (pa < pb || pb < pa) {
          return pa < pb;
        }

        return this->analysis.explainNames.locName(a.loc) <
               this->analysis.explainNames.locName(b.loc);
      });
      for (const Touch &t : list) {
        if (t.read) {
          out << "    reads  " << this->analysis.explainNames.locName(t.loc) << " ("
              << this->analysis.explainNames.positions(t.readAts) << ")\n";
        }
        if (t.write) {
          out << "    writes " << this->analysis.explainNames.locName(t.loc) << " ("
              << this->analysis.explainNames.positions(t.writeAts) << ")\n";
        }
      }
      if (mine.size() == 1) {
        for (const std::string &line : decisionLines(mine[0])) {
          out << "    " << line << "\n";
        }
        continue;
      }
      for (unsigned i : mine) {
        out << "    in " << this->analysis.explainTitles.contextOf(i) << ":\n";
        for (const std::string &line : decisionLines(i)) {
          out << "      " << line << "\n";
        }
      }
    }
  }
}

}
