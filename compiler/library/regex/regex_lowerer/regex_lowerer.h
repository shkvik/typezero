#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Regular expressions, the descriptions of matches, replace with a function.
class RegexLowerer {
public:
  explicit RegexLowerer(CodeGen &codegen);

  // `s.matchAll(re)` (codegen_regex.cpp): the matches of the regex held in
  // `regexSlot` over the string in `textSlot`, as a copy of the regex whose
  // lastIndex is in `lastSlot` finds them; `shape` builds each.
  CodeGen::Producer matchAllProducer(llvm::Value *textSlot, llvm::Value *regexSlot, llvm::Value *lastSlot,
                            llvm::Value *shape);
  // Regular expressions (codegen_regex.cpp). A literal's program is a
  // constant; a literal without g and y is a constant object too (see
  // TzRegex in runtime/core/tzrt/tzrt.h).
  llvm::FunctionCallee regexFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  llvm::Constant *regexProgram(const RegexLitExpr *lit);
  llvm::Value *emitRegexLiteral(RegexLitExpr *lit);
  // How the runtime builds matches of `matchType` (TzMatchShape) for
  // `regex`, whose names tell which captures the named groups are; null for
  // a regex not known.
  llvm::Constant *matchShape(const Type *matchType, const RegexLitExpr *regex);
  llvm::Value *emitRegexCall(CallExpr *call);
  // replace() and replaceAll() with a function.
  llvm::Value *emitReplaceWith(CallExpr *call);

private:
  CodeGen &codegen_;
  std::map<std::vector<uint32_t>, llvm::Constant *> regexPrograms_;
  std::map<std::pair<const Type *, std::vector<uint32_t>>, llvm::Constant *> matchShapes_;
};

} // namespace tz
