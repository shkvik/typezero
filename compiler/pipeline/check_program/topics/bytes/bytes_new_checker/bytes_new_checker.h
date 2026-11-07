#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_new_checker/bytes_new_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class BytesNewChecker {
public:
  explicit BytesNewChecker(CheckState &owner);

  Expr *checkBytesNew(NewExpr *expr, const Type *type);
  Expr *checkTextDecoderParse(CallExpr *call, MemberExpr *member);
  Expr *checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type);
  bool checkBytesSource(Expr *source, const Type *target);

private:
  using ResultDecoderLabel = Result<const char *, BytesNewCheckerDecoderLabelError>;
  using ResultViewedBuffer = Result<const Type *, BytesNewCheckerViewedBufferError>;

  static ResultViewedBuffer viewedBuffer(const Type *buffer);
  static ResultDecoderLabel decoderLabel(llvm::StringRef label);
  static std::optional<std::u16string> literalText(const Expr *e);
  static bool mayBeFatal(const Expr *options);

  CheckState &state;
};

}
