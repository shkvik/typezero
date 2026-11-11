#include "compiler/interprocedural/wide_writes/wide_writes.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_graph_builder/flow_graph_builder.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <tuple>

namespace tz {

WideWrites::WideWrites(Program &checked, const LibraryRanges &library)
    : program(checked),
      library(library) {}

bool WideWrites::breaks(const Write &write, const NarrowedPart &narrowing, int depth) const {
  bool isOtherDepth = depth >= 0 && narrowing.depth != depth;
  if (isOtherDepth || write.element != narrowing.element) {
    return false;
  }
  bool isOtherName = !write.element && write.name != "*" && write.name != narrowing.name;
  if (isOtherName) {
    return false;
  }

  return !narrowing.narrow || !write.value ||
         !SemanticTypes::isImplicitlyConvertible(write.value, narrowing.narrow);
}

bool WideWrites::mayBe(const Write &write, const NarrowedPart &narrowing) const {
  const Type *through = write.through ? write.through->nonOptional() : nullptr;
  bool isOpen = !through || !narrowing.owner || TypeQueries::mentionsParam(through) ||
                through->isUnknown() || through->isAnyObject();
  if (isOpen) {
    return true;
  }
  if (narrowing.element) {
    return !through->isArray() || !narrowing.narrow ||
           TypeQueries::mentionsParam(through->inner()) ||
           SemanticTypes::isImplicitlyConvertible(narrowing.narrow, through->inner());
  }

  return SemanticTypes::isImplicitlyConvertible(narrowing.owner, through);
}

bool WideWrites::mayHold(const Type *type, const Type *value, int depth) const {
  auto pending = std::vector<TypeGoal>{TypeGoal{.type = type, .depth = depth}};
  while (!pending.empty()) {
    TypeGoal goal = pending.back();
    pending.pop_back();
    TypeStep step = this->holdStep(goal, value);
    if (step.holds) {
      return true;
    }
    pending.insert(pending.end(), step.needs.rbegin(), step.needs.rend());
  }

  return false;
}

WideWrites::TypeStep WideWrites::holdStep(const TypeGoal &goal, const Type *value) const {
  if (!goal.type) {
    return TypeStep{.holds = true};
  }
  const Type *type = goal.type->nonOptional();
  bool isOpen = type->isUnknown() || type->isAnyObject() || type->isError() ||
                TypeQueries::mentionsParam(type);
  if (isOpen) {
    return TypeStep{.holds = true};
  }
  if (goal.depth == 0) {
    bool isHolding = SemanticTypes::isImplicitlyConvertible(value, type);
    return TypeStep{.holds = isHolding};
  }
  if (!FlowGraphBuilder::holdsReferences(type)) {
    return TypeStep{};
  }
  if (type->isArray() || type->isDict() || type->isUnion() || type->isTuple()) {
    auto needs = this->innerGoals(type, goal.depth);
    return TypeStep{.needs = needs};
  }

  return TypeStep{.holds = true};
}

bool WideWrites::mayBePart(const Type *type, const Type *whole, int depth) const {
  if (!type) {
    return true;
  }
  const Type *part = type->nonOptional();
  auto pending = std::vector<TypeGoal>{TypeGoal{.type = whole, .depth = depth}};
  while (!pending.empty()) {
    TypeGoal goal = pending.back();
    pending.pop_back();
    TypeStep step = this->partStep(part, goal);
    if (step.holds) {
      return true;
    }
    pending.insert(pending.end(), step.needs.rbegin(), step.needs.rend());
  }

  return false;
}

WideWrites::TypeStep WideWrites::partStep(const Type *type, const TypeGoal &goal) const {
  if (!goal.type) {
    return TypeStep{.holds = true};
  }
  const Type *whole = goal.type->nonOptional();
  if (goal.depth == 0) {
    bool isHolding = this->mayHold(type, whole, 0) || this->mayHold(whole, type, 0);
    return TypeStep{.holds = isHolding};
  }
  bool isOpen = whole->isUnknown() || whole->isAnyObject() || whole->isError() ||
                TypeQueries::mentionsParam(whole);
  if (isOpen) {
    return TypeStep{.holds = true};
  }
  if (!FlowGraphBuilder::holdsReferences(whole)) {
    return TypeStep{};
  }
  if (whole->isArray() || whole->isDict() || whole->isUnion() || whole->isTuple()) {
    auto needs = this->innerGoals(whole, goal.depth);
    return TypeStep{.needs = needs};
  }
  const ClassDecl *cls = whole->classDecl();
  bool isClosedClass = cls != nullptr && cls->children.empty();
  if (isClosedClass) {
    auto needs = std::vector<TypeGoal>();
    for (const Field &field : whole->fields()) {
      needs.push_back(TypeGoal{.type = field.type, .depth = goal.depth - 1});
    }
    return TypeStep{.needs = needs};
  }

  return TypeStep{.holds = true};
}

std::vector<WideWrites::TypeGoal> WideWrites::innerGoals(const Type *type, int depth) const {
  auto goals = std::vector<TypeGoal>();
  if (type->isArray()) {
    goals.push_back(TypeGoal{.type = type->inner(), .depth = depth - 1});
    return goals;
  }
  if (type->isDict()) {
    goals.push_back(TypeGoal{.type = type->keyType(), .depth = depth - 1});
    goals.push_back(TypeGoal{.type = type->valueType(), .depth = depth - 1});
    return goals;
  }
  if (type->isUnion()) {
    for (const Type *member : type->members()) {
      goals.push_back(TypeGoal{.type = member, .depth = depth});
    }
    return goals;
  }
  for (const Field &field : type->fields()) {
    goals.push_back(TypeGoal{.type = field.type, .depth = depth - 1});
  }

  return goals;
}

WideWrites::WritesAt WideWrites::writesByNode(const FlowGraph &graph) const {
  WritesAt writesAt(graph.edges.size());
  for (size_t w = 0; w < graph.writes.size(); ++w) {
    const Refs &targets = graph.writes[w].target;
    for (size_t r = 0; r < targets.size(); ++r) {
      writesAt[size_t(targets[r].node)].push_back(WriteAt{.write = w, .target = r});
    }
  }

  return writesAt;
}

bool WideWrites::nodeMayHold(const FlowGraph &graph, const Conversion &conv, int node,
                             int off) const {
  const Type *type = size_t(node) < graph.types.size() ? graph.types[size_t(node)] : nullptr;
  if (!type) {
    return true;
  }
  if (off <= 0) {
    return this->mayHold(type, conv.from, -off) || this->mayHold(type, conv.to, -off);
  }

  return this->mayBePart(type, conv.from, off);
}

WideWrites::WriteBreak WideWrites::breakingNarrowing(const Write &write, const Conversion &conv,
                                                     int depth) const {
  for (const NarrowedPart &narrowing : conv.narrowings) {
    bool isBroken = this->breaks(write, narrowing, depth) && this->mayBe(write, narrowing);
    if (isBroken) {
      return WriteBreak{.write = &write, .narrowing = &narrowing};
    }
  }

  return WriteBreak{};
}

WideWrites::WriteBreak WideWrites::breakingAt(const FlowGraph &graph, const WritesAt &writesAt,
                                              const Conversion &conv, int node, int off,
                                              int md) const {
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
    auto found = this->breakingNarrowing(write, conv, depth);
    if (found.write) {
      return found;
    }
  }

