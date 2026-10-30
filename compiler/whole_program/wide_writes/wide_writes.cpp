#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "compiler/check/generics/generic_widening/generic_widening.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr int kMaxDepth = 6;

void collectNarrowings(const Type *from, const Type *to, int depth, std::vector<Narrowing> &out,
                       std::set<std::pair<const Type *, const Type *>> &seen) {
  from = from->nonOptional();
  to = to->nonOptional();
  if (from == to || depth > kMaxDepth || !seen.insert({from, to}).second) {
    return;
  }
  if (from->isArray() && to->isArray()) {
    if (from->inner() != to->inner()) {
      out.push_back({depth, true, "", from->inner(), from});
    }
    collectNarrowings(from->inner(), to->inner(), depth + 1, out, seen);
    return;
  }
  if (from->isUnion()) {
    if (llvm::is_contained(from->members(), to)) {
      return;
    }
    for (const Type *m : from->members()) {
      collectNarrowings(m, to, depth, out, seen);
    }
    return;
  }
  if (!from->isObject() || !to->isObject() || from->isAnyObject() || to->isAnyObject()) {
    return;
  }
  for (const Field &wide : to->fields()) {
    const Field *own = from->field(wide.name);
    if (!own) {
      continue;
    }
    if (own->readonly && !wide.readonly) {
      out.push_back({depth, false, wide.name, nullptr, from});
    } else if (own->type != wide.type) {
      out.push_back({depth, false, wide.name, own->type, from});
    }
    if (own->type != wide.type) {
      collectNarrowings(own->type, wide.type, depth + 1, out, seen);
    }
  }
}

}

WideWrites::WideWrites(Checker &checker) : checker_(checker) {}

bool WideWrites::widensWrites(const Type *from, const Type *to) {
  const Type *a = from->nonOptional(), *b = to->nonOptional();
  if (a == b || GenericWidening::widens(a, b)) {
    return false;
  }
  bool arrays = a->isArray() && b->isArray();
  bool objects = a->isObject() && b->isObject() && !a->isAnyObject() && !b->isAnyObject();
  if (!arrays && !objects && !(a->isUnion() && b->isObject())) {
    return false;
  }
  auto key = std::make_pair(a, b);
  if (auto found = widens_.find(key); found != widens_.end()) {
    return found->second;
  }
  std::vector<Narrowing> out;
  std::set<std::pair<const Type *, const Type *>> seen;
  collectNarrowings(a, b, 0, out, seen);

  return widens_[key] = !out.empty();
}

