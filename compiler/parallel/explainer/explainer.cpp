#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/library_effects/library_effects.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

Explainer::Explainer(Analysis &analysis) : analysis_(analysis) {}

Analysis::Pos Explainer::posOf(llvm::SMLoc loc) const {
  Analysis::Pos pos;
  if (!loc.isValid()) {
    return pos;
  }
  pos.buffer = analysis_.sources_.FindBufferContainingLoc(loc);
  if (pos.buffer) {
    pos.offset =
        unsigned(loc.getPointer() - analysis_.sources_.getMemoryBuffer(pos.buffer)->getBufferStart());
  }

  return pos;
}

unsigned Explainer::lineOf(llvm::SMLoc loc) const {
  if (!loc.isValid()) {
    return 0;
  }

  return analysis_.sources_.FindLineNumber(loc);
}

// "12" in the entry file, "lib/math.tz:12" in another.
std::string Explainer::where(llvm::SMLoc loc) const {
  if (!loc.isValid()) {
    return "?";
  }
  unsigned buffer = analysis_.sources_.FindBufferContainingLoc(loc);
  std::string line = std::to_string(analysis_.sources_.FindLineNumber(loc, buffer));
  if (!buffer) {
    return line;
  }
  llvm::StringRef id = analysis_.sources_.getMemoryBuffer(buffer)->getBufferIdentifier();
  for (const Module *module : analysis_.program_.modules) {
    if (module->path == id) {
      return module->isEntry ? line : module->name + ":" + line;
    }
  }

  return line;
}

std::string Explainer::typeText(const Type *t) const {
  if (!t) {
    return "object";
  }

  return t->nonOptional()->name().str();
}

