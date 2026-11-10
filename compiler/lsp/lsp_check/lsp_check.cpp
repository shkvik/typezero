#include "compiler/lsp/lsp_check/lsp_check.h"
#include "compiler/knowledge/knowledge_json/knowledge_json.h"
#include "compiler/knowledge/knowledge_pass/knowledge_pass.h"
#include "compiler/lsp/check_report/check_report.h"
#include "compiler/lsp/program_entry/program_entry.h"
#include "compiler/packages/project_packages/project_packages.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/check_program/check_program.h"
#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

LspCheck::LspCheck(const char *executable)
    : argv0(executable) {}

LspCheck::ResultResponseFile LspCheck::run(llvm::StringRef requestPath,
                                           llvm::StringRef responsePath) const {
  ResultRequestValue request = LspCheck::readRequest(requestPath);
  if (request.isErr()) {
    return err(LspCheckRunError{
        .code = LspCheckRunErrorCode::BAD_REQUEST,
        .message = request.error().message,
    });
  }
  const llvm::json::Object &object = *request.value().getAsObject();
  std::string entry = object.getString("entry")->str();
  SourceOverlay overlay = LspCheck::overlayOf(object);

  ProjectPackages packages;
  RuntimeFiles runtime(this->argv0);
  Program program;
  std::vector<Diagnostic> found;
  if (packages.prepare(entry)) {
    BuildOptions options{entry, "", true, false, packages.graph(), runtime, &overlay};
    std::optional<int64_t> level = object.getInteger("level");
    options.overflow = level.value_or(2) != 1;
    ResultCheckedProgram checked = checkProgram(program, options);
    found = checked.isOk() ? checked.value().warnings : checked.error();
  } else {
    std::string message = ErrorTextLspCheck::packages();
    found.push_back({.form = Diagnostic::Form::Tool, .message = message});
  }
  CheckReport report(program.sourceMgr, entry);
  llvm::json::Value result = report.build(found, program.file.modules);
  llvm::json::Object *answer = result.getAsObject();
  bool wantsKnowledge = object.getBoolean("knowledge").value_or(false);
  if (!program.file.modules.empty() && wantsKnowledge) {
    KnowledgePass pass(program, entry, ProgramEntry::rootOf(entry));
    Knowledge knowledge = pass.run();
    std::set<std::string> detailed = LspCheck::detailedFiles(object);
    llvm::json::Value written = KnowledgeJson::write(knowledge, false, &detailed);
    answer->try_emplace("knowledge", std::move(written));
  }

  std::error_code writeError;
  llvm::raw_fd_ostream out(responsePath, writeError);
  if (writeError) {
    std::string why = writeError.message();
    std::string message = ErrorTextLspCheck::unwritten(responsePath, why);

    return err(LspCheckRunError{
        .code = LspCheckRunErrorCode::RESPONSE_NOT_WRITTEN,
        .message = message,
    });
  }
  out << result;

  return ok(responsePath);
}

SourceOverlay LspCheck::overlayOf(const llvm::json::Object &request) {
  SourceOverlay overlay;
  const llvm::json::Array *files = request.getArray("files");
  if (files == nullptr) {
    return overlay;
  }
  for (const llvm::json::Value &file : *files) {
    const llvm::json::Object *fields = file.getAsObject();
    if (fields == nullptr) {
      continue;
    }
    std::optional<llvm::StringRef> path = fields->getString("path");
    std::optional<llvm::StringRef> text = fields->getString("text");
    if (path.has_value() && text.has_value()) {
      overlay.set(*path, *text);
    }
  }

  return overlay;
}

LspCheck::ResultRequestValue LspCheck::readRequest(llvm::StringRef requestPath) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
      llvm::MemoryBuffer::getFile(requestPath);
  if (!buffer) {
    std::string why = buffer.getError().message();
    std::string message = ErrorTextLspCheck::unread(requestPath, why);

    return err(LspCheckReadRequestError{
        .code = LspCheckReadRequestErrorCode::REQUEST_NOT_READ,
        .message = message,
    });
  }
  llvm::Expected<llvm::json::Value> request = llvm::json::parse((*buffer)->getBuffer());
  if (!request) {
    std::string why = llvm::toString(request.takeError());
    std::string message = ErrorTextLspCheck::notJson(requestPath, why);

    return err(LspCheckReadRequestError{
        .code = LspCheckReadRequestErrorCode::REQUEST_NOT_JSON,
        .message = message,
    });
  }
  const llvm::json::Object *object = request->getAsObject();
  bool hasEntry = object != nullptr && object->getString("entry").has_value();
  if (!hasEntry) {
    std::string message = ErrorTextLspCheck::noEntry(requestPath);

    return err(LspCheckReadRequestError{
        .code = LspCheckReadRequestErrorCode::NO_ENTRY,
        .message = message,
    });
  }
  llvm::json::Value value = std::move(*request);

  return ok(std::move(value));
}

std::set<std::string> LspCheck::detailedFiles(const llvm::json::Object &request) {
  std::set<std::string> detailed;
  const llvm::json::Array *files = request.getArray("files");
  if (files == nullptr) {
    return detailed;
  }
  for (const llvm::json::Value &file : *files) {
    const llvm::json::Object *fields = file.getAsObject();
    if (fields == nullptr) {
      continue;
    }
    std::optional<llvm::StringRef> path = fields->getString("path");
    if (path.has_value()) {
      std::string key = SourceOverlay::key(*path);
      detailed.insert(key);
    }
  }

  return detailed;
}

}
