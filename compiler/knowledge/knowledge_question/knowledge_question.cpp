#include "compiler/knowledge/knowledge_question/knowledge_question.h"
#include "compiler/knowledge/knowledge_answers/knowledge_answers.h"
#include "compiler/knowledge/knowledge_json/knowledge_json.h"
#include "compiler/knowledge/knowledge_target/knowledge_target.h"
#include "compiler/knowledge/project_text/project_text.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

namespace {

std::string printed(const llvm::json::Value &value, bool pretty) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  if (pretty) {
    stream << llvm::formatv("{0:2}", value);
  } else {
    stream << value;
  }

  return text + "\n";
}

}

bool KnowledgeQuestion::answer(const Knowledge &knowledge, const KnowledgeRequest &request,
                               std::string &out, std::string &error) {
  if (request.kind == "records") {
    std::set<std::string> none;
    out = printed(KnowledgeJson::write(knowledge, true, request.brief ? &none : nullptr),
                  !request.json);
    return true;
  }
  ProjectText text(knowledge.root);
  KnowledgeAnswers answers(knowledge, text);
  std::optional<KnowledgeTarget> target;
  std::vector<size_t> symbols;
  size_t local = Knowledge::kNone;
  if (request.target) {
    target.emplace(*request.target);
    symbols = target->resolve(knowledge);
    local = target->localAt(knowledge);
    bool typed = (request.kind == "type" && target->typeAt(knowledge)) || local != Knowledge::kNone;
    if (symbols.empty() && !typed && target->isPlace()) {
      error = "nothing is declared or used at " + target->text();
      return false;
    }
    if (symbols.empty() && !typed) {
      error = "nothing in the program is named '" + target->text() +
              "'; try 'Name', 'Class.member' or 'file.tz:line:column'";
      return false;
    }
  }
  const std::string &command = request.command;
  if (target && symbols.empty() && local != Knowledge::kNone && !request.json &&
      command != "query") {
    out = answers.local(local, "references");
  } else if (command == "map") {
    out = request.json ? printed(answers.mapJson(request.library), true)
                       : answers.map(request.budget, request.library);
  } else if (command == "context") {
    out = request.json ? printed(answers.contextJson(symbols), true)
                       : answers.context(symbols, request.budget);
  } else if (command == "impact") {
    out = request.json ? printed(answers.impactJson(symbols), true)
                       : answers.impact(symbols, request.budget);
  } else if (request.json) {
    out = printed(answers.queryJson(request.kind, *target, symbols), true);
  } else {
    out = answers.query(request.kind, *target, symbols);
  }

  return true;
}

}