  return WriteBreak{};
}

WideWrites::WriteBreak WideWrites::breakingAnywhere(const FlowGraph &graph,
                                                    const Conversion &conv) const {
  for (const Write &write : graph.writes) {
    if (this->library.contains(write.loc)) {
      continue;
    }
    auto found = this->breakingNarrowing(write, conv, -1);
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
  bool isUnknown = false;
  while (!todo.empty() && !found.write) {
    auto [node, off, md] = todo.back();
    todo.pop_back();
    bool isOutside = off > WideWrites::maxDepth || off < -16 || md > 16;
    if (isOutside) {
      continue;
    }
    auto [_, isFirstVisit] = seen.insert({node, off, md});
    if (!isFirstVisit || !this->nodeMayHold(graph, conv, node, off)) {
      continue;
    }
    if (node == graph.unknownNode) {
      isUnknown = true;
      continue;
    }
    found = this->breakingAt(graph, writesAt, conv, node, off, md);
    for (const Edge &edge : graph.edges[size_t(node)]) {
      int nextMd = std::max({edge.md, md - edge.off, 0});
      todo.push_back({edge.to, off + edge.off, nextMd});
    }
  }
  if (!found.write && isUnknown) {
    found = this->breakingAnywhere(graph, conv);
  }

  return found;
}

std::string WideWrites::writeLine(llvm::SMLoc write, llvm::SMLoc at) const {
  llvm::SourceMgr &sources = this->program.sourceMgr;
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

WideWrites::ResultWideningsChecked WideWrites::checkWidenings() {
  const auto &wideningStore = this->program.file.inventory.wideningStore;
  if (!wideningStore.hasWideningCasts() && !wideningStore.hasNarrowedFields()) {
    return ok();
  }

  auto narrowings = [](const Type *from, const Type *to) {
    return SemanticTypes::narrowingsOf(from, to);
  };
  auto convertible = [](const Type *from, const Type *to) {
    return SemanticTypes::isImplicitlyConvertible(from, to);
  };

  auto builder = FlowGraphBuilder(wideningStore, narrowings, convertible);
  auto graph = builder.buildGraph(this->program.file);

  auto reportedConversions = std::set<const char *>();
  auto nodeWrites = this->writesByNode(graph);
  auto diagnostics = std::vector<Diagnostic>();

  for (const Conversion &conv : graph.conversions) {
    if (conv.narrowings.empty()) {
      continue;
    }
    if (reportedConversions.contains(conv.loc.getPointer())) {
      continue;
    }

    auto breaking = this->breakingWrite(graph, nodeWrites, conv);
    auto safe = this->widening(conv, breaking);
    if (safe.isOk()) {
      continue;
    }
    reportedConversions.insert(conv.loc.getPointer());

    const auto *through = breaking.write->through ? breaking.write->through : conv.to;
    auto note = ErrorTextWidening::through(through);

    diagnostics.push_back({
        .kind = llvm::SourceMgr::DK_Error,
        .loc = conv.loc,
        .message = safe.error().message,
    });
    diagnostics.push_back({
        .kind = llvm::SourceMgr::DK_Note,
        .loc = breaking.write->loc,
        .message = note,
    });
  }
  if (!diagnostics.empty()) {
    return err(diagnostics);
  }

  return ok();
}

}