std::string Explainer::fnName(const FnInfo &f) const {
  const FunctionDecl *fn = f.decl;
  if (!fn) {
    return "the top-level code";
  }
  std::string name = fn->name.str();
  if (fn->cls) {
    std::string owner = fn->cls->literal ? "the object " + Analysis::atText(where(fn->cls->literal->loc()))
                                         : fn->cls->name.str();
    if (fn->isConstructor) {
      return "constructor of " + owner;
    }
    if (fn == fn->cls->init) {
      return "fields of " + owner;
    }
    return fn->cls->literal ? name + " of " + owner : owner + "." + name;
  }
  if (fn->isInstance && !fn->typeArgs.empty()) {
    name += "<";
    for (size_t i = 0; i < fn->typeArgs.size(); ++i) {
      name += (i ? ", " : "") + typeText(fn->typeArgs[i]);
    }
    name += ">";
  }
  if (fn->isArrow || fn->isExpression) {
    if (!name.empty()) {
      return name;
    }
    name = (fn->isArrow ? "the arrow " : "the function ") + Analysis::atText(where(fn->loc()));
    // Each copy of a generic function has a copy of it.
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

std::string Explainer::fnTitle(const FnInfo &f) const {
  const FunctionDecl *fn = f.decl;
  if (!fn) {
    return "top-level code";
  }
  std::string kind = f.asyncGen ? "async generator" : f.async ? "async" : "";
  std::string what = fn->cls            ? (fn->isConstructor ? "" : "method")
                     : fn->isArrow      ? "arrow"
                     : fn->isExpression ? "function expression"
                                        : "function";
  std::string title = kind;
  if (!what.empty()) {
    title += (title.empty() ? "" : " ") + what;
  }
  if (title.empty()) {
    title = "callback";
  }
  if (fn->isArrow || fn->isExpression) {
    if (!fn->name.empty()) {
      title += " " + fn->name.str();
    }
  } else {
    title += " " + fnName(f);
  }
  title += ", " + Analysis::lineText(where(fn->loc()));
  for (const FunctionDecl *up = fn->parent; up && (fn->isArrow || fn->isExpression);
       up = up->parent) {
    if (up->isInstance) {
      FnInfo info;
      info.decl = up;
      return title + ", in " + fnName(info);
    }
  }

  return title;
}

std::string Explainer::segTitle(const FnInfo &f, unsigned seg) const {
  const Segment &s = f.segs[seg];
  if (s.kind == SegKind::Start) {
    return "start";
  }
  // Two awaits on one line are told apart by their column.
  std::string at = where(s.loc);
  unsigned line = lineOf(s.loc), same = 0;
  for (const Segment &o : f.segs) {
    if (o.kind != SegKind::Start && lineOf(o.loc) == line &&
        posOf(o.loc).buffer == posOf(s.loc).buffer) {
      ++same;
    }
  }
  if (same > 1) {
    at += ":" + std::to_string(analysis_.sources_.getLineAndColumn(s.loc).second);
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

std::string Explainer::calleeText(const Expr *e) const {
  e = strip(e);
  if (auto *n = dyn_cast_or_null<NameExpr>(e)) {
    return n->name.str();
  }
  if (auto *m = dyn_cast_or_null<MemberExpr>(e)) {
    return calleeText(m->object) + "." + m->name.str();
  }
  if (auto *c = dyn_cast_or_null<CallExpr>(e)) {
    return calleeText(c->callee) + "()";
  }
  if (auto *ix = dyn_cast_or_null<IndexExpr>(e)) {
    return calleeText(ix->object) + "[...]";
  }

  return "(...)";
}

// The text of a call as written, `a.run(2)`; a shorter one for a long call.
std::string Explainer::callText(const CallExpr *c) const {
  std::string fallback = calleeText(c->callee) + "()";
  const Expr *first = c->callee;
  while (true) {
    if (auto *m = dyn_cast<MemberExpr>(first)) {
      first = m->object;
    } else if (auto *call = dyn_cast<CallExpr>(first)) {
      first = call->callee;
    } else if (auto *ix = dyn_cast<IndexExpr>(first)) {
      first = ix->object;
    } else if (auto *conv = dyn_cast<CastExpr>(first); conv && conv->isImplicit()) {
      first = conv->operand;
    } else {
      break;
    }
  }
  const char *start = first->loc().getPointer(), *p = c->loc().getPointer();
  if (!start || !p || start > p) {
    return fallback;
  }
  while (*p && *p != '(' && *p != '\n' && *p != ';') {
    ++p;
  }
  if (*p != '(') {
    return fallback;
  }
  int depth = 0;
  for (; *p && *p != '\n'; ++p) {
    if (*p == '(') {
      ++depth;
    } else if (*p == ')' && --depth == 0) {
      break;
    }
  }
  if (*p != ')') {
    return fallback;
  }
  std::string text(start, size_t(p - start + 1));

  return text.size() <= 40 ? text : fallback;
}

// How the explanation names a task; two tasks started on one line by calls
// of the same text are told apart by the column.
std::string Explainer::rootTitle(int r) const {
  if (analysis_.rootTitles_.empty()) {
    // Only tasks of one function need names apart: the explanation shows
    // tasks under the pieces of their function.
    auto key = [&](size_t i, const std::string &title) {
      return title + "|" + std::to_string(analysis_.roots_[i].fn);
    };
    // First by the column, where that tells them apart.
    std::map<std::string, std::set<std::string>> columns;
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      columns[key(i, rootTitleAt(int(i), false))].insert(rootTitleAt(int(i), true));
    }
    std::vector<std::string> titles;
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      std::string title = rootTitleAt(int(i), false);
      titles.push_back(columns[key(i, title)].size() > 1 ? rootTitleAt(int(i), true) : title);
    }
    std::map<std::string, unsigned> count;
    // Then by the task that starts them (parents come first), and last by
    // their order.
    std::map<std::string, unsigned> same, seen;
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      ++same[key(i, titles[i])];
    }
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      std::string title = titles[i];
      int parent = analysis_.roots_[i].parent;
      if (same[key(i, titles[i])] > 1 && parent >= 0 && size_t(parent) < i) {
        title += " from " + analysis_.rootTitles_[size_t(parent)];
      }
      analysis_.rootTitles_.push_back(title);
    }
    count.clear();
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      ++count[key(i, analysis_.rootTitles_[i])];
    }
    for (size_t i = 0; i < analysis_.roots_.size(); ++i) {
      if (count[key(i, analysis_.rootTitles_[i])] > 1) {
        analysis_.rootTitles_[i] += " #" + std::to_string(++seen[key(i, analysis_.rootTitles_[i])]);
      }
    }
  }

  return analysis_.rootTitles_[size_t(r)];
}

std::string Explainer::rootTitleAt(int r, bool column) const {
  const Root &root = analysis_.roots_[r];
  if (r == 0) {
    return "the top-level code";
  }
  const CallRec &rec = analysis_.fns_[root.callerFn].calls[root.rec];
  std::string place = where(rec.loc);
  if (column) {
    place += ":" + std::to_string(analysis_.sources_.getLineAndColumn(rec.loc).second);
  }
  std::string at = " " + Analysis::atText(place);
  if (root.mode == Mode::Callback) {
    return (root.multi ? "the callbacks" : "the callback") + at;
  }
  if (rec.kind == CallKind::Inline) {
    // A callback that map (or the like) runs for each element.
    const FunctionDecl *fn = analysis_.fns_[root.fn].decl;
    std::string text = (root.multi ? "the tasks of " : "the task of ") + fnName(analysis_.fns_[root.fn]);
    if (fn && !fn->name.empty()) {
      text += at;
    }
    return text;
  }
  std::string text = rec.call ? callText(rec.call) : fnName(analysis_.fns_[root.fn]) + "()";

  return (root.multi ? "the tasks " : "the task ") + text + at;
}

