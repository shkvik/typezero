#include "compiler/semantic/types_v2/text_patterns/types_text_patterns_registry.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/string/simple_case_table/simple_case_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

namespace {

constexpr size_t kMaxTemplateStrings = 100000;

}

TypesTextPatternsRegistry::TypesTextPatternsRegistry(SemanticState &owner)
    : state(owner) {}

bool TypesTextPatternsRegistry::isTemplateHole(const Type *type) const {
  return type->isString() || type->isNumeric() || type->isBool() || type->isLiteral() ||
         type->isUndefined() || type->isParam();
}

std::u16string TypesTextPatternsRegistry::unitInCase(TypeOp op, char16_t unit, bool isFirst) const {
  bool isUpper = op == TypeOp::Uppercase || (op == TypeOp::Capitalize && isFirst);
  bool isLower = op == TypeOp::Lowercase || (op == TypeOp::Uncapitalize && isFirst);
  if (isUpper) {
    return SimpleCaseTable::upperOf(unit);
  }
  if (isLower) {
    return SimpleCaseTable::lowerOf(unit);
  }

  return std::u16string(1, unit);
}

std::u16string TypesTextPatternsRegistry::changeCase(TypeOp op, const std::u16string &text) const {
  auto changed = std::u16string();
  for (size_t index = 0; index < text.size(); ++index) {
    bool isFirst = index == 0;
    changed += this->unitInCase(op, text[index], isFirst);
  }

  return changed;
}

const Type *TypesTextPatternsRegistry::firstNonHole(const Type *hole) const {
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(hole)) {
    if (!this->isTemplateHole(member)) {
      return member;
    }
  }

  return nullptr;
}

TypesTextPatternsRegistry::ResultHoleAccepted
TypesTextPatternsRegistry::checkHole(const Type *hole) const {
  const Type *rejected = this->firstNonHole(hole);
  if (rejected != nullptr) {
    auto message = ErrorTextHoleType::notHole(rejected->name());

    return err(TypesTextPatternsRegistryHoleTypeError{
        .code = TypesTextPatternsRegistryHoleTypeErrorCode::NOT_A_HOLE,
        .message = message,
    });
  }

  return ok(hole);
}

TypesTextPatternsRegistry::ResultCountAllowed
TypesTextPatternsRegistry::checkStringCount(size_t count) const {
  if (count > kMaxTemplateStrings) {
    auto message = ErrorTextStringCount::tooMany(kMaxTemplateStrings);

    return err(TypesTextPatternsRegistryStringCountError{
        .code = TypesTextPatternsRegistryStringCountErrorCode::TOO_MANY_STRINGS,
        .message = message,
    });
  }

  return ok(count);
}

void TypesTextPatternsRegistry::startTemplate(const StartTemplate &task) {
  auto draft = TemplateDraft{
      .ref = task.ref,
      .next = 0,
      .operands = {},
  };

  return this->nextTemplateHole(draft);
}

void TypesTextPatternsRegistry::nextTemplateHole(TemplateDraft draft) {
  const TypeRef &ref = *draft.ref;
  if (draft.next == ref.texts().size()) {
    return this->finishTemplateHoles(draft);
  }
  const auto *text = Type::stringLiteral(ref.texts()[draft.next]);
  draft.operands.push_back(text);
  if (draft.next >= ref.args.size()) {
    return this->finishTemplateHoles(draft);
  }
  this->state.walk.tasks.pushTask(AfterTemplateHole{.draft = draft});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[draft.next], .use = TypeUse::INNER});
}

void TypesTextPatternsRegistry::afterTemplateHole(const AfterTemplateHole &task) {
  TemplateDraft draft = task.draft;
  const TypeRef &arg = draft.ref->args[draft.next];
  const Type *hole = this->state.walk.tasks.takeAnswer();
  if (hole->isError()) {
    return this->state.walk.tasks.pushAnswer(hole);
  }
  auto accepted = this->checkHole(hole);
  if (accepted.isErr()) {
    this->state.diagnostics.error(arg.loc, accepted.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  draft.operands.push_back(hole);
  draft.next += 1;

  return this->nextTemplateHole(draft);
}

void TypesTextPatternsRegistry::finishTemplateHoles(const TemplateDraft &draft) {
  this->state.types.context.rememberTemplateLoc(draft.ref->loc);
  const auto *unevaluated = Type::typeOp(TypeOp::Template, draft.operands);
  this->state.walk.tasks.pushTask(FinishTemplate{});
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = unevaluated, .operands = draft.operands});
}

void TypesTextPatternsRegistry::finishTemplate() {
  this->state.types.context.forgetTemplateLoc();
}

