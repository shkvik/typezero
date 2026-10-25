#include "compiler/parallel/explainer/explainer.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
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

Explainer::Explainer(Analysis &analysis) : analysis_(analysis) {}

std::vector<std::string> Explainer::decisionLines(unsigned piece) const {
  const Analysis::Decision &d = analysis_.decisions_[piece];
  std::vector<std::string> lines;
  if (d.alone) {
    return {"runs alone"};
  }
  if (d.partners.empty()) {
    return {"parallel"};
  }
  std::vector<std::pair<std::string, std::string>> partners;
  for (const Analysis::Partner &partner : d.partners) {
    std::string who;
    if (partner.piece == piece) {
      who = "the same piece in other tasks";
    } else {
      // The partner's task, if its piece runs in more than one.
      const PieceInst &p = analysis_.pieces_[partner.piece];
      who = analysis_.explainTitles().pieceTitle(partner.piece,
                                                 analysis_.segPieces_.at({p.fn, p.seg}).size() > 1);
    }
    std::vector<std::string> names;
    for (const Loc &loc : partner.locs) {
      names.push_back(analysis_.explainNames().locName(loc));
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
  std::string from = analysis_.explainNames().where(d.from),
              to = analysis_.explainNames().where(d.to);
  if (from == to) {
    lines.push_back("in order: " + Analysis::lineText(from));
  } else if (Analysis::inOtherFile(from) || Analysis::inOtherFile(to)) {
    // In another file: "lib.tz:3-5" within it.
    llvm::StringRef a(from), b(to);
    if (a.rsplit(':').first == b.rsplit(':').first) {
      lines.push_back("in order: " + from + "-" + b.rsplit(':').second.str());
    } else {
      lines.push_back("in order: " + from + "-" + to);
    }
  } else {
    lines.push_back("in order: lines " + from + "-" + to);
  }

  return lines;
}

void Explainer::print(llvm::raw_ostream &out) {
  if (!analysis_.program_.usesAsync) {
    out << "no async code: the program runs as one piece\n";
    return;
  }
  // The functions with pieces, in the order of the source: the top-level
  // code first.
  std::vector<unsigned> order;
  std::vector<bool> shown(analysis_.fns_.size(), false);
  for (const PieceInst &p : analysis_.pieces_) {
    shown[p.fn] = true;
  }
  // An async function whose start runs inside its callers is shown too.
  for (const Activation &act : analysis_.acts_) {
    if (analysis_.fns_[act.fn].async || analysis_.fns_[act.fn].asyncGen) {
      shown[act.fn] = true;
    }
  }
  for (unsigned fi = 0; fi < analysis_.fns_.size(); ++fi) {
    if (shown[fi]) {
      order.push_back(fi);
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
    if (!analysis_.fns_[a].decl || !analysis_.fns_[b].decl) {
      return !analysis_.fns_[a].decl && analysis_.fns_[b].decl;
    }

    return analysis_.explainNames().posOf(analysis_.fns_[a].decl->loc()) <
           analysis_.explainNames().posOf(analysis_.fns_[b].decl->loc());
  });
  bool first = true;
  for (unsigned fi : order) {
    const FnInfo &f = analysis_.fns_[fi];
    if (!first) {
      out << "\n";
    }
    first = false;
    out << analysis_.explainTitles().fnTitle(f) << "\n";
    std::vector<unsigned> segs;
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      segs.push_back(s);
    }
    std::stable_sort(segs.begin(), segs.end(), [&](unsigned a, unsigned b) {
      if (a == 0 || b == 0) {
        return a == 0 && b != 0;
      }

      return analysis_.explainNames().posOf(f.segs[a].loc) <
             analysis_.explainNames().posOf(f.segs[b].loc);
    });
    for (unsigned s : segs) {
      std::vector<unsigned> mine;
      if (auto found = analysis_.segPieces_.find({fi, s}); found != analysis_.segPieces_.end()) {
        mine = found->second;
      }
      std::vector<std::pair<std::string, unsigned>> byContext;
      for (unsigned i : mine) {
        byContext.push_back({analysis_.explainTitles().contextOf(i), i});
      }
      std::sort(byContext.begin(), byContext.end());
      for (size_t k = 0; k < mine.size(); ++k) {
        mine[k] = byContext[k].second;
      }
      std::string title = analysis_.explainTitles().segTitle(f, s);
      if (mine.empty()) {
        if (!f.isInline(s)) {
          continue; // never runs
        }
        title +=
            f.asyncGen ? ": runs inside the loop that iterates it" : ": runs inside its caller";
      } else if (mine.size() == 1 && analysis_.pieces_[mine[0]].root != 0) {
        title += " (in " + analysis_.explainTitles().contextOf(mine[0]) + ")";
      }
      out << "  " << title << "\n";
      // What it does, with `this` as the code writes it.
      std::vector<Touch> list = analysis_.conflicts().touches(f.segs[s].effects);
      std::stable_sort(list.begin(), list.end(), [&](const Touch &a, const Touch &b) {
        Analysis::Pos pa = analysis_.explainNames().posOf(a.ats.front().first),
                      pb = analysis_.explainNames().posOf(b.ats.front().first);
        for (const auto &[at, via] : a.ats) {
          if (analysis_.explainNames().posOf(at) < pa) {
            pa = analysis_.explainNames().posOf(at);
          }
        }
        for (const auto &[at, via] : b.ats) {
          if (analysis_.explainNames().posOf(at) < pb) {
            pb = analysis_.explainNames().posOf(at);
          }
        }
        if (pa < pb || pb < pa) {
          return pa < pb;
        }

        return analysis_.explainNames().locName(a.loc) < analysis_.explainNames().locName(b.loc);
      });
      for (const Touch &t : list) {
        if (t.read) {
          out << "    reads  " << analysis_.explainNames().locName(t.loc) << " ("
              << analysis_.explainNames().positions(t.readAts) << ")\n";
        }
        if (t.write) {
          out << "    writes " << analysis_.explainNames().locName(t.loc) << " ("
              << analysis_.explainNames().positions(t.writeAts) << ")\n";
        }
      }
      if (mine.size() == 1) {
        for (const std::string &line : decisionLines(mine[0])) {
          out << "    " << line << "\n";
        }
        continue;
      }
      for (unsigned i : mine) {
        out << "    in " << analysis_.explainTitles().contextOf(i) << ":\n";
        for (const std::string &line : decisionLines(i)) {
          out << "      " << line << "\n";
        }
      }
    }
  }
}

} // namespace tz::parallel
