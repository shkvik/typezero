#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/bytes/buffer_checker/buffer_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class BufferChecker {
public:
  explicit BufferChecker(CheckState &owner);

  Expr *checkBufferMethod(CallExpr *call, MemberExpr *member);
  Expr *checkTextMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultCountOfBytes = Result<int64_t, BufferCheckerCountOfBytesError>;
  using ResultDecodedInput = Result<const Type *, BufferCheckerDecodedInputError>;

  static ResultDecodedInput decodedInput(const Type *type);
  static ResultCountOfBytes countOfBytes(std::optional<int64_t> count, llvm::StringRef name);
  static std::optional<std::u16string> literalText(const Expr *e);

  CheckState &state;

  void checkWriteLength(const Expr *object, const Expr *offset, const Expr *length);
};

}
