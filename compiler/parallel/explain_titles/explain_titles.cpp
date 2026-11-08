#include "compiler/parallel/explain_titles/explain_titles.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/analysis/analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explain_names/explain_names.h"
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

ExplainTitles::ExplainTitles(Analysis &analysis)
    : analysis(analysis) {}

std::string ExplainTitles::fnName(const FnInfo &f) const {
  const FunctionDecl *fn = f.decl;
  if (fn == this->analysis.module) {
    return "the top-level code";
  }
  std::string name = fn->name.str();
  if (fn->cls()) {
    std::string owner =
        fn->cls()->literal()
            ? "the object " +
                  Analysis::atText(this->analysis.explainNames.where(fn->cls()->literal()->loc()))
            : fn->cls()->name.str();
    if (fn->isConstructor()) {
      return "constructor of " + owner;
    }
    if (fn == fn->cls()->init) {
      return "fields of " + owner;
    }
    return fn->cls()->literal() ? name + " of " + owner : owner + "." + name;
  }
  if (fn->isInstance && !fn->typeArgs.empty()) {
    name += "<";
    for (size_t i = 0; i < fn->typeArgs.size(); ++i) {
      name += (i ? ", " : "") + this->analysis.explainNames.typeText(fn->typeArgs[i]);
    }
    name += ">";
  }
  if (fn->isArrow() || fn->isExpression()) {
    if (!name.empty()) {
      return name;
    }
    name = (fn->isArrow() ? "the arrow " : "the function ") +
           Analysis::atText(this->analysis.explainNames.where(fn->loc()));
    for (const FunctionDecl *up = fn->parent; up; up = up->parent) {
      if (up->isInstance) {
        FnInfo info;
        info.decl = up;
        return name + " in " + fnName(info);
      }
    }
  }

  return name;
}

std::string ExplainTitles::fnTitle(const FnInfo &f) const {
  const FunctionDecl *fn = f.decl;
  if (fn == this->analysis.module) {
    return "top-level code";
  }
  std::string kind = f.asyncGen ? "async generator" : f.async ? "async" : "";
  std::string what = fn->cls()            ? (fn->isConstructor() ? "" : "method")
                     : fn->isArrow()      ? "arrow"
                     : fn->isExpression() ? "function expression"
                                          : "function";
  std::string title = kind;
  if (!what.empty()) {
    title += (title.empty() ? "" : " ") + what;
  }
  if (title.empty()) {
    title = "callback";
  }
  if (fn->isArrow() || fn->isExpression()) {
    if (!fn->name.empty()) {
      title += " " + fn->name.str();
    }
  } else {
    title += " " + fnName(f);
  }
  title += ", " + Analysis::lineText(this->analysis.explainNames.where(fn->loc()));
  for (const FunctionDecl *up = fn->parent; up && (fn->isArrow() || fn->isExpression());
       up = up->parent) {
    if (up->isInstance) {
      FnInfo info;
      info.decl = up;
      return title + ", in " + fnName(info);
    }
  }

  return title;
}

std::string ExplainTitles::segTitle(const FnInfo &f, unsigned seg) const {
  const Segment &s = f.segs[seg];
  if (s.kind == SegKind::Start) {
    return "start";
  }
  std::string at = this->analysis.explainNames.where(s.loc);
  unsigned line = this->analysis.explainNames.lineOf(s.loc), same = 0;
  for (const Segment &o : f.segs) {
    if (o.kind != SegKind::Start && this->analysis.explainNames.lineOf(o.loc) == line &&
        this->analysis.explainNames.posOf(o.loc).buffer ==
            this->analysis.explainNames.posOf(s.loc).buffer) {
      ++same;
    }
  }
  if (same > 1) {
    at += ":" + std::to_string(this->analysis.sources.getLineAndColumn(s.loc).second);
  }
  switch (s.kind) {
  case SegKind::Await:
    return (s.delegate ? "after yield* " : "after await ") + Analysis::atText(at);
  case SegKind::ForAwait:
    return "after for await " + Analysis::atText(at);
  case SegKind::Yield:
    return "after yield " + Analysis::atText(at);
  case SegKind::Start:
    break;
  }

  return "start";
}

