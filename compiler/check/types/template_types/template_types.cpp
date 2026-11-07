#include "compiler/check/types/template_types/template_types.h"
#include "compiler/check/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/check/types/extends_relation/extends_relation.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>


namespace tz {

namespace {

constexpr size_t kMaxTemplateStrings = 100000;

}

std::string TemplateTypes::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::u16string TemplateTypes::utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

std::vector<const Type *> TemplateTypes::distributed(const Type *type) {
  std::vector<const Type *> out;
  distribute(type, out);

  return out;
}

bool TemplateTypes::templateHole(const Type *type) {
  return type->isString() || type->isNumeric() || type->isBool() || type->isLiteral() ||
         type->isUndefined() || type->isParam();
}

std::u16string TemplateTypes::upperCase(char16_t c) {
  if (c >= u'a' && c <= u'z') {
    return std::u16string(1, char16_t(c - 32));
  }
  if (c == 0xdf) {
    return u"SS";
  }
  if (c == 0xb5) {
    return std::u16string(1, char16_t(0x39c));
  }
  if (c >= 0xe0 && c <= 0xfe && c != 0xf7) {
    return std::u16string(1, char16_t(c - 32));
  }
  if (c == 0xff) {
    return std::u16string(1, char16_t(0x178));
  }
  if (c == 0x131 || c == 0x17f) {
    return c == 0x131 ? u"I" : u"S";
  }
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) {
    return std::u16string(1, char16_t(c & ~1u));
  }
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) {
    return std::u16string(1, char16_t(c % 2 == 0 ? c - 1 : c));
  }
  if (c == 0x3c2) {
    return std::u16string(1, char16_t(0x3a3));
  }
  if ((c >= 0x3b1 && c <= 0x3c1) || (c >= 0x3c3 && c <= 0x3cb)) {
    return std::u16string(1, char16_t(c - 32));
  }
  if (c == 0x3ac) {
    return std::u16string(1, char16_t(0x386));
  }
  if (c >= 0x3ad && c <= 0x3af) {
    return std::u16string(1, char16_t(c - 37));
  }
  if (c == 0x3cc) {
    return std::u16string(1, char16_t(0x38c));
  }
  if (c == 0x3cd || c == 0x3ce) {
    return std::u16string(1, char16_t(c - 63));
  }
  if (c >= 0x430 && c <= 0x44f) {
    return std::u16string(1, char16_t(c - 32));
  }
  if (c >= 0x450 && c <= 0x45f) {
    return std::u16string(1, char16_t(c - 80));
  }

  return std::u16string(1, c);
}

std::u16string TemplateTypes::lowerCase(char16_t c) {
  if (c >= u'A' && c <= u'Z') {
    return std::u16string(1, char16_t(c + 32));
  }
  if (c >= 0xc0 && c <= 0xde && c != 0xd7) {
    return std::u16string(1, char16_t(c + 32));
  }
  if (c == 0x178) {
    return std::u16string(1, char16_t(0xff));
  }
  if (c == 0x130) {
    return std::u16string{u'i', char16_t(0x307)};
  }
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) {
    return std::u16string(1, char16_t(c | 1u));
  }
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) {
    return std::u16string(1, char16_t(c % 2 == 1 ? c + 1 : c));
  }
  if ((c >= 0x391 && c <= 0x3a1) || (c >= 0x3a3 && c <= 0x3ab)) {
    return std::u16string(1, char16_t(c + 32));
  }
  if (c == 0x386) {
    return std::u16string(1, char16_t(0x3ac));
  }
  if (c >= 0x388 && c <= 0x38a) {
    return std::u16string(1, char16_t(c + 37));
  }
  if (c == 0x38c) {
    return std::u16string(1, char16_t(0x3cc));
  }
  if (c == 0x38e || c == 0x38f) {
    return std::u16string(1, char16_t(c + 63));
  }
  if (c >= 0x410 && c <= 0x42f) {
    return std::u16string(1, char16_t(c + 32));
  }
  if (c >= 0x400 && c <= 0x40f) {
    return std::u16string(1, char16_t(c + 80));
  }

  return std::u16string(1, c);
}

std::u16string TemplateTypes::changeCase(TypeOp op, const std::u16string &text) {
  std::u16string out;
  for (size_t i = 0; i < text.size(); ++i) {
    bool first = i == 0;
    if (op == TypeOp::Uppercase || (op == TypeOp::Capitalize && first)) {
      out += TemplateTypes::upperCase(text[i]);
    } else if (op == TypeOp::Lowercase || (op == TypeOp::Uncapitalize && first)) {
      out += TemplateTypes::lowerCase(text[i]);
    } else {
      out += text[i];
    }
  }

  return out;
}

TemplateTypes::TemplateTypes(Checker &owner)
    : checker(owner) {}

