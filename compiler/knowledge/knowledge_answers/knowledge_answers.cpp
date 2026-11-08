#include "compiler/knowledge/knowledge_answers/knowledge_answers.h"
#include "compiler/knowledge/symbol_text/symbol_text.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <map>
#include <set>

namespace tz {

namespace {

class Budget {
public:
  explicit Budget(size_t tokens) : left_(tokens * 4) {}

  bool fits(const std::string &line) const { return line.size() + 1 <= left_; }
  bool take(std::string &out, const std::string &line) {
    if (!fits(line)) {
      return false;
    }
    left_ -= line.size() + 1;
    out += line + "\n";

    return true;
  }
  void spend(size_t chars) { left_ = chars < left_ ? left_ - chars : 0; }
  size_t left() const { return left_; }

private:
  size_t left_;
};

}

KnowledgeAnswers::KnowledgeAnswers(const Knowledge &knowledge, ProjectText &text)
    : knowledge(knowledge),
      text_(text) {}

std::string KnowledgeAnswers::joined(const std::vector<std::string> &parts) {
  std::string out;
  for (const std::string &part : parts) {
    out += (out.empty() ? "" : ", ") + part;
  }

  return out;
}

std::string KnowledgeAnswers::uses(size_t count) {
  return count == 1 ? "1 use" : std::to_string(count) + " uses";
}

std::string KnowledgeAnswers::title(size_t symbol) const {
  return this->knowledge.symbols[symbol].kind + " " + this->knowledge.qualified(symbol);
}

std::string KnowledgeAnswers::fromText(size_t from, size_t module) const {
  if (from == Knowledge::kNone) {
    return "top-level code of " + this->knowledge.modules[module].name;
  }

  return title(from);
}

llvm::json::Object KnowledgeAnswers::symbolJson(size_t symbol) const {
  const KnownSymbol &known = this->knowledge.symbols[symbol];

  return llvm::json::Object{{"key", known.key},
                            {"kind", known.kind},
                            {"name", known.name},
                            {"container", known.container},
                            {"signature", known.signature},
                            {"exported", known.exported},
                            {"file", this->knowledge.relative(known.place.path)},
                            {"line", known.place.line + 1},
                            {"column", known.place.character + 1},
                            {"uses", this->knowledge.useCount(symbol)}};
}

llvm::json::Object KnowledgeAnswers::referenceJson(size_t reference) const {
  const KnownReference &known = this->knowledge.references[reference];
  llvm::json::Value from = known.from == Knowledge::kNone
                               ? llvm::json::Value(nullptr)
                               : llvm::json::Value(this->knowledge.symbols[known.from].key);

  return llvm::json::Object{{"file", this->knowledge.relative(known.place.path)},
                            {"line", known.place.line + 1},
                            {"column", known.place.character + 1},
                            {"from", std::move(from)},
                            {"in", fromText(known.from, known.module)},
                            {"call", known.call},
                            {"text", known.line}};
}

std::vector<size_t> KnowledgeAnswers::problemsIn(size_t symbol) const {
  const KnownSymbol &known = this->knowledge.symbols[symbol];
  std::vector<size_t> found;
  for (size_t i = 0; i < this->knowledge.problems.size(); ++i) {
    const KnownProblem &problem = this->knowledge.problems[i];
    if (problem.module == known.module && problem.place.line >= known.extent.line &&
        problem.place.line <= known.extent.endLine) {
      found.push_back(i);
    }
  }

  return found;
}

std::string KnowledgeAnswers::map(size_t tokens, bool library) const {
  std::string out;
  Budget budget(tokens);
  std::vector<size_t> shown;
  std::vector<std::string> builtins;
  unsigned lines = 0;
  for (size_t m = 0; m < this->knowledge.modules.size(); ++m) {
    const KnownModule &module = this->knowledge.modules[m];
    if (module.library) {
      builtins.push_back(module.name);
    }
    if (!module.library || library) {
      shown.push_back(m);
      lines += module.lines;
    }
  }
  std::string header = "program " + this->knowledge.relative(this->knowledge.entry) + ": " +
                       std::to_string(shown.size()) + " modules, " + std::to_string(lines) +
                       " lines";
  if (!this->knowledge.problems.empty()) {
    header += ", " + std::to_string(this->knowledge.problems.size()) + " problems";
  }
  budget.take(out, header);
  if (!builtins.empty() && !library) {
    budget.take(out, "built-in modules: " + KnowledgeAnswers::joined(builtins));
  }

  std::vector<size_t> problems(this->knowledge.modules.size(), 0);
  for (const KnownProblem &problem : this->knowledge.problems) {
    if (problem.module < problems.size()) {
      ++problems[problem.module];
    }
  }
  std::vector<double> score(this->knowledge.modules.size(), 0);
  for (const KnownReference &reference : this->knowledge.references) {
    size_t target = this->knowledge.symbols[reference.symbol].module;
    if (target != reference.module) {
      score[target] += 1;
    }
  }
  for (size_t m : shown) {
    score[m] += static_cast<double>(this->knowledge.importersOf(m).size()) * 10;
    if (this->knowledge.modules[m].entry) {
      score[m] = 1e18;
    }
  }
  std::stable_sort(shown.begin(), shown.end(),
                   [&](size_t a, size_t b) { return score[a] > score[b]; });

  auto head = [&](size_t m) {
    const KnownModule &module = this->knowledge.modules[m];
    std::string text = "\n" + module.name + " - " + std::to_string(module.lines) + " lines";
    if (!module.specifiers.empty()) {
      std::vector<std::string> some(module.specifiers.begin(),
                                    module.specifiers.begin() +
                                        std::min<size_t>(module.specifiers.size(), 6));
      text += "; imports " + KnowledgeAnswers::joined(some);
      if (module.specifiers.size() > some.size()) {
        text += " and " + std::to_string(module.specifiers.size() - some.size()) + " more";
      }
    }
    std::vector<size_t> importers = this->knowledge.importersOf(m);
    if (!importers.empty()) {
      std::vector<std::string> some;
      for (size_t importer : llvm::ArrayRef(importers).take_front(6)) {
        some.push_back(this->knowledge.modules[importer].name);
      }
      text += "; imported by " + KnowledgeAnswers::joined(some);
      if (importers.size() > some.size()) {
        text += " and " + std::to_string(importers.size() - some.size()) + " more";
      }
    }
    if (problems[m] != 0) {
      text += "; " + std::to_string(problems[m]) + " problems";
    }

    return text;
  };
  std::vector<size_t> listed;
  size_t headRoom = budget.left() * 2 / 5;
  size_t headCost = 0;
  for (size_t m : shown) {
    std::string text = head(m);
    if (!listed.empty() && headCost + text.size() + 1 > headRoom) {
      break;
    }
    headCost += text.size() + 1;
    listed.push_back(m);
  }
  budget.spend(headCost);

  auto line = [&](size_t symbol) {
    const KnownSymbol &known = this->knowledge.symbols[symbol];
    std::string text = known.parent == Knowledge::kNone ? "  " : "    ";
    if (known.exported && known.parent == Knowledge::kNone &&
        !llvm::StringRef(known.signature).starts_with("export ")) {
      text += "export ";
    }
    text += known.signature.empty() ? title(symbol) : known.signature;

    return text + "  - " + KnowledgeAnswers::uses(this->knowledge.useCount(symbol)) + ", line " +
           std::to_string(known.place.line + 1);
  };
  std::vector<std::pair<double, size_t>> candidates;
  for (size_t m : listed) {
    for (size_t i : this->knowledge.symbolsOf(m)) {
      const KnownSymbol &known = this->knowledge.symbols[i];
      double priority = static_cast<double>(this->knowledge.useCount(i)) * 10;
      if (known.parent == Knowledge::kNone) {
        priority += (known.exported ? 1e6 : 0) + (known.kind == "class" ? 1 : 0);
      } else {
        priority += (known.exported ? 5e5 : 0) - 1;
      }
      candidates.push_back({priority, i});
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const auto &a, const auto &b) { return a.first > b.first; });
  std::set<size_t> chosen;
  for (const auto &[priority, symbol] : candidates) {
    size_t parent = this->knowledge.symbols[symbol].parent;
    if (parent != Knowledge::kNone && chosen.count(parent) == 0) {
      continue;
    }
    std::string text = line(symbol);
    if (budget.fits(text)) {
      budget.spend(text.size() + 1);
      chosen.insert(symbol);
    }
  }

  for (size_t m : listed) {
    out += head(m) + "\n";
    size_t omitted = 0;
    for (size_t i : this->knowledge.symbolsOf(m)) {
      if (this->knowledge.symbols[i].parent != Knowledge::kNone) {
        continue;
      }
      if (chosen.count(i) == 0) {
        omitted += 1 + this->knowledge.members(i).size();
        continue;
      }
      out += line(i) + "\n";
      for (size_t member : this->knowledge.members(i)) {
        if (chosen.count(member) != 0) {
          out += line(member) + "\n";
        } else {
          ++omitted;
        }
      }
    }
    if (omitted != 0) {
      out += "  ... " + std::to_string(omitted) + " more (raise --budget)\n";
    }
  }
  if (listed.size() < shown.size()) {
    std::vector<std::string> rest;
    for (size_t m : llvm::ArrayRef(shown).drop_front(listed.size()).take_front(8)) {
      rest.push_back(this->knowledge.modules[m].name);
    }
    out += "\n... " + std::to_string(shown.size() - listed.size()) + " more modules (" +
           KnowledgeAnswers::joined(rest) +
           (shown.size() - listed.size() > rest.size() ? ", ..." : "") +
           "); raise --budget, or ask for one with context\n";
  }

  return out;
}

llvm::json::Value KnowledgeAnswers::mapJson(bool library) const {
  llvm::json::Array modules;
  for (size_t m = 0; m < this->knowledge.modules.size(); ++m) {
    const KnownModule &module = this->knowledge.modules[m];
    if (module.library && !library) {
      continue;
    }
    llvm::json::Array imports;
    for (const std::string &specifier : module.specifiers) {
      imports.push_back(specifier);
    }
    llvm::json::Array importers;
    for (size_t importer : this->knowledge.importersOf(m)) {
      importers.push_back(this->knowledge.modules[importer].name);
    }
    llvm::json::Array symbols;
    for (size_t i : this->knowledge.symbolsOf(m)) {
      if (this->knowledge.symbols[i].parent != Knowledge::kNone) {
        continue;
      }
      llvm::json::Object item = symbolJson(i);
      llvm::json::Array members;
      for (size_t member : this->knowledge.members(i)) {
        members.push_back(symbolJson(member));
      }
      if (!members.empty()) {
        item["members"] = std::move(members);
      }
      symbols.push_back(std::move(item));
    }
    size_t problems = llvm::count_if(
        this->knowledge.problems, [&](const KnownProblem &problem) { return problem.module == m; });
    modules.push_back(llvm::json::Object{{"name", module.name},
                                         {"path", module.path},
                                         {"lines", module.lines},
                                         {"library", module.library},
                                         {"imports", std::move(imports)},
                                         {"importedBy", std::move(importers)},
                                         {"problems", problems},
                                         {"symbols", std::move(symbols)}});
  }

  return llvm::json::Object{{"entry", this->knowledge.relative(this->knowledge.entry)},
                            {"root", this->knowledge.root},
                            {"modules", std::move(modules)}};
}

std::string KnowledgeAnswers::context(const std::vector<size_t> &symbols, size_t tokens) {
  std::string out;
  Budget budget(tokens);
  if (symbols.size() > 1) {
    budget.take(out, std::to_string(symbols.size()) + " symbols match; each follows");
  }
  for (size_t s : llvm::ArrayRef(symbols).take_front(5)) {
    const KnownSymbol &known = this->knowledge.symbols[s];
    std::string head = "\n## " + title(s) + " - " + this->knowledge.where(known.place);
    if (known.exported) {
      head += " (exported)";
    }
    if (!budget.take(out, head)) {
      break;
    }
    if (!known.signature.empty()) {
      budget.take(out, known.signature);
    }
    std::vector<size_t> problems = problemsIn(s);
    if (!problems.empty()) {
      budget.take(out, "problems here:");
      for (size_t p : problems) {
        const KnownProblem &problem = this->knowledge.problems[p];
        budget.take(out, "  " + this->knowledge.where(problem.place) +
                             (problem.severity == 1 ? " error: " : " warning: ") +
                             problem.message.substr(0, problem.message.find('\n')));
      }
    }
    std::string source = this->knowledge.sourceOf(s);
    if (known.kind != "field" && known.kind != "const" && known.kind != "let" && !source.empty()) {
      llvm::SmallVector<llvm::StringRef, 0> rows;
      llvm::StringRef(source).split(rows, '\n');
      size_t indent = llvm::StringRef::npos;
      for (llvm::StringRef row : rows) {
        if (!row.trim().empty()) {
          indent = std::min(indent, row.size() - row.ltrim(" \t").size());
        }
      }
      for (llvm::StringRef &row : rows) {
        row = row.drop_front(std::min(indent, row.size() - row.ltrim(" \t").size()));
      }
      size_t room = budget.left() / 2;
      std::string block;
      size_t shownRows = 0;
      for (llvm::StringRef row : rows) {
        std::string number = "  " + std::to_string(known.extent.line + 1 + shownRows) + " |";
        std::string text = row.trim().empty() ? number : number + " " + row.rtrim().str();
        if (block.size() + text.size() + 1 > room) {
          break;
        }
        block += text + "\n";
        ++shownRows;
      }
      budget.take(out, "source:");
      budget.spend(block.size());
      out += block;
      if (shownRows < rows.size()) {
        budget.take(out, "  ... " + std::to_string(rows.size() - shownRows) + " lines more");
      }
    }
    std::vector<size_t> members = this->knowledge.members(s);
    if (!members.empty()) {
      budget.take(out, "members:");
      for (size_t member : members) {
        const KnownSymbol &part = this->knowledge.symbols[member];
        if (!budget.take(out, "  " + part.signature + "  - " +
                                  KnowledgeAnswers::uses(this->knowledge.useCount(member)) +
                                  ", line " + std::to_string(part.place.line + 1))) {
          break;
        }
      }
    }
    std::vector<size_t> used = this->knowledge.usesOf(s);
    for (size_t member : members) {
      for (size_t inner : this->knowledge.usesOf(member)) {
        if (inner != s && !llvm::is_contained(members, inner) && !llvm::is_contained(used, inner)) {
          used.push_back(inner);
        }
      }
    }
    if (!used.empty()) {
      budget.take(out, "uses (" + std::to_string(used.size()) + "):");
      for (size_t inner : used) {
        const KnownSymbol &part = this->knowledge.symbols[inner];
        std::string text = "  " + title(inner) + " - " + this->knowledge.where(part.place);
        if (!part.signature.empty() && part.signature != title(inner)) {
          text += " - " + part.signature;
        }
        if (!budget.take(out, text)) {
          break;
        }
      }
    }
    std::vector<size_t> references = this->knowledge.referencesTo(s);
    if (!references.empty()) {
      budget.take(out, "used by (" + std::to_string(references.size()) + "):");
      size_t listed = 0;
      for (size_t reference : references) {
        const KnownReference &referrer = this->knowledge.references[reference];
        if (!budget.take(out, "  " + this->knowledge.where(referrer.place) + " in " +
                                  fromText(referrer.from, referrer.module) + " - " +
                                  referrer.line)) {
          break;
        }
        ++listed;
      }
      if (listed < references.size()) {
        budget.take(out, "  ... " + std::to_string(references.size() - listed) + " more");
      }
    }
    std::vector<TextMention> docs = text_.docs(known.name, this->knowledge.qualified(s), 5);
    if (!docs.empty()) {
      budget.take(out, "docs:");
      for (const TextMention &mention : docs) {
        std::string file = mention.file;
        llvm::StringRef rest = file;
        if (rest.consume_front(text_.root())) {
          file = llvm::sys::path::convert_to_slash(rest.ltrim("/\\"));
        }
        budget.take(out, "  " + file + ":" + std::to_string(mention.line) + " (" + mention.heading +
                             ") - " + SymbolText::collapse(mention.text, 160));
      }
    }
    std::vector<std::string> tests = text_.tests(known.name, 5);
    if (!tests.empty()) {
      std::vector<std::string> names;
      for (const std::string &file : tests) {
        llvm::StringRef rest = file;
        rest.consume_front(text_.root());
        names.push_back(llvm::sys::path::convert_to_slash(rest.ltrim("/\\")));
      }
      budget.take(out, "tests that mention '" + known.name +
                           "' (by text): " + KnowledgeAnswers::joined(names));
    }
  }

  return out;
}

llvm::json::Value KnowledgeAnswers::contextJson(const std::vector<size_t> &symbols) {
  llvm::json::Array all;
  for (size_t s : symbols) {
    const KnownSymbol &known = this->knowledge.symbols[s];
    llvm::json::Array problems;
    for (size_t p : problemsIn(s)) {
      const KnownProblem &problem = this->knowledge.problems[p];
      problems.push_back(
          llvm::json::Object{{"file", this->knowledge.relative(problem.place.path)},
                             {"line", problem.place.line + 1},
                             {"column", problem.place.character + 1},
                             {"severity", problem.severity == 1 ? "error" : "warning"},
                             {"message", problem.message}});
    }
    llvm::json::Array members;
    for (size_t member : this->knowledge.members(s)) {
      members.push_back(symbolJson(member));
    }
    llvm::json::Array used;
    for (size_t inner : this->knowledge.usesOf(s)) {
      used.push_back(symbolJson(inner));
    }
    llvm::json::Array usedBy;
    for (size_t reference : this->knowledge.referencesTo(s)) {
      usedBy.push_back(referenceJson(reference));
    }
    llvm::json::Array docs;
    for (const TextMention &mention : text_.docs(known.name, this->knowledge.qualified(s), 5)) {
      docs.push_back(llvm::json::Object{{"file", mention.file},
                                        {"line", mention.line},
                                        {"heading", mention.heading},
                                        {"text", mention.text}});
    }
    llvm::json::Array tests;
    for (const std::string &file : text_.tests(known.name, 5)) {
      tests.push_back(file);
    }
    all.push_back(llvm::json::Object{{"symbol", symbolJson(s)},
                                     {"source", this->knowledge.sourceOf(s)},
                                     {"problems", std::move(problems)},
                                     {"members", std::move(members)},
                                     {"uses", std::move(used)},
                                     {"usedBy", std::move(usedBy)},
                                     {"docs", std::move(docs)},
                                     {"testsByText", std::move(tests)}});
  }

  return all;
}

std::vector<size_t> KnowledgeAnswers::callers(size_t symbol) const {
  std::vector<size_t> targets = {symbol};
  for (size_t member : this->knowledge.members(symbol)) {
    if (this->knowledge.symbols[member].kind == "constructor") {
      targets.push_back(member);
    }
  }
  std::vector<size_t> found;
  for (size_t target : targets) {
    for (size_t reference : this->knowledge.referencesTo(target)) {
      if (this->knowledge.references[reference].call) {
        found.push_back(reference);
      }
    }
  }

  return found;
}

std::string KnowledgeAnswers::local(size_t local, llvm::StringRef kind) const {
  const KnownLocal &known = this->knowledge.locals[local];
  std::string owner = known.owner == Knowledge::kNone
                          ? "top-level code of " + this->knowledge.modules[known.module].name
                          : title(known.owner);
  if (kind == "type") {
    return known.name + ": " + known.type + "\n";
  }
  if (kind == "definition") {
    return this->knowledge.where(known.place) + "  " + known.name + ": " + known.type +
           " (local of " + owner + ")\n";
  }
  std::string out = "local " + known.name + ": " + known.type + " - " +
                    this->knowledge.where(known.place) + " in " + owner + "\n";
  out += std::to_string(known.uses.size()) + (known.uses.size() == 1 ? " use" : " uses") + ":\n";
  for (const SourcePlace &use : known.uses) {
    out += "  " + this->knowledge.where(use) + "\n";
  }

  return out;
}

std::string KnowledgeAnswers::query(llvm::StringRef kind, const KnowledgeTarget &target,
                                    const std::vector<size_t> &symbols) const {
  std::string out;
  if (size_t found = target.localAt(this->knowledge);
      found != Knowledge::kNone && symbols.empty()) {
    return local(found, kind);
  }
  if (kind == "type" && target.isPlace()) {
    if (const KnownType *type = target.typeAt(this->knowledge)) {
      return type->name + ": " + type->type + "\n";
    }
  }
  if (symbols.empty()) {
    return "nothing in the program is named '" + target.text() + "'\n";
  }
  for (size_t s : symbols) {
    const KnownSymbol &known = this->knowledge.symbols[s];
    if (kind == "definition") {
      out += this->knowledge.where(known.place) + "  " + known.signature + "\n";
    } else if (kind == "type") {
      out += (known.signature.empty() ? title(s) : known.signature) + "\n";
    } else if (kind == "references") {
      std::vector<size_t> references = this->knowledge.referencesTo(s);
      out += title(s) + " - " + std::to_string(references.size()) + " references\n";
      for (size_t reference : references) {
        const KnownReference &ref = this->knowledge.references[reference];
        out += "  " + this->knowledge.where(ref.place) + " in " + fromText(ref.from, ref.module) +
               " - " + ref.line + "\n";
      }
    } else {
      std::vector<size_t> calls = callers(s);
      out += title(s) + " - called from " + std::to_string(calls.size()) +
             (calls.size() == 1 ? " place\n" : " places\n");
      for (size_t reference : calls) {
        const KnownReference &ref = this->knowledge.references[reference];
        out +=
            "  " + fromText(ref.from, ref.module) + " - " + this->knowledge.where(ref.place) + "\n";
      }
    }
  }

  return out;
}

llvm::json::Value KnowledgeAnswers::queryJson(llvm::StringRef kind, const KnowledgeTarget &target,
                                              const std::vector<size_t> &symbols) const {
  if (kind == "type" && target.isPlace()) {
    if (const KnownType *type = target.typeAt(this->knowledge)) {
      return llvm::json::Object{{"name", type->name}, {"type", type->type}};
    }
  }
  llvm::json::Array all;
  for (size_t s : symbols) {
    llvm::json::Object item{{"symbol", symbolJson(s)}};
    if (kind == "references" || kind == "callers") {
      llvm::json::Array places;
      for (size_t reference : kind == "references" ? this->knowledge.referencesTo(s) : callers(s)) {
        places.push_back(referenceJson(reference));
      }
      item[kind] = std::move(places);
    }
    all.push_back(std::move(item));
  }

  return all;
}

std::vector<std::vector<size_t>>
KnowledgeAnswers::impactLevels(const std::vector<size_t> &symbols,
                               std::vector<size_t> &modules) const {
  std::set<size_t> seen;
  std::vector<size_t> frontier;
  for (size_t s : symbols) {
    if (seen.insert(s).second) {
      frontier.push_back(s);
    }
    for (size_t member : this->knowledge.members(s)) {
      if (seen.insert(member).second) {
        frontier.push_back(member);
      }
    }
  }
  for (size_t s : frontier) {
    if (!llvm::is_contained(modules, this->knowledge.symbols[s].module)) {
      modules.push_back(this->knowledge.symbols[s].module);
    }
  }
  std::vector<std::vector<size_t>> levels;
  while (!frontier.empty() && levels.size() < 10) {
    std::vector<size_t> next;
    for (size_t s : frontier) {
      for (size_t reference : this->knowledge.referencesTo(s)) {
        const KnownReference &ref = this->knowledge.references[reference];
        if (!llvm::is_contained(modules, ref.module)) {
          modules.push_back(ref.module);
        }
        if (ref.from != Knowledge::kNone && seen.insert(ref.from).second) {
          next.push_back(ref.from);
        }
      }
    }
    if (!next.empty()) {
      levels.push_back(next);
    }
    frontier = std::move(next);
  }

  return levels;
}

std::string KnowledgeAnswers::impact(const std::vector<size_t> &symbols, size_t tokens) const {
  std::string out;
  Budget budget(tokens);
  std::vector<size_t> modules;
  std::vector<std::vector<size_t>> levels = impactLevels(symbols, modules);
  std::vector<std::string> targets;
  for (size_t s : symbols) {
    targets.push_back(title(s) + " (" + this->knowledge.where(this->knowledge.symbols[s].place) +
                      ")");
  }
  size_t total = 0;
  for (const std::vector<size_t> &level : levels) {
    total += level.size();
  }
  budget.take(out, "changing " + KnowledgeAnswers::joined(targets) + " can affect " +
                       std::to_string(total) + " symbols in " + std::to_string(modules.size()) +
                       " modules");
  std::vector<std::string> exported;
  for (size_t i = 0; i < levels.size(); ++i) {
    budget.take(out, "level " + std::to_string(i + 1) +
                         (i == 0 ? " - use it directly (" : " - use those above (") +
                         std::to_string(levels[i].size()) + "):");
    size_t listed = 0;
    for (size_t s : levels[i]) {
      const KnownSymbol &known = this->knowledge.symbols[s];
      if (known.exported) {
        exported.push_back(title(s));
      }
      if (budget.take(out, "  " + title(s) + " - " + this->knowledge.where(known.place))) {
        ++listed;
      }
    }
    if (listed < levels[i].size()) {
      out += "  ... " + std::to_string(levels[i].size() - listed) + " more\n";
    }
  }
  std::vector<std::string> names;
  for (size_t m : modules) {
    names.push_back(this->knowledge.modules[m].name);
  }
  budget.take(out, "modules: " + KnowledgeAnswers::joined(names));
  if (!exported.empty()) {
    budget.take(out, "exported among them: " + KnowledgeAnswers::joined(exported));
  }

  return out;
}

llvm::json::Value KnowledgeAnswers::impactJson(const std::vector<size_t> &symbols) const {
  std::vector<size_t> modules;
  std::vector<std::vector<size_t>> levels = impactLevels(symbols, modules);
  llvm::json::Array targets;
  for (size_t s : symbols) {
    targets.push_back(symbolJson(s));
  }
  llvm::json::Array rows;
  for (const std::vector<size_t> &level : levels) {
    llvm::json::Array row;
    for (size_t s : level) {
      row.push_back(symbolJson(s));
    }
    rows.push_back(std::move(row));
  }
  llvm::json::Array names;
  for (size_t m : modules) {
    names.push_back(this->knowledge.modules[m].name);
  }

  return llvm::json::Object{
      {"targets", std::move(targets)}, {"levels", std::move(rows)}, {"modules", std::move(names)}};
}

}
