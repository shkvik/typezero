#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class IteratorMethod {
  NEXT,
  RETURN,
  THROW,
  MAP,
  FILTER,
  TAKE,
  DROP,
  FLAT_MAP,
  TO_ARRAY,
  FOR_EACH,
  REDUCE,
  SOME,
  EVERY,
  FIND,
};

enum class IteratorMethodKind {
  STEP,
  CLOSING,
  STAGE,
  CONSUMER,
};

struct IteratorStepMethod {
  static constexpr IteratorMethodKind kind = IteratorMethodKind::STEP;
  Builtin builtin;
};

struct IteratorClosingMethod {
  static constexpr IteratorMethodKind kind = IteratorMethodKind::CLOSING;
};

struct IteratorStageMethod {
  static constexpr IteratorMethodKind kind = IteratorMethodKind::STAGE;
  Builtin builtin;
};

struct IteratorConsumerMethod {
  static constexpr IteratorMethodKind kind = IteratorMethodKind::CONSUMER;
  Builtin builtin;
};

using IteratorMethodForm =
    Union<IteratorStepMethod, IteratorClosingMethod, IteratorStageMethod, IteratorConsumerMethod>;

struct IteratorMethodInfo {
  llvm::StringLiteral name;
  IteratorMethod method;
  IteratorMethodForm form;
};

class IteratorMethodTable {
public:
  static const IteratorMethodInfo *find(llvm::StringRef name);
  static llvm::StringRef getName(IteratorMethod method);
};

}
