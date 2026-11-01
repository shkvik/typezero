#include "compiler/cli/error_printer/error_printer.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

ErrorPrinter::ErrorPrinter(const llvm::SourceMgr &sourceMgr, bool hints)
    : sourceMgr_(sourceMgr), hints_(hints) {}

void ErrorPrinter::print(const Errors &errors) const {
  for (const Diagnostic &diagnostic : errors) {
    if (diagnostic.kind == llvm::SourceMgr::DK_Remark) {
      if (hints_ && diagnostic.form == Diagnostic::Form::Located) {
        sourceMgr_.GetMessage(diagnostic.loc, diagnostic.kind, "hint: " + diagnostic.message)
            .print(nullptr, llvm::errs(), true, false);
      }
      continue;
    }
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

}
