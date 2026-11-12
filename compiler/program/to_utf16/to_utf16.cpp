#include "compiler/program/to_utf16/to_utf16.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

std::u16string ToUtf16::toUtf16(llvm::StringRef text) {
  auto units = llvm::SmallVector<llvm::UTF16, 32>();
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

}
