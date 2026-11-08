#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

std::string ToUtf8::toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);

  return utf8;
}

}
