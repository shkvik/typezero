#pragma once

#include "compiler/interprocedural/wide_writes/wide_writes_errors.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/program/library_ranges/library_ranges.h"
#include "compiler/program/program/program.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_types.h"
#include <map>
#include <set>
#include <vector>

namespace tz {

struct Write;
struct Conversion;
struct FlowGraph;

class WideWrites {
public:
  using ResultWideningsChecked = Result<void, std::vector<Diagnostic>>;

  WideWrites(Program &checked, const LibraryRanges &library);

  ResultWideningsChecked checkWidenings();

private:
  struct WriteAt {
    size_t write = 0;
    size_t target = 0;
  };

  struct WriteBreak {
    const Write *write = nullptr;
    const NarrowedPart *narrowing = nullptr;
  };

  using WritesAt = std::vector<std::vector<WriteAt>>;
  using ResultWideningSafe = Result<const Conversion *, WideWritesWideningError>;

  static constexpr int maxDepth = 6;

  static WritesAt writesByNode(const FlowGraph &graph);
  static bool mayBe(const Write &write, const NarrowedPart &narrowing);
  static bool mayHold(const Type *type, const Type *value, int depth);
  static bool mayBePart(const Type *type, const Type *whole, int depth);
  static bool breaks(const Write &write, const NarrowedPart &narrowing, int depth);
  static bool nodeMayHold(const FlowGraph &graph, const Conversion &conv, int node, int off);
  static WriteBreak breakingNarrowing(const Write &write, const Conversion &conv, int depth);
  static WriteBreak breakingAt(const FlowGraph &graph, const WritesAt &writesAt,
                               const Conversion &conv, int node, int off, int md);

  std::string writeLine(llvm::SMLoc write, llvm::SMLoc at) const;
  WriteBreak breakingAnywhere(const FlowGraph &graph, const Conversion &conv) const;
  ResultWideningSafe widening(const Conversion &conv, const WriteBreak &breaking) const;
  WriteBreak breakingWrite(const FlowGraph &graph, const WritesAt &writesAt,
                           const Conversion &conv) const;

  Program &program;
  const LibraryRanges &library;
};

}
