#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class DateLowerer {
public:
  explicit DateLowerer(Lowering &lowering);

  llvm::Value *emitDateCall(CallExpr *call);
  llvm::Value *newDate(llvm::Value *time);
  llvm::Value *dateTimeAddress(llvm::Value *date);
  llvm::Value *emitDateOptions(llvm::ArrayRef<Expr *> args, std::vector<Lowering::Operand> &held);

private:
  Lowering &lowering_;
};

}