std::string Explainer::contextOf(unsigned piece) const {
  const PieceInst &p = analysis_.pieces_[piece];
  std::string text = rootTitle(p.root);
  // Pieces of one function in one task with different objects as `this`.
  if (analysis_.sameTask_.at(std::make_tuple(p.fn, p.seg, p.root)) > 1 && p.self) {
    text += ", this = " + refName(*p.self);
  }

  return text;
}

std::string Explainer::pieceTitle(unsigned piece, bool withContext) const {
  const PieceInst &p = analysis_.pieces_[piece];
  const FnInfo &f = analysis_.fns_[p.fn];
  std::string text = p.seg != 0          ? fnName(f) + " " + segTitle(f, p.seg)
                     : f.segs.size() > 1 ? "the start of " + fnName(f)
                                         : fnName(f);
  if (withContext) {
    text += " in " + contextOf(piece);
  }

  return text;
}

std::string Explainer::refName(Ref r) const {
  switch (r.kind) {
  case RefKind::This:
    return "this";
  case RefKind::Local:
    return "its own object";
  case RefKind::Site: {
    auto *e = static_cast<const Expr *>(r.ptr);
    auto found = analysis_.siteName_.find(e);
    if (found != analysis_.siteName_.end()) {
      return found->second.str();
    }
    const Type *t = e->type ? e->type->nonOptional() : nullptr;
    std::string what = !t                ? "the object"
                       : t->isArray()    ? "the array"
                       : t->isMap()      ? "the map"
                       : t->isSet()      ? "the set"
                       : t->isDict()     ? "the record"
                       : t->isIterator() ? "the iterator"
                                         : "the " + typeText(t);
    return what + " made " + Analysis::atText(where(e->loc()));
  }
  case RefKind::Type:
    return "any " + typeText(static_cast<const Type *>(r.ptr));
  }

  return "?";
}

std::string Explainer::locName(const Loc &loc) const {
  switch (loc.kind) {
  case LocKind::Var: {
    // The compiler's own variables: what a decorator put in a method's
    // place ("%Calc.add.0"), and the others.
    llvm::StringRef name = static_cast<const VarSymbol *>(loc.key)->name;
    if (!name.consume_front("%")) {
      return name.str();
    }
    if (name.count('.') == 2) {
      return "the method " + name.rsplit('.').first.str() + " as decorated";
    }
    return "a variable the compiler made";
  }
  case LocKind::Field: {
    llvm::StringRef field = static_cast<const char *>(loc.key);
    std::string object = refName(loc.ref);
    if (field == "*") {
      return "the fields of " + object;
    }
    if (field == "%time") {
      return "the time of " + object;
    }
    if (field == "%state") {
      return "the state of " + object;
    }
    if (field == "%listeners") {
      return "the listeners of " + object;
    }
    if (field == "%length") {
      return "the length of " + object;
    }
    if (loc.ref.kind == RefKind::Type ||
        (loc.ref.kind == RefKind::Site &&
         !analysis_.siteName_.count(static_cast<const Expr *>(loc.ref.ptr)))) {
      return field.str() + " of " + object;
    }
    return object + "." + field.str();
  }
  case LocKind::Elements: {
    const Type *t = analysis_.aliasAnalysis().typeOfRef(loc.ref);
    std::string what = t && t->isDict()                                 ? "entries"
                       : t && (t->isIterator() || t->isAsyncIterator()) ? "position"
                       : t && t->isBytes()                              ? "bytes"
                       : t && t->isJson()                               ? "contents"
                                                                        : "elements";
    return what + " of " + refName(loc.ref);
  }
  case LocKind::OwnCell:
    return "elements of " + refName(loc.ref) + ", each task its own";
  case LocKind::Contents:
    if (loc.key == analysis_.inner_) {
      return "the objects inside its own " + typeText(analysis_.aliasAnalysis().typeOfRef(loc.ref));
    }
    return "everything in " + refName(loc.ref);
  case LocKind::Resource:
    switch (loc.res) {
    case Res::Stdout:
      return "stdout";
    case Res::Stderr:
      return "stderr";
    case Res::Stdin:
      return "stdin";
    case Res::Files:
      return "the files";
    case Res::Process:
      return "the exit code and environment";
    case Res::Signals:
      return "the abort signals";
    case Res::Metadata:
      return "the metadata";
    case Res::Runtime:
      return "the runtime";
    }
  }

  return "?";
}

