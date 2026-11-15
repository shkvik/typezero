#include "compiler/cli/syntax/ast_dump/dump_text/dump_text.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

DumpText::DumpText(const char *begin)
    : begin(begin) {}

int64_t DumpText::offset(llvm::SMLoc loc) const {
  if (!loc.isValid()) {
    return -1;
  }

  return loc.getPointer() - this->begin;
}

std::string DumpText::span(llvm::StringRef text) const {
  if (text.empty()) {
    return "-";
  }
  int64_t start = text.data() - this->begin;

  return std::to_string(start) + "-" + std::to_string(start + int64_t(text.size()));
}

std::string DumpText::quote(llvm::StringRef text) const {
  std::string result = "\"";
  for (char c : text) {
    auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f || c == '"' || c == '\\') {
      llvm::raw_string_ostream escaped(result);
      escaped << "\\u" << llvm::format_hex_no_prefix(byte, 4);
      continue;
    }
    result += c;
  }

  return result + "\"";
}

std::string DumpText::units(const std::u16string &text) const {
  std::string result = "\"";
  for (char16_t unit : text) {
    if (unit >= 0x20 && unit < 0x7f && unit != u'"' && unit != u'\\') {
      result += static_cast<char>(unit);
      continue;
    }
    llvm::raw_string_ostream escaped(result);
    escaped << "\\u" << llvm::format_hex_no_prefix(static_cast<unsigned>(unit), 4);
  }

  return result + "\"";
}

std::string DumpText::bits(double value) const {
  std::string result;
  llvm::raw_string_ostream text(result);
  text << llvm::format_hex_no_prefix(llvm::bit_cast<uint64_t>(value), 16);

  return result;
}

std::string DumpText::flags(const std::vector<bool> &values) const {
  if (values.empty()) {
    return "-";
  }
  std::string result;
  for (bool value : values) {
    result += value ? '1' : '0';
  }

  return result;
}

std::string DumpText::quoteAll(const std::vector<llvm::StringRef> &names) const {
  std::string result = "[";
  for (size_t i = 0; i < names.size(); ++i) {
    result += (i == 0 ? "" : ",") + this->quote(names[i]);
  }

  return result + "]";
}

std::string DumpText::typeKind(const TypeRef &ref) const {
  static const char *const names[] = {
      "Name",   "Array",       "Union",     "Generic",  "Function",     "Object",
      "Tuple",  "Literal",     "Predicate", "KeyOf",    "Indexed",      "TypeOf",
      "Mapped", "Conditional", "Infer",     "Template", "Intersection",
  };

  return names[static_cast<int>(ref.kind())];
}

std::string DumpText::typeExtras(const TypeRef &ref) const {
  std::string result;
  llvm::raw_string_ostream text(result);
  switch (ref.kind()) {
    case TypeRefKind::FUNCTION_REF:
      text << " names=" << this->quoteAll(ref.names()) << " opt=" << this->flags(ref.optional())
           << " rest=" << ref.rest();
      break;
    case TypeRefKind::OBJECT_REF: {
      text << " names=" << this->quoteAll(ref.names()) << " locs=[";
      for (size_t i = 0; i < ref.locs().size(); ++i) {
        text << (i == 0 ? "" : ",") << this->offset(ref.locs()[i]);
      }
      text << "] opt=" << this->flags(ref.optional()) << " ro=" << this->flags(ref.readonly())
           << " methods=" << this->flags(ref.methods());
      break;
    }
    case TypeRefKind::TUPLE_REF:
      text << " ro=" << this->flags(ref.readonly()) << " spread=" << this->flags(ref.spread());
      break;
    case TypeRefKind::LITERAL_REF:
      text << " text=" << this->units(ref.text()) << " number=" << this->bits(ref.number())
           << " int=" << ref.integer();
      break;
    case TypeRefKind::PREDICATE_REF:
      text << " asserts=" << ref.asserts();
      break;
    case TypeRefKind::TYPE_OF_REF:
      text << " names=" << this->quoteAll(ref.names());
      break;
    case TypeRefKind::MAPPED_REF:
      text << " opt=" << ref.mappedOptional() << " ro=" << ref.mappedReadonly();
      break;
    case TypeRefKind::TEMPLATE_REF: {
      text << " texts=[";
      for (size_t i = 0; i < ref.texts().size(); ++i) {
        text << (i == 0 ? "" : ",") << this->units(ref.texts()[i]);
      }
      text << "]";
      break;
    }
    default:
      break;
  }

  return result;
}

std::string DumpText::role(const FunctionDecl *fn) const {
  const ClassMember *member = fn->member();
  if (member == nullptr) {
    return "plain";
  }

  return "member " + std::to_string(static_cast<int>(member->access)) + " " +
         std::to_string(member->isOverride ? 1 : 0) + " " +
         std::to_string(static_cast<int>(member->method.kind())) + " " +
         std::to_string(member->decorators.size());
}

}
