#include "compiler/semantic/types/template_types/template_types.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/extends_relation/extends_relation.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

namespace {

constexpr size_t kMaxTemplateStrings = 100000;
constexpr double kMaxWholeNumber = 9007199254740992.0;
}

TemplateTypes::TemplateTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeResolver = &all.typeResolver;
  this->typeOperators = &all.typeOperators;
  this->extendsRelation = &all.extendsRelation;
}

bool TemplateTypes::isTemplateHole(const Type *type) const {
  return type->isString() || type->isNumeric() || type->isBool() || type->isLiteral() ||
         type->isUndefined() || type->isParam();
}

std::u16string TemplateTypes::upperCase(char16_t unit) const {
  if (unit >= u'a' && unit <= u'z') {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit == 0xdf) {
    return u"SS";
  }
  if (unit == 0xb5) {
    return std::u16string(1, char16_t(0x39c));
  }
  if (unit >= 0xe0 && unit <= 0xfe && unit != 0xf7) {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit == 0xff) {
    return std::u16string(1, char16_t(0x178));
  }
  if (unit == 0x131) {
    return u"I";
  }
  if (unit == 0x17f) {
    return u"S";
  }
  if ((unit >= 0x100 && unit <= 0x137) || (unit >= 0x14a && unit <= 0x177)) {
    return std::u16string(1, char16_t(unit & ~1u));
  }
  if ((unit >= 0x139 && unit <= 0x148) || (unit >= 0x179 && unit <= 0x17e)) {
    return std::u16string(1, char16_t(unit % 2 == 0 ? unit - 1 : unit));
  }
  if (unit == 0x3c2) {
    return std::u16string(1, char16_t(0x3a3));
  }
  if ((unit >= 0x3b1 && unit <= 0x3c1) || (unit >= 0x3c3 && unit <= 0x3cb)) {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit == 0x3ac) {
    return std::u16string(1, char16_t(0x386));
  }
  if (unit >= 0x3ad && unit <= 0x3af) {
    return std::u16string(1, char16_t(unit - 37));
  }
  if (unit == 0x3cc) {
    return std::u16string(1, char16_t(0x38c));
  }
  if (unit == 0x3cd || unit == 0x3ce) {
    return std::u16string(1, char16_t(unit - 63));
  }
  if (unit >= 0x430 && unit <= 0x44f) {
    return std::u16string(1, char16_t(unit - 32));
  }
  if (unit >= 0x450 && unit <= 0x45f) {
    return std::u16string(1, char16_t(unit - 80));
  }

  return std::u16string(1, unit);
}

std::u16string TemplateTypes::lowerCase(char16_t unit) const {
  if (unit >= u'A' && unit <= u'Z') {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit >= 0xc0 && unit <= 0xde && unit != 0xd7) {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit == 0x178) {
    return std::u16string(1, char16_t(0xff));
  }
  if (unit == 0x130) {
    return std::u16string{u'i', char16_t(0x307)};
  }
  if ((unit >= 0x100 && unit <= 0x137) || (unit >= 0x14a && unit <= 0x177)) {
    return std::u16string(1, char16_t(unit | 1u));
  }
  if ((unit >= 0x139 && unit <= 0x148) || (unit >= 0x179 && unit <= 0x17e)) {
    return std::u16string(1, char16_t(unit % 2 == 1 ? unit + 1 : unit));
  }
  if ((unit >= 0x391 && unit <= 0x3a1) || (unit >= 0x3a3 && unit <= 0x3ab)) {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit == 0x386) {
    return std::u16string(1, char16_t(0x3ac));
  }
  if (unit >= 0x388 && unit <= 0x38a) {
    return std::u16string(1, char16_t(unit + 37));
  }
  if (unit == 0x38c) {
    return std::u16string(1, char16_t(0x3cc));
  }
  if (unit == 0x38e || unit == 0x38f) {
    return std::u16string(1, char16_t(unit + 63));
  }
  if (unit >= 0x410 && unit <= 0x42f) {
    return std::u16string(1, char16_t(unit + 32));
  }
  if (unit >= 0x400 && unit <= 0x40f) {
    return std::u16string(1, char16_t(unit + 80));
  }

  return std::u16string(1, unit);
}

