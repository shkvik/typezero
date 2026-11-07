#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/whole_program/wide_writes/wide_writes_errors.h"
#include "compiler/result/result/result.h"
#include <map>
#include <set>
#include <vector>

namespace tz {

struct Write;
struct Narrowing;
struct Conversion;
class Flows;

class WideWrites {
public:
  explicit WideWrites(CheckState &owner);

  static bool sameReference(const Type *from, const Type *to);

  void checkWidenings(SourceFile &file);
  bool widensWrites(const Type *from, const Type *to);

private:
  struct WriteAt {
    size_t write = 0;
    size_t target = 0;
  };

  struct WriteBreak {
    const Write *write = nullptr;
    const Narrowing *narrowing = nullptr;
  };

  using WritesAt = std::vector<std::vector<WriteAt>>;
  using TypePairs = std::set<std::pair<const Type *, const Type *>>;
  using ResultWideningSafe = Result<const Conversion *, WideWritesWideningError>;

  static constexpr int maxDepth = 6;

  static WritesAt writesByNode(const Flows &flows);
  static bool mayBe(const Write &write, const Narrowing &narrowing);
  static bool mayHold(const Type *type, const Type *value, int depth);
  static bool mayBePart(const Type *type, const Type *whole, int depth);
  static std::vector<Narrowing> narrowingsOf(const Type *from, const Type *to);
  static bool breaks(const Write &write, const Narrowing &narrowing, int depth);
  static bool nodeMayHold(const Flows &flows, const Conversion &conv, int node, int off);
  static WriteBreak breakingNarrowing(const Write &write, const Conversion &conv, int depth);
  static void collectNarrowings(const Type *from, const Type *to, int depth,
                                std::vector<Narrowing> &out, TypePairs &seen);
  static WriteBreak breakingAt(const Flows &flows, const WritesAt &writesAt, const Conversion &conv,
                               int node, int off, int md);

  std::string writeLine(llvm::SMLoc write, llvm::SMLoc at) const;
  WriteBreak breakingAnywhere(const Flows &flows, const Conversion &conv) const;
  ResultWideningSafe widening(const Conversion &conv, const WriteBreak &breaking) const;
  WriteBreak breakingWrite(const Flows &flows, const WritesAt &writesAt,
                           const Conversion &conv) const;

  CheckState &state;
};

}