void TypesTextPatternsRegistry::evalTemplate(const EvalTemplate &task) {
  llvm::ArrayRef<const Type *> operands = task.operands;
  auto first = TemplatePart{.texts = {operands[0]->literalText()}, .holes = {}};
  auto parts = std::vector<TemplatePart>{first};
  for (size_t index = 1; index + 1 < operands.size(); index += 2) {
    auto members = TypeopsCheckHelpers::distributedMembers(operands[index]);
    const std::u16string &after = operands[index + 1]->literalText();
    auto grown = this->grownParts(parts, members, after);
    if (!grown.has_value()) {
      return this->state.walk.tasks.pushAnswer(Type::error());
    }
    parts = std::move(*grown);
  }
  auto results = std::vector<const Type *>();
  for (const TemplatePart &part : parts) {
    results.push_back(this->partType(part));
  }
  const Type *joined = results.empty() ? Type::never() : Type::unionOf(results);
  this->state.walk.tasks.pushAnswer(joined);
}

std::optional<std::vector<TypesTextPatternsRegistry::TemplatePart>>
TypesTextPatternsRegistry::grownParts(const std::vector<TemplatePart> &parts,
                                      llvm::ArrayRef<const Type *> members,
                                      const std::u16string &after) {
  auto grown = std::vector<TemplatePart>();
  for (const TemplatePart &part : parts) {
    for (const Type *member : members) {
      auto made = this->partWithMember(part, member);
      if (!made.has_value()) {
        return std::nullopt;
      }
      made->texts.back() += after;
      grown.push_back(std::move(*made));
      auto count = this->checkStringCount(grown.size());
      if (count.isErr()) {
        this->reportTooManyStrings(count.error().message);

        return std::nullopt;
      }
    }
  }

  return grown;
}

std::optional<TypesTextPatternsRegistry::TemplatePart>
TypesTextPatternsRegistry::partWithMember(const TemplatePart &part, const Type *member) const {
  TemplatePart made = part;
  if (member->isStringLiteral()) {
    made.texts.back() += member->literalText();

    return made;
  }
  if (member->isLiteral()) {
    made.texts.back() += ToUtf16::toUtf16(member->name());

    return made;
  }
  if (member->isUndefined()) {
    made.texts.back() += ToUtf16::toUtf16(TypeNameTable::getName(TypeName::UNDEFINED));

    return made;
  }
  if (member->isPattern()) {
    this->appendPattern(made, member);

    return made;
  }
  if (member->isString() || member->isNumeric()) {
    made.holes.push_back(member);
    made.texts.emplace_back();

    return made;
  }

  return std::nullopt;
}

void TypesTextPatternsRegistry::appendPattern(TemplatePart &part, const Type *pattern) const {
  part.texts.back() += pattern->patternTexts()[0];
  for (size_t index = 0; index < pattern->patternHoles().size(); ++index) {
    part.holes.push_back(pattern->patternHoles()[index]);
    part.texts.push_back(pattern->patternTexts()[index + 1]);
  }
}

void TypesTextPatternsRegistry::reportTooManyStrings(llvm::StringRef message) {
  auto templateLoc = this->state.types.context.getTemplateLoc();
  auto instanceLoc = this->state.generics.copies.getInstanceLoc();
  llvm::SMLoc loc = templateLoc.isValid() ? templateLoc : instanceLoc;
  if (loc.isValid()) {
    this->state.diagnostics.error(loc, message);
  }
}

const Type *TypesTextPatternsRegistry::partType(const TemplatePart &part) const {
  if (part.holes.empty()) {
    return Type::stringLiteral(part.texts[0]);
  }

  return Type::stringPattern(part.texts, part.holes);
}

void TypesTextPatternsRegistry::evalStringCase(const EvalStringCase &task) {
  auto results = std::vector<const Type *>();
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(task.operand)) {
    const Type *changed = this->memberInCase(task.kind, member);
    if (changed == nullptr) {
      return this->state.walk.tasks.pushAnswer(Type::error());
    }
    results.push_back(changed);
  }
  const Type *joined = results.empty() ? Type::never() : Type::unionOf(results);
  this->state.walk.tasks.pushAnswer(joined);
}

const Type *TypesTextPatternsRegistry::memberInCase(TypeOp op, const Type *member) const {
  if (member->isStringLiteral()) {
    auto text = this->changeCase(op, member->literalText());

    return Type::stringLiteral(text);
  }
  if (member->isPattern()) {
    return this->patternInCase(op, member);
  }
  if (member->isString()) {
    return Type::string();
  }

  return nullptr;
}

const Type *TypesTextPatternsRegistry::patternInCase(TypeOp op, const Type *pattern) const {
  auto texts =
      std::vector<std::u16string>(pattern->patternTexts().begin(), pattern->patternTexts().end());
  auto holes =
      std::vector<const Type *>(pattern->patternHoles().begin(), pattern->patternHoles().end());
  bool isWholeText = op == TypeOp::Uppercase || op == TypeOp::Lowercase;
  size_t changedCount = isWholeText ? texts.size() : 1;
  for (size_t index = 0; index < changedCount; ++index) {
    texts[index] = this->changeCase(op, texts[index]);
  }

  return Type::stringPattern(texts, holes);
}

}