void WideWrites::checkWidenings(SourceFile &file) {
  if (checker_.widenings_.empty() && !checker_.narrowedFields_) {
    return;
  }
  auto narrowings = [](const Type *from, const Type *to) {
    std::vector<Narrowing> out;
    std::set<std::pair<const Type *, const Type *>> seen;
    collectNarrowings(from, to, 0, out, seen);

    return out;
  };
  auto convertible = [](const Type *from, const Type *to) {
    return TypeRelations::isImplicitlyConvertible(from, to);
  };
  Flows flows(checker_.widenings_, narrowings, convertible);
  flows.walkProgram(file);

  std::vector<std::vector<std::pair<size_t, size_t>>> writesAt(flows.edges.size());
  for (size_t w = 0; w < flows.writes.size(); ++w) {
    for (size_t r = 0; r < flows.writes[w].target.size(); ++r) {
      writesAt[size_t(flows.writes[w].target[r].node)].push_back({w, r});
    }
  }

  auto breaks = [](const Write &w, const Narrowing &n, int depth) {
    if ((depth >= 0 && n.depth != depth) || w.element != n.element) {
      return false;
    }
    if (!w.element && w.name != "*" && w.name != n.name) {
      return false;
    }

    return !n.narrow || !w.value || !TypeRelations::isImplicitlyConvertible(w.value, n.narrow);
  };
  auto mayBe = [](const Write &w, const Narrowing &n) {
    const Type *through = w.through ? w.through->nonOptional() : nullptr;
    if (!through || !n.owner || mentionsParam(through) || through->isUnknown() ||
        through->isAnyObject()) {
      return true;
    }
    if (n.element) {
      return !through->isArray() || !n.narrow || mentionsParam(through->inner()) ||
             TypeRelations::isImplicitlyConvertible(n.narrow, through->inner());
    }

    return TypeRelations::isImplicitlyConvertible(n.owner, through);
  };
  std::function<bool(const Type *, const Type *, int)> mayHold =
      [&](const Type *type, const Type *value, int depth) -> bool {
    if (!type) {
      return true;
    }
    type = type->nonOptional();
    if (type->isUnknown() || type->isAnyObject() || type->isError() || mentionsParam(type)) {
      return true;
    }
    if (depth == 0) {
      return TypeRelations::isImplicitlyConvertible(value, type);
    }
    if (!holdsReferences(type)) {
      return false;
    }
    if (type->isArray()) {
      return mayHold(type->inner(), value, depth - 1);
    }
    if (type->isDict()) {
      return mayHold(type->keyType(), value, depth - 1) ||
             mayHold(type->valueType(), value, depth - 1);
    }
    if (type->isUnion()) {
      return llvm::any_of(type->members(), [&](const Type *m) { return mayHold(m, value, depth); });
    }
    if (type->isTuple()) {
      return llvm::any_of(type->fields(),
                          [&](const Field &f) { return mayHold(f.type, value, depth - 1); });
    }

    return true;
  };
  std::function<bool(const Type *, const Type *, int)> mayBePart =
      [&](const Type *type, const Type *whole, int depth) -> bool {
    if (!type || !whole) {
      return true;
    }
    type = type->nonOptional();
    whole = whole->nonOptional();
    if (depth == 0) {
      return mayHold(type, whole, 0) || mayHold(whole, type, 0);
    }
    if (whole->isUnknown() || whole->isAnyObject() || whole->isError() || mentionsParam(whole)) {
      return true;
    }
    if (!holdsReferences(whole)) {
      return false;
    }
    if (whole->isArray()) {
      return mayBePart(type, whole->inner(), depth - 1);
    }
    if (whole->isDict()) {
      return mayBePart(type, whole->keyType(), depth - 1) ||
             mayBePart(type, whole->valueType(), depth - 1);
    }
    if (whole->isUnion()) {
      return llvm::any_of(whole->members(),
                          [&](const Type *m) { return mayBePart(type, m, depth); });
    }
    const ClassDecl *cls = whole->classDecl();
    if (whole->isTuple() || (cls && cls->children.empty())) {
      return llvm::any_of(whole->fields(),
                          [&](const Field &f) { return mayBePart(type, f.type, depth - 1); });
    }

    return true;
  };
  std::set<const char *> reported;
  for (const Conversion &conv : flows.conversions) {
    if (conv.narrowings.empty() || !reported.insert(conv.loc.getPointer()).second) {
      continue;
    }
    std::set<std::tuple<int, int, int>> seen;
    std::vector<std::tuple<int, int, int>> todo{{conv.node, 0, 0}};
    const Write *found = nullptr;
    const Narrowing *broken = nullptr;
    bool unknown = false;
    while (!todo.empty() && !found) {
      auto [node, off, md] = todo.back();
      todo.pop_back();
      if (off > kMaxDepth || off < -16 || md > 16 || !seen.insert({node, off, md}).second) {
        continue;
      }
      if (size_t(node) < flows.types.size()) {
        if (const Type *type = flows.types[size_t(node)];
            type && (off <= 0 ? !mayHold(type, conv.from, -off) && !mayHold(type, conv.to, -off)
                              : !mayBePart(type, conv.from, off))) {
          continue;
        }
      }
      if (node == flows.unknown_) {
        unknown = true;
        continue;
      }
      for (auto [w, r] : writesAt[size_t(node)]) {
        const Write &write = flows.writes[w];
        const Ref &target = write.target[size_t(r)];
        if (target.md != 0 || target.off < md) {
          continue;
        }
        int depth = target.off + off;
        if (node == conv.initializes && depth == 0) {
          continue;
        }
        for (const Narrowing &n : conv.narrowings) {
          if (breaks(write, n, depth) && mayBe(write, n)) {
            found = &write;
            broken = &n;
            break;
          }
        }
        if (found) {
          break;
        }
      }
      for (const Edge &e : flows.edges[size_t(node)]) {
        todo.push_back({e.to, off + e.off, std::max({e.md, md - e.off, 0})});
      }
    }
    if (!found && unknown) {
      for (const Write &write : flows.writes) {
        if (checker_.libraryCalls().inLibrary(write.loc)) {
          continue;
        }
        for (const Narrowing &n : conv.narrowings) {
          if (breaks(write, n, -1) && mayBe(write, n)) {
            found = &write;
            broken = &n;
            break;
          }
        }
        if (found) {
          break;
        }
      }
    }
    if (found) {
      reportWidening(conv.from, conv.to, conv.loc, conv.role, conv.name, found->loc, found->value,
                     found->through, broken->element, broken->name, broken->narrow, broken->owner,
                     broken->depth);
    }
  }
}

