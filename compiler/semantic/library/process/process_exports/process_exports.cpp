#include "compiler/semantic/library/process/process_exports/process_exports.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const ProcessMember kExportedMembers[] = {
    ProcessMember::ARGV,      ProcessMember::ENV,    ProcessMember::EXIT,
    ProcessMember::EXIT_CODE, ProcessMember::CWD,    ProcessMember::PLATFORM,
    ProcessMember::EXEC_PATH, ProcessMember::STDIN,  ProcessMember::STDOUT,
    ProcessMember::STDERR,    ProcessMember::UPTIME, ProcessMember::PID};
}

ProcessExports::ResultExportFound ProcessExports::checkProcessExport(llvm::StringRef name,
                                                                     llvm::StringRef path) {
  auto member = ProcessMemberTable::find(name);
  if (member.has_value() && llvm::is_contained(kExportedMembers, *member)) {
    return ok(name);
  }
  auto message = ErrorTextProcessExport::notYet(name, path);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NOT_YET,
      .message = message,
  });
}

}