std::u16string TemplateTypes::unitInCase(TypeOp op, char16_t unit, bool isFirst) const {
  bool isUpper = op == TypeOp::Uppercase || (op == TypeOp::Capitalize && isFirst);
  bool isLower = op == TypeOp::Lowercase || (op == TypeOp::Uncapitalize && isFirst);
  if (isUpper) {
    return this->upperCase(unit);
  }
  if (isLower) {
    return this->lowerCase(unit);
  }

  return std::u16string(1, unit);
}

std::u16string TemplateTypes::changeCase(TypeOp op, const std::u16string &text) const {
  auto changed = std::u16string();
  for (size_t index = 0; index < text.size(); ++index) {
    bool isFirst = index == 0;
    changed += this->unitInCase(op, text[index], isFirst);
  }

  return changed;
}

const Type *TemplateTypes::firstNonHole(const Type *hole) const {
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(hole)) {
    if (!this->isTemplateHole(member)) {
      return member;
    }
  }

  return nullptr;
}

TemplateTypes::ResultHoleAccepted TemplateTypes::checkHole(const Type *hole) const {
  const Type *rejected = this->firstNonHole(hole);
  if (rejected != nullptr) {
    auto message = ErrorTextHoleType::notHole(rejected->name());

    return err(TemplateTypesHoleTypeError{
        .code = TemplateTypesHoleTypeErrorCode::NOT_A_HOLE,
        .message = message,
    });
  }

  return ok(hole);
}

TemplateTypes::ResultCountAllowed TemplateTypes::checkStringCount(size_t count) const {
  if (count > kMaxTemplateStrings) {
    auto message = ErrorTextStringCount::tooMany(kMaxTemplateStrings);

    return err(TemplateTypesStringCountError{
        .code = TemplateTypesStringCountErrorCode::TOO_MANY_STRINGS,
        .message = message,
    });
  }

  return ok(count);
}

bool TemplateTypes::isMatchingTemplate(const std::u16string &text,
                                       llvm::ArrayRef<const Type *> parts, InferMatch *inference) {
  auto texts = std::vector<std::u16string>();
  auto holes = std::vector<const Type *>();
  for (size_t index = 0; index < parts.size(); ++index) {
    bool isText = index % 2 == 0;
    if (isText) {
      texts.push_back(parts[index]->literalText());
    } else {
      holes.push_back(parts[index]);
    }
  }
  auto pieces = std::vector<std::u16string>();
  if (!TemplatePatterns::splitByTemplate(text, texts, pieces)) {
    return false;
  }
  for (size_t index = 0; index < holes.size(); ++index) {
    if (!this->isPieceFittingHole(pieces[index], holes[index], inference)) {
      return false;
    }
  }

  return true;
}

bool TemplateTypes::isPieceFittingHole(const std::u16string &piece, const Type *hole,
                                       InferMatch *inference) {
  if (!TypeQueries::mentionsParam(hole)) {
    return TemplatePatterns::holeAccepts(hole, piece);
  }
  const Type *pieceType = this->pieceType(piece, hole, inference);
  if (pieceType == nullptr) {
    return false;
  }

  return this->extendsRelation->isExtendingType(pieceType, hole, inference);
}

const Type *TemplateTypes::pieceType(const std::u16string &piece, const Type *hole,
                                     const InferMatch *inference) const {
  if (!this->isNumberHole(hole, inference)) {
    return Type::stringLiteral(piece);
  }
  double value = 0;
  if (!TemplatePatterns::numberString(piece, value)) {
    return nullptr;
  }
  const Type *number = this->numberLiteral(value);
  if (number->name() != ToUtf8::toUtf8(piece)) {
    return hole->constraint()->nonOptional();
  }

  return number;
}

bool TemplateTypes::isNumberHole(const Type *hole, const InferMatch *inference) const {
  bool isInferred = inference != nullptr && llvm::is_contained(inference->params, hole);
  if (!isInferred || hole->constraint() == nullptr) {
    return false;
  }

  return hole->constraint()->nonOptional()->isNumeric();
}

const Type *TemplateTypes::numberLiteral(double value) const {
  bool isWhole = value == std::trunc(value) && std::fabs(value) <= kMaxWholeNumber;
  if (isWhole) {
    return Type::numberLiteral(value, Type::intTy());
  }

  return Type::numberLiteral(value, Type::doubleTy());
}

