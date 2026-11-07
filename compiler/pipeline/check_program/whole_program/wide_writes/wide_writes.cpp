#include "compiler/pipeline/check_program/whole_program/wide_writes/wide_writes.h"
#include "compiler/pipeline/check_program/generics/generic_widening/generic_widening.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <tuple>

namespace tz {

WideWrites::WideWrites(CheckState &owner)
    : state(owner) {}

void WideWrites::collectNarrowings(const Type *from, const Type *to, int depth,
                                   std::vector<Narrowing> &out, TypePairs &seen) {
  from = from->nonOptional();
  to = to->nonOptional();
  if (from == to || depth > WideWrites::maxDepth) {
    return;
  }
  bool firstVisit = seen.insert({from, to}).second;
  if (!firstVisit) {
    return;
  }
  if (from->isArray() && to->isArray()) {
    if (from->inner() != to->inner()) {
      out.push_back(Narrowing{
          .depth = depth,
          .element = true,
          .name = "",
          .narrow = from->inner(),
          .owner = from,
      });
    }
    WideWrites::collectNarrowings(from->inner(), to->inner(), depth + 1, out, seen);
    return;
  }
  if (from->isUnion()) {
    if (llvm::is_contained(from->members(), to)) {
      return;
    }
    for (const Type *member : from->members()) {
      WideWrites::collectNarrowings(member, to, depth, out, seen);
    }
    return;
  }
  bool objects = from->isObject() && to->isObject() && !from->isAnyObject() && !to->isAnyObject();
  if (!objects) {
    return;
  }
  for (const Field &wide : to->fields()) {
    const Field *own = from->field(wide.name);
    if (!own) {
      continue;
    }
    if (own->readonly && !wide.readonly) {
      out.push_back(Narrowing{
          .depth = depth,
          .element = false,
          .name = wide.name,
          .narrow = nullptr,
          .owner = from,
      });
    } else if (own->type != wide.type) {
      out.push_back(Narrowing{
          .depth = depth,
          .element = false,
          .name = wide.name,
          .narrow = own->type,
          .owner = from,
      });
    }
    if (own->type != wide.type) {
      WideWrites::collectNarrowings(own->type, wide.type, depth + 1, out, seen);
    }
  }
}

std::vector<Narrowing> WideWrites::narrowingsOf(const Type *from, const Type *to) {
  std::vector<Narrowing> out;
  TypePairs seen;
  WideWrites::collectNarrowings(from, to, 0, out, seen);

  return out;
}

bool WideWrites::widensWrites(const Type *from, const Type *to) {
  const Type *bareFrom = from->nonOptional();
  const Type *bareTo = to->nonOptional();
  if (bareFrom == bareTo || GenericWidening::widens(bareFrom, bareTo)) {
    return false;
  }
  bool arrays = bareFrom->isArray() && bareTo->isArray();
  bool objects = bareFrom->isObject() && bareTo->isObject() && !bareFrom->isAnyObject() &&
                 !bareTo->isAnyObject();
  bool unionToObject = bareFrom->isUnion() && bareTo->isObject();
  if (!arrays && !objects && !unionToObject) {
    return false;
  }
  WideningStore &widenings = this->state.getWideningStore();
  if (widenings.hasKnownWidening(bareFrom, bareTo)) {
    return widenings.isNarrowingWidening(bareFrom, bareTo);
  }
  std::vector<Narrowing> narrowings = WideWrites::narrowingsOf(bareFrom, bareTo);
  bool narrowed = !narrowings.empty();
  widenings.rememberWidening(bareFrom, bareTo, narrowed);

  return narrowed;
}

bool WideWrites::breaks(const Write &write, const Narrowing &narrowing, int depth) {
  bool otherDepth = depth >= 0 && narrowing.depth != depth;
  if (otherDepth || write.element != narrowing.element) {
    return false;
  }
  bool otherName = !write.element && write.name != "*" && write.name != narrowing.name;
  if (otherName) {
    return false;
  }

  return !narrowing.narrow || !write.value ||
         !TypeRelations::isImplicitlyConvertible(write.value, narrowing.narrow);
}

bool WideWrites::mayBe(const Write &write, const Narrowing &narrowing) {
  const Type *through = write.through ? write.through->nonOptional() : nullptr;
  bool open = !through || !narrowing.owner || TypeQueries::mentionsParam(through) ||
              through->isUnknown() || through->isAnyObject();
  if (open) {
    return true;
  }
  if (narrowing.element) {
    return !through->isArray() || !narrowing.narrow ||
           TypeQueries::mentionsParam(through->inner()) ||
           TypeRelations::isImplicitlyConvertible(narrowing.narrow, through->inner());
  }

  return TypeRelations::isImplicitlyConvertible(narrowing.owner, through);
}

bool WideWrites::mayHold(const Type *type, const Type *value, int depth) {
  if (!type) {
    return true;
  }
  type = type->nonOptional();
  bool open = type->isUnknown() || type->isAnyObject() || type->isError() ||
              TypeQueries::mentionsParam(type);
  if (open) {
    return true;
  }
  if (depth == 0) {
    return TypeRelations::isImplicitlyConvertible(value, type);
  }
  if (!Flows::holdsReferences(type)) {
    return false;
  }
  if (type->isArray()) {
    return WideWrites::mayHold(type->inner(), value, depth - 1);
  }
  if (type->isDict()) {
    return WideWrites::mayHold(type->keyType(), value, depth - 1) ||
           WideWrites::mayHold(type->valueType(), value, depth - 1);
  }
  auto memberHolds = [&](const Type *member) { return WideWrites::mayHold(member, value, depth); };
  auto fieldHolds = [&](const Field &field) {
    return WideWrites::mayHold(field.type, value, depth - 1);
  };
  if (type->isUnion()) {
    return llvm::any_of(type->members(), memberHolds);
  }
  if (type->isTuple()) {
    return llvm::any_of(type->fields(), fieldHolds);
  }

  return true;
}

bool WideWrites::mayBePart(const Type *type, const Type *whole, int depth) {
  if (!type || !whole) {
    return true;
  }
  type = type->nonOptional();
  whole = whole->nonOptional();
  if (depth == 0) {
    return WideWrites::mayHold(type, whole, 0) || WideWrites::mayHold(whole, type, 0);
  }
  bool open = whole->isUnknown() || whole->isAnyObject() || whole->isError() ||
              TypeQueries::mentionsParam(whole);
  if (open) {
    return true;
  }
  if (!Flows::holdsReferences(whole)) {
    return false;
  }
  if (whole->isArray()) {
    return WideWrites::mayBePart(type, whole->inner(), depth - 1);
  }
  if (whole->isDict()) {
    return WideWrites::mayBePart(type, whole->keyType(), depth - 1) ||
           WideWrites::mayBePart(type, whole->valueType(), depth - 1);
  }
  auto memberPart = [&](const Type *member) { return WideWrites::mayBePart(type, member, depth); };
  auto fieldPart = [&](const Field &field) {
    return WideWrites::mayBePart(type, field.type, depth - 1);
  };
  if (whole->isUnion()) {
    return llvm::any_of(whole->members(), memberPart);
  }
  const ClassDecl *cls = whole->classDecl();
  bool closed = whole->isTuple() || (cls && cls->children.empty());
  if (closed) {
    return llvm::any_of(whole->fields(), fieldPart);
  }

  return true;
}

WideWrites::WritesAt WideWrites::writesByNode(const FlowGraph &graph) {
  WritesAt writesAt(graph.edges.size());
  for (size_t w = 0; w < graph.writes.size(); ++w) {
    const Refs &targets = graph.writes[w].target;
    for (size_t r = 0; r < targets.size(); ++r) {
      writesAt[size_t(targets[r].node)].push_back(WriteAt{.write = w, .target = r});
    }
  }

  return writesAt;
}

bool WideWrites::nodeMayHold(const FlowGraph &graph, const Conversion &conv, int node, int off) {
  const Type *type = size_t(node) < graph.types.size() ? graph.types[size_t(node)] : nullptr;
  if (!type) {
    return true;
  }
  if (off <= 0) {
    return WideWrites::mayHold(type, conv.from, -off) || WideWrites::mayHold(type, conv.to, -off);
  }

  return WideWrites::mayBePart(type, conv.from, off);
}

WideWrites::WriteBreak WideWrites::breakingNarrowing(const Write &write, const Conversion &conv,
                                                     int depth) {
  for (const Narrowing &narrowing : conv.narrowings) {
    bool broken =
        WideWrites::breaks(write, narrowing, depth) && WideWrites::mayBe(write, narrowing);
    if (broken) {
      return WriteBreak{.write = &write, .narrowing = &narrowing};
    }
  }

  return WriteBreak{};
}

WideWrites::WriteBreak WideWrites::breakingAt(const FlowGraph &graph, const WritesAt &writesAt,
                                              const Conversion &conv, int node, int off, int md) {
  for (const WriteAt &at : writesAt[size_t(node)]) {
    const Write &write = graph.writes[at.write];
    const Ref &target = write.target[at.target];
    if (target.md != 0 || target.off < md) {
      continue;
    }
    int depth = target.off + off;
    if (node == conv.initializes && depth == 0) {
      continue;
    }
    WriteBreak found = WideWrites::breakingNarrowing(write, conv, depth);
    if (found.write) {
      return found;
    }
  }

  return WriteBreak{};
}

WideWrites::WriteBreak WideWrites::breakingAnywhere(const FlowGraph &graph,
                                                    const Conversion &conv) const {
  for (const Write &write : graph.writes) {
    if (LibraryCalls(this->state).inLibrary(write.loc)) {
      continue;
    }
    WriteBreak found = WideWrites::breakingNarrowing(write, conv, -1);
    if (found.write) {
      return found;
    }
  }

  return WriteBreak{};
}

WideWrites::WriteBreak WideWrites::breakingWrite(const FlowGraph &graph, const WritesAt &writesAt,
                                                 const Conversion &conv) const {
  std::set<std::tuple<int, int, int>> seen;
  std::vector<std::tuple<int, int, int>> todo{{conv.node, 0, 0}};
  WriteBreak found;
  bool unknown = false;
  while (!todo.empty() && !found.write) {
    auto [node, off, md] = todo.back();
    todo.pop_back();
    bool outside = off > WideWrites::maxDepth || off < -16 || md > 16;
    if (outside) {
      continue;
    }
    bool firstVisit = seen.insert({node, off, md}).second;
    if (!firstVisit || !WideWrites::nodeMayHold(graph, conv, node, off)) {
      continue;
    }
    if (node == graph.unknownNode) {
      unknown = true;
      continue;
    }
    found = WideWrites::breakingAt(graph, writesAt, conv, node, off, md);
    for (const Edge &edge : graph.edges[size_t(node)]) {
      int nextMd = std::max({edge.md, md - edge.off, 0});
      todo.push_back({edge.to, off + edge.off, nextMd});
    }
  }
  if (!found.write && unknown) {
    found = this->breakingAnywhere(graph, conv);
  }

  return found;
}

std::string WideWrites::writeLine(llvm::SMLoc write, llvm::SMLoc at) const {
  llvm::SourceMgr &sources = this->state.getDiagnostics().sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(write);
  if (!id) {
    return ErrorTextWidening::someLine();
  }
  int number = int(sources.getLineAndColumn(write, id).first);
  if (id == sources.FindBufferContainingLoc(at)) {
    return ErrorTextWidening::line(number);
  }
  llvm::StringRef identifier = sources.getMemoryBuffer(id)->getBufferIdentifier();
  llvm::StringRef file = llvm::sys::path::filename(identifier);

  return ErrorTextWidening::lineOf(number, file);
}

WideWrites::ResultWideningSafe WideWrites::widening(const Conversion &conv,
                                                    const WriteBreak &breaking) const {
  const Write *write = breaking.write;
  const Narrowing *narrowing = breaking.narrowing;
  std::string line = write ? this->writeLine(write->loc, conv.loc) : std::string();
  std::string head = write ? ErrorTextWidening::head(conv.from, conv.to, conv.role, line) : line;
  if (write && narrowing->element && narrowing->depth == 0) {
    std::string message =
        ErrorTextWidening::intoArray(head, write->value, narrowing->narrow, conv.name);

    return err(WideWritesWideningError{
        .code = WideWritesWideningErrorCode::PUTS_INTO_ARRAY,
        .message = message,
    });
  }
  if (write && narrowing->element) {
    std::string message = ErrorTextWidening::intoInner(head, write->value, narrowing->narrow);

    return err(WideWritesWideningError{
        .code = WideWritesWideningErrorCode::PUTS_INTO_INNER_ARRAY,
        .message = message,
    });
  }
  if (write && !narrowing->narrow) {
    std::string message =
        ErrorTextWidening::readOnly(head, narrowing->owner, narrowing->name, narrowing->depth);

    return err(WideWritesWideningError{
        .code = WideWritesWideningErrorCode::WRITES_READ_ONLY,
        .message = message,
    });
  }
  if (write) {
    std::string message = ErrorTextWidening::otherType(
        head, write->value, narrowing->owner, narrowing->name, narrowing->narrow, narrowing->depth);

    return err(WideWritesWideningError{
        .code = WideWritesWideningErrorCode::WRITES_OTHER_TYPE,
        .message = message,
    });
  }

  return ok(&conv);
}

void WideWrites::checkWidenings(SourceFile &file) {
  WideningStore &widenings = this->state.getWideningStore();
  if (!widenings.hasWideningCasts() && !widenings.hasNarrowedFields()) {
    return;
  }
  auto narrowings = [](const Type *from, const Type *to) {
    return WideWrites::narrowingsOf(from, to);
  };
  auto convertible = [](const Type *from, const Type *to) {
    return TypeRelations::isImplicitlyConvertible(from, to);
  };
  Flows flows(widenings, narrowings, convertible);
  flows.walkProgram(file);
  const FlowGraph &graph = flows.getFlowGraph();

  WritesAt writesAt = WideWrites::writesByNode(graph);
  std::set<const char *> reported;
  for (const Conversion &conv : graph.conversions) {
    if (conv.narrowings.empty()) {
      continue;
    }
    bool firstReport = reported.insert(conv.loc.getPointer()).second;
    if (!firstReport) {
      continue;
    }
    WriteBreak breaking = this->breakingWrite(graph, writesAt, conv);
    ResultWideningSafe safe = this->widening(conv, breaking);
    if (safe.isErr()) {
      const Type *through = breaking.write->through ? breaking.write->through : conv.to;
      std::string note = ErrorTextWidening::through(through);
      this->state.getDiagnostics().error(conv.loc, safe.error().message);
      this->state.getDiagnostics().note(breaking.write->loc, note);
    }
  }
}

bool WideWrites::sameReference(const Type *from, const Type *to) {
  if (from == to) {
    return true;
  }
  if (to->isOptional()) {
    const Type *inner = to->inner();
    bool reference = inner->isObject() || inner->isArray();
    return reference && WideWrites::sameReference(from->nonOptional(), inner);
  }
  if (from->isOptional()) {
    return false;
  }
  if (from->isArray() && to->isArray()) {
    return WideWrites::sameReference(from->inner(), to->inner());
  }
  auto sameObject = [&](const Type *member) {
    return member->isObject() && WideWrites::sameReference(member, to);
  };
  if (from->isUnion()) {
    return !from->isTagged() && llvm::all_of(from->members(), sameObject);
  }
  bool objects = from->isObject() && to->isObject() && !from->isAnyObject() && !to->isAnyObject();
  if (!objects) {
    return false;
  }
  if (from->isSubtypeOf(to) || GenericWidening::widens(from, to)) {
    return true;
  }
  bool otherShape = from->isClass() || to->isClass() || from->isTuple() != to->isTuple() ||
                    from->fields().size() != to->fields().size() || !from->overloads().empty() ||
                    !to->overloads().empty() || from->isBytes() || to->isBytes() ||
                    from->isRuntimeObject() || to->isRuntimeObject();
  if (otherShape) {
    return false;
  }
  auto sameField = [&](const Field &field) {
    const Field *other = to->field(field.name);
    return other && other->type == field.type;
  };

  return llvm::all_of(from->fields(), sameField);
}

}
