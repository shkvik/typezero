#include "compiler/lsp/check_report/check_report.h"
#include "compiler/program/module/module.h"
#include <utility>

namespace tz {

namespace {

struct Item {
  SourcePlace place;
  int severity = 1;
  std::string message;
  std::vector<std::pair<SourcePlace, std::string>> related;
};

}

CheckReport::CheckReport(const llvm::SourceMgr &sourceMgr, llvm::StringRef entry)
    : places_(sourceMgr), entry_(SourcePlaces::absolute(entry)) {}

llvm::json::Object CheckReport::range(const SourcePlace &place) {
  llvm::json::Object start{{"line", place.line}, {"character", place.character}};
  llvm::json::Object end{{"line", place.endLine}, {"character", place.endCharacter}};

  return llvm::json::Object{{"start", std::move(start)}, {"end", std::move(end)}};
}

llvm::json::Value CheckReport::build(const Errors &errors,
                                     const std::vector<Module *> &modules) const {
  std::vector<Item> items;
  for (const Diagnostic &diagnostic : errors) {
    std::optional<SourcePlace> place =
        diagnostic.form == Diagnostic::Form::Located ? places_.find(diagnostic.loc) : std::nullopt;
    std::string text = llvm::StringRef(diagnostic.message).rtrim().str();
    std::string where = places_.describe(diagnostic.loc);
    std::string located = place || where.empty() ? text : where + ": " + text;
    if (diagnostic.kind == llvm::SourceMgr::DK_Note) {
      if (items.empty()) {
        continue;
      }
      if (place) {
        items.back().related.push_back({*place, text});
      } else {
        items.back().message += "\n" + located;
      }
      continue;
    }
    Item item;
    item.severity = diagnostic.kind == llvm::SourceMgr::DK_Error     ? 1
                    : diagnostic.kind == llvm::SourceMgr::DK_Warning ? 2
                                                                     : 3;
    item.message = located;
    if (place) {
      item.place = *place;
    } else {
      item.place.path = entry_;
    }
    items.push_back(std::move(item));
  }

  llvm::json::Array diagnostics;
  for (const Item &item : items) {
    llvm::json::Array related;
    for (const auto &[place, message] : item.related) {
      related.push_back(
          llvm::json::Object{{"path", place.path}, {"range", range(place)}, {"message", message}});
    }
    diagnostics.push_back(llvm::json::Object{{"path", item.place.path},
                                             {"range", range(item.place)},
                                             {"severity", item.severity},
                                             {"message", item.message},
                                             {"related", std::move(related)}});
  }
  llvm::json::Array files;
  for (const Module *module : modules) {
    if (!module->builtin && !module->path.empty()) {
      files.push_back(SourcePlaces::absolute(module->path));
    }
  }

  return llvm::json::Object{{"files", std::move(files)}, {"diagnostics", std::move(diagnostics)}};
}

}
