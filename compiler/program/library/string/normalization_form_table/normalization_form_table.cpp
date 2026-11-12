#include "compiler/program/library/string/normalization_form_table/normalization_form_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kForms[] = {"NFC", "NFD", "NFKC", "NFKD"};

}

llvm::ArrayRef<llvm::StringLiteral> NormalizationFormTable::getForms() {
  return kForms;
}

bool NormalizationFormTable::isKnown(llvm::StringRef form) {
  return llvm::is_contained(kForms, form);
}

}
