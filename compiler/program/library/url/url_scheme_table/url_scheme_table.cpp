#include "compiler/program/library/url/url_scheme_table/url_scheme_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kSpecialSchemes[] = {"http", "https", "ws", "wss", "ftp"};

}

bool UrlSchemeTable::isSpecial(llvm::StringRef scheme) {
  return llvm::is_contained(kSpecialSchemes, scheme);
}

}