bool WideWrites::sameReference(const Type *from, const Type *to) {
  if (from == to) {
    return true;
  }
  if (to->isOptional()) {
    const Type *inner = to->inner();
    return (inner->isObject() || inner->isArray()) && sameReference(from->nonOptional(), inner);
  }
  if (from->isOptional()) {
    return false;
  }
  if (from->isArray() && to->isArray()) {
    return sameReference(from->inner(), to->inner());
  }
  if (from->isUnion()) {
    return !from->isTagged() && llvm::all_of(from->members(), [&](const Type *m) {
      return m->isObject() && sameReference(m, to);
    });
  }
  if (!from->isObject() || !to->isObject() || from->isAnyObject() || to->isAnyObject()) {
    return false;
  }
  if (from->isSubtypeOf(to) || GenericWidening::widens(from, to)) {
    return true;
  }
  if (from->isClass() || to->isClass() || from->isTuple() != to->isTuple() ||
      from->fields().size() != to->fields().size() || !from->overloads().empty() ||
      !to->overloads().empty() || from->isBytes() || to->isBytes() || from->isRuntimeObject() ||
      to->isRuntimeObject()) {
    return false;
  }

  return llvm::all_of(from->fields(), [&](const Field &f) {
    const Field *t = to->field(f.name);
    return t && t->type == f.type;
  });
}

void WideWrites::reportWidening(const Type *from, const Type *to, llvm::SMLoc at,
                                llvm::StringRef role, llvm::StringRef name, llvm::SMLoc write,
                                const Type *value, const Type *through, bool element,
                                llvm::StringRef field, const Type *narrow, const Type *owner,
                                int depth) {
  llvm::SourceMgr &sources = checker_.diags_.sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(write);
  std::string line =
      id ? "line " + std::to_string(sources.getLineAndColumn(write, id).first) : "a line";
  if (id && id != sources.FindBufferContainingLoc(at)) {
    line += " of " +
            llvm::sys::path::filename(sources.getMemoryBuffer(id)->getBufferIdentifier()).str();
  }
  std::string message = "'" + from->name().str() + "' cannot go where '" + to->name().str() +
                        "' is expected here" + (role.empty() ? std::string() : ", " + role.str()) +
                        ": " + line;
  auto article = [](const Type *t) {
    char c = t->name().empty() ? 'x' : char(std::tolower(static_cast<unsigned char>(t->name()[0])));

    return std::string(llvm::StringRef("aeiou").contains(c) ? "an '" : "a '") + t->name().str() +
           "'";
  };
  std::string what = value ? article(value) : std::string("a value");
  if (element) {
    message += " puts " + what + " into " + (depth == 0 ? std::string("it") : "an array in it") +
               ", and an array of '" + narrow->name().str() + "' cannot hold one";
    if (depth == 0) {
      message += "; pass a copy, '[..." + (name.empty() ? std::string("xs") : name.str()) + "]'";
    }
  } else {
    std::string slot = owner->isTuple() ? "element " + field.str() : "field '" + field.str() + "'";
    std::string place = depth == 0 ? "its " + slot : slot + " of " + article(owner) + " in it";
    if (!narrow) {
      message += " writes to " + place + ", which is read-only in '" + owner->name().str() + "'";
    } else {
      message += " writes " + what + " to " + place + ", which is " + article(narrow) + " in '" +
                 owner->name().str() + "'";
    }
  }
  checker_.diags_.error(at, message);
  checker_.diags_.note(write, "the write, through '" + (through ? through : to)->name() + "'");
}

}