bool TemplateTypes::matchTemplate(const std::u16string &text, llvm::ArrayRef<const Type *> parts,
                                  Checker::InferMatch *inference) {
  std::vector<std::u16string> texts;
  std::vector<const Type *> holes;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i % 2 == 0) {
      texts.push_back(parts[i]->literalText());
    } else {
      holes.push_back(parts[i]);
    }
  }
  std::vector<std::u16string> pieces;
  if (!splitByTemplate(text, texts, pieces)) {
    return false;
  }
  for (size_t k = 0; k < holes.size(); ++k) {
    const Type *hole = holes[k];
    if (!mentionsParam(hole)) {
      if (!holeAccepts(hole, pieces[k])) {
        return false;
      }
      continue;
    }
    const Type *piece = Type::stringLiteral(pieces[k]);
    if (inference && llvm::is_contained(inference->params, hole)) {
      if (const Type *constraint = hole->constraint();
          constraint && constraint->nonOptional()->isNumeric()) {
        double value = 0;
        if (!numberString(pieces[k], value)) {
          return false;
        }
        bool whole = value == std::trunc(value) && std::fabs(value) <= 9007199254740992.0;
        const Type *number = Type::numberLiteral(value, whole ? Type::intTy() : Type::doubleTy());
        piece =
            number->name() == TemplateTypes::utf8(pieces[k]) ? number : constraint->nonOptional();
      }
    }
    if (!this->checker.extendsRelation().extendsType(piece, hole, inference)) {
      return false;
    }
  }

  return true;
}

const Type *TemplateTypes::resolveTemplate(const TypeRef &ref) {
  std::vector<const Type *> operands;
  for (size_t i = 0; i < ref.texts().size(); ++i) {
    operands.push_back(Type::stringLiteral(ref.texts()[i]));
    if (i >= ref.args.size()) {
      break;
    }
    const Type *hole = this->checker.typeResolver().resolveTypeInner(ref.args[i]);
    if (hole->isError()) {
      return hole;
    }
    for (const Type *m : TemplateTypes::distributed(hole)) {
      if (!TemplateTypes::templateHole(m)) {
        this->checker.diags_.error(ref.args[i].loc,
                                   "a template literal type holds strings, numbers, booleans and "
                                   "undefined, not '" +
                                       m->name() + "'");
        return Type::error();
      }
    }
    operands.push_back(hole);
  }
  this->templateLoc = ref.loc;
  const Type *type =
      this->checker.typeOperators().evalTypeOp(Type::typeOp(TypeOp::Template, operands), operands);
  this->templateLoc = llvm::SMLoc();

  return type;
}

const Type *TemplateTypes::evalTemplate(llvm::ArrayRef<const Type *> operands) {
  struct Part {
    std::vector<std::u16string> texts;
    std::vector<const Type *> holes;
  };
  std::vector<Part> parts{{{operands[0]->literalText()}, {}}};
  for (size_t i = 1; i + 1 < operands.size(); i += 2) {
    std::vector<const Type *> members = TemplateTypes::distributed(operands[i]);
    const std::u16string &after = operands[i + 1]->literalText();
    std::vector<Part> next;
    for (const Part &part : parts) {
      for (const Type *m : members) {
        Part made = part;
        if (m->isStringLiteral()) {
          made.texts.back() += m->literalText();
        } else if (m->isLiteral()) {
          made.texts.back() += TemplateTypes::utf16(m->name());
        } else if (m->isUndefined()) {
          made.texts.back() += u"undefined";
        } else if (m->isPattern()) {
          made.texts.back() += m->patternTexts()[0];
          for (size_t k = 0; k < m->patternHoles().size(); ++k) {
            made.holes.push_back(m->patternHoles()[k]);
            made.texts.push_back(m->patternTexts()[k + 1]);
          }
        } else if (m->isString() || m->isNumeric()) {
          made.holes.push_back(m);
          made.texts.emplace_back();
        } else {
          return Type::error();
        }
        made.texts.back() += after;
        next.push_back(std::move(made));
        if (next.size() > kMaxTemplateStrings) {
          llvm::SMLoc loc =
              this->templateLoc.isValid() ? this->templateLoc : this->checker.instanceLoc_;
          if (loc.isValid()) {
            this->checker.diags_.error(loc,
                                       "this template literal type makes more than " +
                                           std::to_string(kMaxTemplateStrings) +
                                           " strings, the most TypeScript allows; use 'string'");
          }
          return Type::error();
        }
      }
    }
    parts = std::move(next);
  }
  std::vector<const Type *> results;
  for (Part &part : parts) {
    results.push_back(part.holes.empty() ? Type::stringLiteral(part.texts[0])
                                         : Type::stringPattern(part.texts, part.holes));
  }

  return results.empty() ? Type::never() : Type::unionOf(results);
}

const Type *TemplateTypes::evalStringCase(TypeOp op, const Type *type) {
  std::vector<const Type *> results;
  for (const Type *m : TemplateTypes::distributed(type)) {
    if (m->isStringLiteral()) {
      results.push_back(Type::stringLiteral(TemplateTypes::changeCase(op, m->literalText())));
    } else if (m->isPattern()) {
      std::vector<std::u16string> texts(m->patternTexts().begin(), m->patternTexts().end());
      if (op == TypeOp::Uppercase || op == TypeOp::Lowercase) {
        for (std::u16string &text : texts) {
          text = TemplateTypes::changeCase(op, text);
        }
      } else {
        texts[0] = TemplateTypes::changeCase(op, texts[0]);
      }
      results.push_back(Type::stringPattern(
          texts, std::vector<const Type *>(m->patternHoles().begin(), m->patternHoles().end())));
    } else if (m->isString()) {
      results.push_back(Type::string());
    } else {
      return Type::error();
    }
  }

  return results.empty() ? Type::never() : Type::unionOf(results);
}

}
