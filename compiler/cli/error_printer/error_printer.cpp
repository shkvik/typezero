#include "compiler/cli/error_printer/error_printer.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

ErrorPrinter::ErrorPrinter(const llvm::SourceMgr &sourceMgr) : sourceMgr_(sourceMgr) {}

void ErrorPrinter::print(const Errors &errors) const {
  for (const Diagnostic &diagnostic : errors) {
    switch (diagnostic.form) {
    case Diagnostic::Form::Located:
      sourceMgr_.PrintMessage(llvm::errs(), diagnostic.loc, diagnostic.kind, diagnostic.message);
      break;
    case Diagnostic::Form::Tool:
      llvm::errs() << "tz: " << diagnostic.message << "\n";
      break;
    case Diagnostic::Form::Raw:
      llvm::errs() << diagnostic.message;
      break;
    }
  }
}

} // namespace tz
