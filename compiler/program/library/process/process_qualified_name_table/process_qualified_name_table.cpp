#include "compiler/program/library/process/process_qualified_name_table/process_qualified_name_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ProcessQualifiedNameInfo kNames[] = {
    {.member = ProcessMember::NEXT_TICK, .qualifiedName = "process.nextTick"},
    {.member = ProcessMember::STDIN, .qualifiedName = "process.stdin"},
    {.member = ProcessMember::STDOUT, .qualifiedName = "process.stdout"},
    {.member = ProcessMember::STDERR, .qualifiedName = "process.stderr"},
};

}

llvm::StringRef ProcessQualifiedNameTable::getQualifiedName(ProcessMember member) {
  for (const ProcessQualifiedNameInfo &info : kNames) {
    if (info.member == member) {
      return info.qualifiedName;
    }
  }
  llvm_unreachable("process member missing from the table");
}

}
