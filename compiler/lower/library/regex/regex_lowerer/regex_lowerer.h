#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class RegexLowerer {
public:
  explicit RegexLowerer(Lowering &owner);

  Lowering::Producer matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot,
                                      llvm::Value *lastSlot, llvm::Value *shape);
  llvm::FunctionCallee regexFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  llvm::Constant *regexProgram(const RegexLitExpr *lit);
  llvm::Value *emitRegexLiteral(RegexLitExpr *lit);
  llvm::Constant *matchShape(const Type *matchType, const RegexLitExpr *regex);
  llvm::Value *emitRegexCall(CallExpr *call);
  llvm::Value *emitReplaceWith(CallExpr *call);

private:
  static bool isReplaceAll(const MemberExpr *member);

  Lowering &lowering;
  std::map<std::vector<uint32_t>, llvm::Constant *> regexPrograms;
  std::map<std::pair<const Type *, std::vector<uint32_t>>, llvm::Constant *> matchShapes;
};

}
