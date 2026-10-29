#include "compiler/lsp/lsp_check/lsp_check.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_packages/project_packages.h"
#include "compiler/knowledge/knowledge_json/knowledge_json.h"
#include "compiler/knowledge/knowledge_pass/knowledge_pass.h"
#include "compiler/load/source_overlay/source_overlay.h"
#include "compiler/lsp/check_report/check_report.h"
#include "compiler/lsp/program_entry/program_entry.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/pass_times/pass_times.h"
#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

LspCheck::LspCheck(const char *argv0) : argv0_(argv0) {}

int LspCheck::run(llvm::StringRef requestPath, llvm::StringRef responsePath) const {
  auto buffer = llvm::MemoryBuffer::getFile(requestPath);
  if (!buffer) {
    llvm::errs() << "tz: cannot read '" << requestPath << "': " << buffer.getError().message()
                 << "\n";
    return kExitError;
  }
  llvm::Expected<llvm::json::Value> request = llvm::json::parse((*buffer)->getBuffer());
  if (!request) {
    llvm::errs() << "tz: '" << requestPath
                 << "' is not JSON: " << llvm::toString(request.takeError()) << "\n";
    return kExitError;
  }
  const llvm::json::Object *object = request->getAsObject();
  std::optional<llvm::StringRef> entry = object ? object->getString("entry") : std::nullopt;
  if (!entry) {
    llvm::errs() << "tz: '" << requestPath << "' names no \"entry\"\n";
    return kExitError;
  }
  SourceOverlay overlay;
  if (const llvm::json::Array *files = object->getArray("files")) {
    for (const llvm::json::Value &file : *files) {
      const llvm::json::Object *fields = file.getAsObject();
      std::optional<llvm::StringRef> path = fields ? fields->getString("path") : std::nullopt;
      std::optional<llvm::StringRef> text = fields ? fields->getString("text") : std::nullopt;
      if (path && text) {
        overlay.set(*path, *text);
      }
    }
  }

  ProjectPackages packages;
  RuntimeFiles runtime(argv0_);
  Program program;
  Errors errors;
  PassTimes times;
  if (packages.prepare(*entry)) {
    BuildOptions options{entry->str(), "", true, false, packages.graph(), runtime, &overlay};
    options.overflow = object->getInteger("level").value_or(2) != 1;
    errors = checkProgram(program, options, &times);
  } else {
    errors.tool("the packages of tz.json are not ready; run 'tz build' to see why");
  }
  CheckReport report(program.sourceMgr, *entry);
  llvm::json::Value result = report.build(errors, program.file.modules);
  result.getAsObject()->try_emplace("times", times.text());
  if (!program.file.modules.empty() && object->getBoolean("knowledge").value_or(false)) {
    KnowledgePass pass(program, *entry, ProgramEntry::rootOf(*entry));
    Knowledge knowledge = pass.run();
    std::set<std::string> detailed;
    if (const llvm::json::Array *files = object->getArray("files")) {
      for (const llvm::json::Value &file : *files) {
        const llvm::json::Object *fields = file.getAsObject();
        if (std::optional<llvm::StringRef> path =
                fields ? fields->getString("path") : std::nullopt) {
          detailed.insert(SourceOverlay::key(*path));
        }
      }
    }
    result.getAsObject()->try_emplace("knowledge",
                                      KnowledgeJson::write(knowledge, false, &detailed));
  }

  std::error_code ec;
  llvm::raw_fd_ostream out(responsePath, ec);
  if (ec) {
    llvm::errs() << "tz: cannot write '" << responsePath << "': " << ec.message() << "\n";
    return kExitError;
  }
  out << result;

  return 0;
}

}
