#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace tz {

bool NodeCheckHelpers::isTargetWindows() {
  return llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows();
}

Expr *NodeCheckHelpers::stringLiteral(AstContext &context, llvm::SMLoc loc, llvm::StringRef text) {
  auto units = llvm::SmallVector<llvm::UTF16, 32>();
  llvm::convertUTF8ToUTF16String(text, units);
  auto *literal = context.make<StringLitExpr>(loc, std::u16string(units.begin(), units.end()));
  literal->type = Type::string();

  return literal;
}

}