const Type *TemplateTypes::resolveTemplate(const TypeRef &ref) {
  auto operands = std::vector<const Type *>();
  for (size_t index = 0; index < ref.texts().size(); ++index) {
    const auto *text = Type::stringLiteral(ref.texts()[index]);
    operands.push_back(text);
    if (index >= ref.args.size()) {
      break;
    }
    const Type *hole = this->resolveHole(ref.args[index]);
    if (hole->isError()) {
      return hole;
    }
    operands.push_back(hole);
  }
  this->state.getTypeContext().rememberTemplateLoc(ref.loc);
  const auto *unevaluated = Type::typeOp(TypeOp::Template, operands);
  const auto *evaluated = this->typeOperators->evalTypeOp(unevaluated, operands);
  this->state.getTypeContext().forgetTemplateLoc();

  return evaluated;
}

const Type *TemplateTypes::resolveHole(const TypeRef &arg) {
  const auto *hole = this->typeResolver->resolveTypeInner(arg);
  if (hole->isError()) {
    return hole;
  }
  auto accepted = this->checkHole(hole);
  if (accepted.isErr()) {
    this->state.getDiagnostics().error(arg.loc, accepted.error().message);

    return Type::error();
  }

  return hole;
}

const Type *TemplateTypes::evalTemplate(llvm::ArrayRef<const Type *> operands) {
  auto first = TemplatePart{.texts = {operands[0]->literalText()}, .holes = {}};
  auto parts = std::vector<TemplatePart>{first};
  for (size_t index = 1; index + 1 < operands.size(); index += 2) {
    auto members = TypeopsCheckHelpers::distributedMembers(operands[index]);
    const std::u16string &after = operands[index + 1]->literalText();
    auto grown = this->grownParts(parts, members, after);
    if (!grown.has_value()) {
      return Type::error();
    }
    parts = std::move(*grown);
  }
  auto results = std::vector<const Type *>();
  for (const TemplatePart &part : parts) {
    results.push_back(this->partType(part));
  }
  if (results.empty()) {
    return Type::never();
  }

  return Type::unionOf(results);
}

std::optional<std::vector<TemplateTypes::TemplatePart>>
TemplateTypes::grownParts(const std::vector<TemplatePart> &parts,
                          llvm::ArrayRef<const Type *> members, const std::u16string &after) {
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

std::optional<TemplateTypes::TemplatePart> TemplateTypes::partWithMember(const TemplatePart &part,
                                                                         const Type *member) const {
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

void TemplateTypes::appendPattern(TemplatePart &part, const Type *pattern) const {
  part.texts.back() += pattern->patternTexts()[0];
  for (size_t index = 0; index < pattern->patternHoles().size(); ++index) {
    part.holes.push_back(pattern->patternHoles()[index]);
    part.texts.push_back(pattern->patternTexts()[index + 1]);
  }
}

void TemplateTypes::reportTooManyStrings(llvm::StringRef message) {
  auto templateLoc = this->state.getTypeContext().getTemplateLoc();
  auto instanceLoc = this->state.getGenericStore().getInstanceLoc();
  llvm::SMLoc loc = templateLoc.isValid() ? templateLoc : instanceLoc;
  if (loc.isValid()) {
    this->state.getDiagnostics().error(loc, message);
  }
}

const Type *TemplateTypes::partType(const TemplatePart &part) const {
  if (part.holes.empty()) {
    return Type::stringLiteral(part.texts[0]);
  }

  return Type::stringPattern(part.texts, part.holes);
}

const Type *TemplateTypes::evalStringCase(TypeOp op, const Type *type) {
  auto results = std::vector<const Type *>();
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(type)) {
    const Type *changed = this->memberInCase(op, member);
    if (changed == nullptr) {
      return Type::error();
    }
    results.push_back(changed);
  }
  if (results.empty()) {
    return Type::never();
  }

  return Type::unionOf(results);
}

const Type *TemplateTypes::memberInCase(TypeOp op, const Type *member) const {
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

const Type *TemplateTypes::patternInCase(TypeOp op, const Type *pattern) const {
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
