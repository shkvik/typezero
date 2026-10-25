#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Dates and Intl.DateTimeFormat.
class DateLowerer {
public:
  explicit DateLowerer(CodeGen &codegen);

  // Dates (codegen_date.cpp). A Date is an object { i64 rc, ptr layout,
  // double time } that the code makes; the runtime
  // (runtime/library/date/date/date.c) computes with times. An
  // Intl.DateTimeFormat is an object of the runtime.
  llvm::Value *emitDateCall(CallExpr *call);
  // A new date that holds `time`.
  llvm::Value *newDate(llvm::Value *time);
  llvm::Value *dateTimeAddress(llvm::Value *date);
  // The TzDateOptions of a locale and options (both may be left out) on the
  // stack; `held` gets the operands to drop after the call.
  llvm::Value *emitDateOptions(llvm::ArrayRef<Expr *> args, std::vector<CodeGen::Operand> &held);

private:
  CodeGen &codegen_;
};

} // namespace tz