std::string
Explainer::positions(const std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> &ats) const {
  std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> sorted = ats;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [&](const auto &a, const auto &b) { return posOf(a.first) < posOf(b.first); });
  std::vector<std::string> parts;
  bool entryFile = false;
  for (const auto &[at, via] : sorted) {
    std::string text = where(at);
    entryFile |= !Analysis::inOtherFile(text);
    if (via) {
      FnInfo info;
      info.decl = via;
      text += " in " + fnName(info);
    }
    if (!llvm::is_contained(parts, text)) {
      parts.push_back(text);
    }
  }
  constexpr size_t kShown = 6;
  std::string out;
  if (entryFile) {
    out = parts.size() == 1 ? "line " : "lines ";
  }
  for (size_t i = 0; i < parts.size() && i < kShown; ++i) {
    out += (i ? ", " : "") + parts[i];
  }
  if (parts.size() > kShown) {
    out += " and " + std::to_string(parts.size() - kShown) + " more";
  }

  return out;
}

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
      who = pieceTitle(partner.piece, analysis_.segPieces_.at({p.fn, p.seg}).size() > 1);
    }
    std::vector<std::string> names;
    for (const Loc &loc : partner.locs) {
      names.push_back(locName(loc));
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
  std::string from = where(d.from), to = where(d.to);
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

    return posOf(analysis_.fns_[a].decl->loc()) < posOf(analysis_.fns_[b].decl->loc());
  });
  bool first = true;
  for (unsigned fi : order) {
    const FnInfo &f = analysis_.fns_[fi];
    if (!first) {
      out << "\n";
    }
    first = false;
    out << fnTitle(f) << "\n";
    std::vector<unsigned> segs;
    for (unsigned s = 0; s < f.segs.size(); ++s) {
      segs.push_back(s);
    }
    std::stable_sort(segs.begin(), segs.end(), [&](unsigned a, unsigned b) {
      if (a == 0 || b == 0) {
        return a == 0 && b != 0;
      }

      return posOf(f.segs[a].loc) < posOf(f.segs[b].loc);
    });
    for (unsigned s : segs) {
      std::vector<unsigned> mine;
      if (auto found = analysis_.segPieces_.find({fi, s}); found != analysis_.segPieces_.end()) {
        mine = found->second;
      }
      std::vector<std::pair<std::string, unsigned>> byContext;
      for (unsigned i : mine) {
        byContext.push_back({contextOf(i), i});
      }
      std::sort(byContext.begin(), byContext.end());
      for (size_t k = 0; k < mine.size(); ++k) {
        mine[k] = byContext[k].second;
      }
      std::string title = segTitle(f, s);
      if (mine.empty()) {
        if (!f.isInline(s)) {
          continue; // never runs
        }
        title +=
            f.asyncGen ? ": runs inside the loop that iterates it" : ": runs inside its caller";
      } else if (mine.size() == 1 && analysis_.pieces_[mine[0]].root != 0) {
        title += " (in " + contextOf(mine[0]) + ")";
      }
      out << "  " << title << "\n";
      // What it does, with `this` as the code writes it.
      std::vector<Touch> list = analysis_.conflicts().touches(f.segs[s].effects);
      std::stable_sort(list.begin(), list.end(), [&](const Touch &a, const Touch &b) {
        Analysis::Pos pa = posOf(a.ats.front().first), pb = posOf(b.ats.front().first);
        for (const auto &[at, via] : a.ats) {
          if (posOf(at) < pa) {
            pa = posOf(at);
          }
        }
        for (const auto &[at, via] : b.ats) {
          if (posOf(at) < pb) {
            pb = posOf(at);
          }
        }
        if (pa < pb || pb < pa) {
          return pa < pb;
        }

        return locName(a.loc) < locName(b.loc);
      });
      for (const Touch &t : list) {
        if (t.read) {
          out << "    reads  " << locName(t.loc) << " (" << positions(t.readAts) << ")\n";
        }
        if (t.write) {
          out << "    writes " << locName(t.loc) << " (" << positions(t.writeAts) << ")\n";
        }
      }
      if (mine.size() == 1) {
        for (const std::string &line : decisionLines(mine[0])) {
          out << "    " << line << "\n";
        }
        continue;
      }
      for (unsigned i : mine) {
        out << "    in " << contextOf(i) << ":\n";
        for (const std::string &line : decisionLines(i)) {
          out << "      " << line << "\n";
        }
      }
    }
  }
}

} // namespace tz::parallel