std::string ExplainTitles::rootTitle(int r) const {
  if (this->analysis.rootTitles.empty()) {
    auto key = [&](size_t i, const std::string &title) {
      return title + "|" + std::to_string(this->analysis.roots[i].fn);
    };
    std::map<std::string, std::set<std::string>> columns;
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      columns[key(i, rootTitleAt(int(i), false))].insert(rootTitleAt(int(i), true));
    }
    std::vector<std::string> titles;
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      std::string title = rootTitleAt(int(i), false);
      titles.push_back(columns[key(i, title)].size() > 1 ? rootTitleAt(int(i), true) : title);
    }
    std::map<std::string, unsigned> count;
    std::map<std::string, unsigned> same, seen;
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      ++same[key(i, titles[i])];
    }
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      std::string title = titles[i];
      int parent = this->analysis.roots[i].parent;
      if (same[key(i, titles[i])] > 1 && parent >= 0 && size_t(parent) < i) {
        title += " from " + this->analysis.rootTitles[size_t(parent)];
      }
      this->analysis.rootTitles.push_back(title);
    }
    count.clear();
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      ++count[key(i, this->analysis.rootTitles[i])];
    }
    for (size_t i = 0; i < this->analysis.roots.size(); ++i) {
      if (count[key(i, this->analysis.rootTitles[i])] > 1) {
        this->analysis.rootTitles[i] +=
            " #" + std::to_string(++seen[key(i, this->analysis.rootTitles[i])]);
      }
    }
  }

  return this->analysis.rootTitles[size_t(r)];
}

std::string ExplainTitles::rootTitleAt(int r, bool column) const {
  const Root &root = this->analysis.roots[r];
  if (r == 0) {
    return "the top-level code";
  }
  const CallRec &rec = this->analysis.byFunction[root.callerFn]->calls[root.rec];
  std::string place = this->analysis.explainNames.where(rec.loc);
  if (column) {
    place += ":" + std::to_string(this->analysis.sources.getLineAndColumn(rec.loc).second);
  }
  std::string at = " " + Analysis::atText(place);
  if (root.mode == Mode::Callback) {
    return (root.multi ? "the callbacks" : "the callback") + at;
  }
  if (rec.kind == CallKind::Inline) {
    const FnInfo &target = *this->analysis.byFunction[root.fn];
    const FunctionDecl *fn = target.decl;
    std::string text = (root.multi ? "the tasks of " : "the task of ") + fnName(target);
    if (fn && !fn->name.empty()) {
      text += at;
    }
    return text;
  }
  std::string text = rec.call ? this->analysis.explainNames.callText(rec.call)
                              : fnName(*this->analysis.byFunction[root.fn]) + "()";

  return (root.multi ? "the tasks " : "the task ") + text + at;
}

std::string ExplainTitles::contextOf(unsigned piece) const {
  const PieceInst &p = this->analysis.pieceInstances[piece];
  std::string text = rootTitle(p.root);
  if (this->analysis.sameTask.at(std::make_tuple(p.fn, p.seg, p.root)) > 1 && p.self) {
    text += ", this = " + this->analysis.explainNames.refName(*p.self);
  }

  return text;
}

std::string ExplainTitles::pieceTitle(unsigned piece, bool withContext) const {
  const PieceInst &p = this->analysis.pieceInstances[piece];
  const FnInfo &f = *this->analysis.byFunction[p.fn];
  std::string text = p.seg != 0          ? fnName(f) + " " + segTitle(f, p.seg)
                     : f.segs.size() > 1 ? "the start of " + fnName(f)
                                         : fnName(f);
  if (withContext) {
    text += " in " + contextOf(piece);
  }

  return text;
}

}
