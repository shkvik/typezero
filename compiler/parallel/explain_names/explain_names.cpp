#include "compiler/parallel/explain_names/explain_names.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
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

using llvm::dyn_cast;

using llvm::dyn_cast_or_null;

ExplainNames::ExplainNames(Analysis &analysis) : analysis_(analysis) {}

Analysis::Pos ExplainNames::posOf(llvm::SMLoc loc) const {
  Analysis::Pos pos;
  if (!loc.isValid()) {
    return pos;
  }
  pos.buffer = analysis_.sources_.FindBufferContainingLoc(loc);
  if (pos.buffer) {
    pos.offset = unsigned(loc.getPointer() -
                          analysis_.sources_.getMemoryBuffer(pos.buffer)->getBufferStart());
  }

  return pos;
}

unsigned ExplainNames::lineOf(llvm::SMLoc loc) const {
  if (!loc.isValid()) {
    return 0;
  }

  return analysis_.sources_.FindLineNumber(loc);
}

std::string ExplainNames::where(llvm::SMLoc loc) const {
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

std::string ExplainNames::typeText(const Type *t) const {
  if (!t) {
    return "object";
  }

  return t->nonOptional()->name().str();
}

std::string ExplainNames::calleeText(const Expr *e) const {
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

std::string ExplainNames::callText(const CallExpr *c) const {
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

std::string ExplainNames::refName(Ref r) const {
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
  case RefKind::Owned:
    return "any " + typeText(static_cast<const OwnedRef *>(r.ptr)->type);
  }

  return "?";
}

std::string ExplainNames::locName(const Loc &loc) const {
  switch (loc.kind) {
  case LocKind::Var: {
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
    case Res::Metadata:
      return "the metadata";
    case Res::Runtime:
      return "the runtime";
    }
  }

  return "?";
}

std::string ExplainNames::positions(
    const std::vector<std::pair<llvm::SMLoc, const FunctionDecl *>> &ats) const {
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
      text += " in " + analysis_.explainTitles().fnName(info);
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

}
