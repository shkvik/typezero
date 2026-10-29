#include "compiler/knowledge/knowledge_json/knowledge_json.h"
#include "compiler/load/source_overlay/source_overlay.h"
#include "llvm/ADT/StringMap.h"

namespace tz {

namespace {

llvm::json::Value keyOf(const Knowledge &knowledge, size_t symbol) {
  if (symbol == Knowledge::kNone) {
    return nullptr;
  }

  return knowledge.symbols[symbol].key;
}

std::string text(const llvm::json::Object &object, llvm::StringRef key) {
  return object.getString(key).value_or("").str();
}

}

llvm::json::Object KnowledgeJson::range(const SourcePlace &place) {
  return llvm::json::Object{{"line", place.line + 1},
                            {"column", place.character + 1},
                            {"endLine", place.endLine + 1},
                            {"endColumn", place.endCharacter + 1}};
}

void KnowledgeJson::readRange(const llvm::json::Object *range, SourcePlace &place) {
  if (!range) {
    return;
  }
  auto number = [&](llvm::StringRef key) {
    int64_t value = range->getInteger(key).value_or(1);

    return static_cast<unsigned>(value > 0 ? value - 1 : 0);
  };
  place.line = number("line");
  place.character = number("column");
  place.endLine = number("endLine");
  place.endCharacter = number("endColumn");
}

llvm::json::Value KnowledgeJson::write(const Knowledge &knowledge, bool libraryBodies,
                                       const std::set<std::string> *detailed) {
  std::vector<llvm::json::Array> symbols(knowledge.modules.size());
  std::vector<llvm::json::Array> references(knowledge.modules.size());
  std::vector<llvm::json::Array> types(knowledge.modules.size());
  std::vector<llvm::json::Array> locals(knowledge.modules.size());
  std::vector<llvm::json::Array> problems(knowledge.modules.size());
  for (const KnownProblem &problem : knowledge.problems) {
    if (problem.module < knowledge.modules.size()) {
      problems[problem.module].push_back(llvm::json::Object{{"severity", problem.severity},
                                                            {"message", problem.message},
                                                            {"range", range(problem.place)}});
    }
  }
  for (size_t i = 0; i < knowledge.symbols.size(); ++i) {
    const KnownSymbol &symbol = knowledge.symbols[i];
    symbols[symbol.module].push_back(llvm::json::Object{{"key", symbol.key},
                                                        {"kind", symbol.kind},
                                                        {"name", symbol.name},
                                                        {"container", symbol.container},
                                                        {"parent", keyOf(knowledge, symbol.parent)},
                                                        {"signature", symbol.signature},
                                                        {"exported", symbol.exported},
                                                        {"range", range(symbol.place)},
                                                        {"extent", range(symbol.extent)}});
  }
  auto body = [&](size_t module) { return libraryBodies || !knowledge.modules[module].library; };
  std::vector<bool> details(knowledge.modules.size(), true);
  if (detailed) {
    for (size_t m = 0; m < knowledge.modules.size(); ++m) {
      const KnownModule &module = knowledge.modules[m];
      details[m] = !module.library && detailed->count(SourceOverlay::key(module.path)) != 0;
    }
  }
  for (const KnownReference &reference : knowledge.references) {
    if (body(reference.module)) {
      references[reference.module].push_back(
          llvm::json::Object{{"to", knowledge.symbols[reference.symbol].key},
                             {"from", keyOf(knowledge, reference.from)},
                             {"call", reference.call},
                             {"range", range(reference.place)}});
    }
  }
  for (const KnownType &type : knowledge.types) {
    if (body(type.module) && details[type.module]) {
      types[type.module].push_back(llvm::json::Object{{"name", type.name},
                                                      {"type", type.type},
                                                      {"base", type.base},
                                                      {"range", range(type.place)}});
    }
  }
  for (const KnownLocal &local : knowledge.locals) {
    if (!body(local.module) || !details[local.module]) {
      continue;
    }
    llvm::json::Array uses;
    for (const SourcePlace &use : local.uses) {
      uses.push_back(range(use));
    }
    locals[local.module].push_back(llvm::json::Object{{"name", local.name},
                                                      {"type", local.type},
                                                      {"owner", keyOf(knowledge, local.owner)},
                                                      {"range", range(local.place)},
                                                      {"uses", std::move(uses)}});
  }
  llvm::json::Array modules;
  for (size_t m = 0; m < knowledge.modules.size(); ++m) {
    const KnownModule &module = knowledge.modules[m];
    llvm::json::Array specifiers;
    for (const std::string &specifier : module.specifiers) {
      specifiers.push_back(specifier);
    }
    llvm::json::Array dependsOn;
    for (size_t imported : module.imports) {
      dependsOn.push_back(knowledge.modules[imported].name);
    }
    modules.push_back(llvm::json::Object{{"path", module.path},
                                         {"file", module.file},
                                         {"name", module.name},
                                         {"hash", module.hash},
                                         {"tz", TZ_VERSION},
                                         {"library", module.library},
                                         {"entry", module.entry},
                                         {"lines", module.lines},
                                         {"imports", std::move(specifiers)},
                                         {"dependsOn", std::move(dependsOn)},
                                         {"symbols", std::move(symbols[m])},
                                         {"references", std::move(references[m])},
                                         {"types", std::move(types[m])},
                                         {"locals", std::move(locals[m])},
                                         {"problems", std::move(problems[m])}});
  }
  llvm::json::Object members;
  for (const auto &[type, list] : knowledge.typeMembers) {
    llvm::json::Array items;
    for (const KnownMember &member : list) {
      items.push_back(llvm::json::Object{
          {"name", member.name}, {"kind", member.kind}, {"detail", member.detail}});
    }
    members[type] = std::move(items);
  }

  return llvm::json::Object{{"entry", knowledge.entry},
                            {"root", knowledge.root},
                            {"modules", std::move(modules)},
                            {"members", std::move(members)}};
}

bool KnowledgeJson::read(const llvm::json::Value &value, Knowledge &out) {
  const llvm::json::Object *all = value.getAsObject();
  const llvm::json::Array *modules = all ? all->getArray("modules") : nullptr;
  if (!modules) {
    return false;
  }
  out = Knowledge();
  out.entry = text(*all, "entry");
  out.root = text(*all, "root");
  llvm::StringMap<size_t> keys;
  llvm::StringMap<size_t> names;
  std::vector<const llvm::json::Object *> items;
  for (const llvm::json::Value &item : *modules) {
    const llvm::json::Object *fields = item.getAsObject();
    if (!fields) {
      continue;
    }
    KnownModule module;
    module.path = text(*fields, "path");
    module.file = text(*fields, "file");
    module.name = text(*fields, "name");
    module.hash = text(*fields, "hash");
    module.library = fields->getBoolean("library").value_or(false);
    module.entry = fields->getBoolean("entry").value_or(false);
    module.lines = static_cast<unsigned>(fields->getInteger("lines").value_or(0));
    if (const llvm::json::Array *specifiers = fields->getArray("imports")) {
      for (const llvm::json::Value &specifier : *specifiers) {
        module.specifiers.push_back(specifier.getAsString().value_or("").str());
      }
    }
    names[module.name] = out.modules.size();
    out.modules.push_back(std::move(module));
    items.push_back(fields);
  }
  std::vector<std::pair<size_t, std::string>> parents;
  for (size_t m = 0; m < items.size(); ++m) {
    if (const llvm::json::Array *dependsOn = items[m]->getArray("dependsOn")) {
      for (const llvm::json::Value &name : *dependsOn) {
        auto found = names.find(name.getAsString().value_or(""));
        if (found != names.end()) {
          out.modules[m].imports.push_back(found->second);
        }
      }
    }
    if (const llvm::json::Array *symbols = items[m]->getArray("symbols")) {
      for (const llvm::json::Value &item : *symbols) {
        const llvm::json::Object *fields = item.getAsObject();
        if (!fields) {
          continue;
        }
        KnownSymbol symbol;
        symbol.key = text(*fields, "key");
        symbol.kind = text(*fields, "kind");
        symbol.name = text(*fields, "name");
        symbol.container = text(*fields, "container");
        symbol.signature = text(*fields, "signature");
        symbol.exported = fields->getBoolean("exported").value_or(false);
        symbol.module = m;
        symbol.place.path = out.modules[m].path;
        symbol.place.file = !out.modules[m].library;
        readRange(fields->getObject("range"), symbol.place);
        symbol.extent.path = symbol.place.path;
        symbol.extent.file = symbol.place.file;
        readRange(fields->getObject("extent"), symbol.extent);
        keys[symbol.key] = out.symbols.size();
        parents.push_back({out.symbols.size(), text(*fields, "parent")});
        out.symbols.push_back(std::move(symbol));
      }
    }
  }
  for (const auto &[symbol, parent] : parents) {
    auto found = keys.find(parent);
    if (!parent.empty() && found != keys.end()) {
      out.symbols[symbol].parent = found->second;
    }
  }
  auto symbolOf = [&](const llvm::json::Object &fields, llvm::StringRef key) {
    auto found = keys.find(fields.getString(key).value_or(""));

    return found == keys.end() ? Knowledge::kNone : found->second;
  };
  for (size_t m = 0; m < items.size(); ++m) {
    const KnownModule &module = out.modules[m];
    if (const llvm::json::Array *references = items[m]->getArray("references")) {
      for (const llvm::json::Value &item : *references) {
        const llvm::json::Object *fields = item.getAsObject();
        size_t target = fields ? symbolOf(*fields, "to") : Knowledge::kNone;
        if (target == Knowledge::kNone) {
          continue;
        }
        KnownReference reference;
        reference.symbol = target;
        reference.from = symbolOf(*fields, "from");
        reference.module = m;
        reference.call = fields->getBoolean("call").value_or(false);
        reference.place.path = module.path;
        reference.place.file = !module.library;
        readRange(fields->getObject("range"), reference.place);
        out.references.push_back(std::move(reference));
      }
    }
    if (const llvm::json::Array *types = items[m]->getArray("types")) {
      for (const llvm::json::Value &item : *types) {
        if (const llvm::json::Object *fields = item.getAsObject()) {
          KnownType type;
          type.name = text(*fields, "name");
          type.type = text(*fields, "type");
          type.base = text(*fields, "base");
          type.module = m;
          type.place.path = module.path;
          readRange(fields->getObject("range"), type.place);
          out.types.push_back(std::move(type));
        }
      }
    }
    if (const llvm::json::Array *problems = items[m]->getArray("problems")) {
      for (const llvm::json::Value &item : *problems) {
        if (const llvm::json::Object *fields = item.getAsObject()) {
          KnownProblem problem;
          problem.severity = static_cast<int>(fields->getInteger("severity").value_or(1));
          problem.message = text(*fields, "message");
          problem.module = m;
          problem.place.path = module.path;
          readRange(fields->getObject("range"), problem.place);
          out.problems.push_back(std::move(problem));
        }
      }
    }
    if (const llvm::json::Array *locals = items[m]->getArray("locals")) {
      for (const llvm::json::Value &item : *locals) {
        const llvm::json::Object *fields = item.getAsObject();
        if (!fields) {
          continue;
        }
        KnownLocal local;
        local.name = text(*fields, "name");
        local.type = text(*fields, "type");
        local.module = m;
        local.owner = symbolOf(*fields, "owner");
        local.place.path = module.path;
        readRange(fields->getObject("range"), local.place);
        if (const llvm::json::Array *uses = fields->getArray("uses")) {
          for (const llvm::json::Value &use : *uses) {
            SourcePlace place;
            place.path = module.path;
            readRange(use.getAsObject(), place);
            local.uses.push_back(place);
          }
        }
        out.locals.push_back(std::move(local));
      }
    }
  }
  if (const llvm::json::Object *members = all->getObject("members")) {
    for (const auto &[type, list] : *members) {
      const llvm::json::Array *items = list.getAsArray();
      if (!items) {
        continue;
      }
      std::vector<KnownMember> &found = out.typeMembers[type.str()];
      for (const llvm::json::Value &item : *items) {
        if (const llvm::json::Object *fields = item.getAsObject()) {
          found.push_back({text(*fields, "name"), text(*fields, "kind"), text(*fields, "detail")});
        }
      }
    }
  }

  return true;
}

}
