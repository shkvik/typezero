#include "compiler/cli/knowledge_command/knowledge_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/error_printer/error_printer.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/cli/project_packages/project_packages.h"
#include "compiler/knowledge/knowledge_pass/knowledge_pass.h"
#include "compiler/knowledge/knowledge_question/knowledge_question.h"
#include "compiler/knowledge/knowledge_target/knowledge_target.h"
#include "compiler/lsp/program_entry/program_entry.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/pipeline/pipeline.h"
#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "compiler/program/program/program.h"
#include "compiler/program/source_places/source_places.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <vector>

namespace tz {

namespace {

namespace path = llvm::sys::path;

const char *const kKinds[] = {"definition", "references", "callers", "type", "records"};

}

KnowledgeCommand::KnowledgeCommand(const char *argv0, llvm::StringRef name)
    : argv0_(argv0), name_(name.str()) {}

bool KnowledgeCommand::learn(const std::string &entry, Knowledge &out) const {
  ProjectPackages packages;
  if (!packages.prepare(entry)) {
    return false;
  }
  RuntimeFiles runtime(argv0_);
  BuildOptions options{entry, "", true, false, packages.graph(), runtime};
  options.overflow = false;
  Program program;
  Errors errors = checkProgram(program, options);
  if (program.file.modules.empty()) {
    ErrorPrinter printer(program.sourceMgr);
    printer.print(errors);
    return false;
  }
  KnowledgePass pass(program, entry, ProgramEntry::rootOf(entry));
  out = pass.run();
  SourcePlaces places(program.sourceMgr);
  for (const Diagnostic &diagnostic : errors) {
    if (diagnostic.kind != llvm::SourceMgr::DK_Error &&
        diagnostic.kind != llvm::SourceMgr::DK_Warning) {
      continue;
    }
    std::optional<SourcePlace> place = places.find(diagnostic.loc);
    if (!place) {
      continue;
    }
    KnownProblem problem;
    problem.severity = diagnostic.kind == llvm::SourceMgr::DK_Error ? 1 : 2;
    problem.message = diagnostic.message;
    problem.module = out.moduleAt(place->path).value_or(Knowledge::kNone);
    problem.place = *place;
    out.problems.push_back(std::move(problem));
  }

  return true;
}

int KnowledgeCommand::run(llvm::ArrayRef<const char *> args) const {
  std::string entry;
  size_t budget = name_ == "map" ? 2000 : 1500;
  bool json = false;
  bool library = false;
  bool brief = false;
  std::vector<std::string> words;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (arg == "--entry" || arg == "--budget") {
      if (i + 1 == args.size()) {
        return Args::usageError("'" + arg + "' needs a value");
      }
      llvm::StringRef value = args[++i];
      if (arg == "--entry") {
        entry = value.str();
      } else if (value.getAsInteger(10, budget) || budget == 0) {
        return Args::usageError("'--budget' takes a number of tokens, e.g. '--budget 2000'");
      }
    } else if (arg == "--json") {
      json = true;
    } else if (arg == "--library" && name_ == "map") {
      library = true;
    } else if (arg == "--brief" && name_ == "query") {
      brief = true;
    } else if (arg.starts_with("-")) {
      return Args::usageError("unknown option '" + arg + "' of 'tz " + name_ + "'");
    } else {
      words.push_back(arg.str());
    }
  }
  std::string kind;
  if (name_ == "query") {
    if (words.empty() || !llvm::is_contained(kKinds, llvm::StringRef(words.front()))) {
      return Args::usageError(
          "'tz query' takes what to find - definition, references, callers, type or records - "
          "and a name or file.tz:line:column, e.g. 'tz query callers parseAge'");
    }
    kind = words.front();
    words.erase(words.begin());
  }
  bool needsTarget = name_ != "map" && kind != "records";
  if (needsTarget && words.size() != 1) {
    return Args::usageError("'tz " + name_ +
                            "' takes one name or place: 'Name', 'Class.member', 'file.tz:line' "
                            "or 'file.tz:line:column'");
  }
  if (!needsTarget && !words.empty()) {
    return Args::usageError("unexpected argument '" + words.front() + "'");
  }
  std::optional<KnowledgeTarget> target;
  if (needsTarget) {
    target.emplace(words.front());
  }
  if (entry.empty() && target && target->isPlace()) {
    if (!llvm::sys::fs::is_regular_file(target->file())) {
      llvm::errs() << "tz: there is no file '" << target->file() << "'\n";
      return kExitError;
    }
    entry = ProgramEntry::forFile(target->file());
  }
  if (entry.empty()) {
    llvm::SmallString<256> here;
    llvm::sys::fs::current_path(here);
    entry = ProgramEntry::forDirectory(here);
  }
  if (entry.empty()) {
    return Args::usageError("which program? name it with '--entry main.tz', or run in a "
                            "directory with a tz.json whose \"main\" names it");
  }
  entry = SourcePlaces::absolute(entry);

  Knowledge knowledge;
  if (!learn(entry, knowledge)) {
    return kExitError;
  }
  if (target && target->isPlace() && !knowledge.moduleAt(target->file())) {
    std::string alone = SourcePlaces::absolute(target->file());
    if (alone == entry || !learn(alone, knowledge)) {
      llvm::errs() << "tz: '" << target->file() << "' is not a module of the program\n";
      return kExitError;
    }
  }
  KnowledgeRequest request;
  request.command = name_;
  request.kind = kind;
  if (target) {
    request.target = target->text();
  }
  request.budget = budget;
  request.json = json;
  request.library = library;
  request.brief = brief;
  std::string out;
  std::string error;
  if (!KnowledgeQuestion::answer(knowledge, request, out, error)) {
    llvm::errs() << "tz: " << error << "\n";
    return kExitError;
  }
  llvm::outs() << out;

  return 0;
}

}
