#include "compiler/interprocedural/wide_writes/wide_writes.h"
#include "compiler/interprocedural/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <tuple>

namespace tz {

WideWrites::WideWrites(SemanticExternalFacade &owner)
    : engine(owner) {}

bool WideWrites::breaks(const Write &write, const NarrowedPart &narrowing, int depth) {
  bool otherDepth = depth >= 0 && narrowing.depth != depth;
  if (otherDepth || write.element != narrowing.element) {
    return false;
  }
  bool otherName = !write.element && write.name != "*" && write.name != narrowing.name;
  if (otherName) {
    return false;
  }

  return !narrowing.narrow || !write.value ||
         !SemanticExternalFacade::isImplicitlyConvertible(write.value, narrowing.narrow);
}

bool WideWrites::mayBe(const Write &write, const NarrowedPart &narrowing) {
  const Type *through = write.through ? write.through->nonOptional() : nullptr;
  bool open = !through || !narrowing.owner || TypeQueries::mentionsParam(through) ||
              through->isUnknown() || through->isAnyObject();
  if (open) {
    return true;
  }
  if (narrowing.element) {
    return !through->isArray() || !narrowing.narrow ||
           TypeQueries::mentionsParam(through->inner()) ||
           SemanticExternalFacade::isImplicitlyConvertible(narrowing.narrow, through->inner());
  }

  return SemanticExternalFacade::isImplicitlyConvertible(narrowing.owner, through);
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
    return SemanticExternalFacade::isImplicitlyConvertible(value, type);
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
  for (const NarrowedPart &narrowing : conv.narrowings) {
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
    if (this->engine.isInsideLibrary(write.loc)) {
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
  llvm::SourceMgr &sources = this->engine.getDiagnostics().sourceMgr();
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
  const NarrowedPart *narrowing = breaking.narrowing;
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
  const WideningStore &widenings = this->engine.getWideningStore();
  if (!widenings.hasWideningCasts() && !widenings.hasNarrowedFields()) {
    return;
  }
  auto narrowings = [](const Type *from, const Type *to) {
    return SemanticExternalFacade::narrowingsOf(from, to);
  };
  auto convertible = [](const Type *from, const Type *to) {
    return SemanticExternalFacade::isImplicitlyConvertible(from, to);
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
      this->engine.getDiagnostics().error(conv.loc, safe.error().message);
      this->engine.getDiagnostics().note(breaking.write->loc, note);
    }
  }
}

}
