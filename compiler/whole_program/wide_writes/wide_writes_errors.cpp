#include "compiler/whole_program/wide_writes/wide_writes_errors.h"
#include "compiler/program/types/type/type.h"
#include "llvm/ADT/Twine.h"
#include <cctype>

namespace tz {

std::string ErrorTextWidening::article(const Type *type) {
  llvm::StringRef name = type->name();
  char first = name.empty() ? 'x' : char(std::tolower(static_cast<unsigned char>(name[0])));
  bool vowel = llvm::StringRef("aeiou").contains(first);
  llvm::StringRef prefix = vowel ? "an '" : "a '";

  return (llvm::Twine(prefix) + name + "'").str();
}

std::string ErrorTextWidening::what(const Type *value) {
  if (!value) {
    return "a value";
  }

  return ErrorTextWidening::article(value);
}

std::string ErrorTextWidening::place(const Type *owner, llvm::StringRef field, int depth) {
  std::string slot = owner->isTuple() ? (llvm::Twine("element ") + field).str()
                                      : (llvm::Twine("field '") + field + "'").str();
  if (depth == 0) {
    return "its " + slot;
  }
  std::string ownerText = ErrorTextWidening::article(owner);

  return slot + " of " + ownerText + " in it";
}

std::string ErrorTextWidening::someLine() {
  return "a line";
}

std::string ErrorTextWidening::line(int number) {
  return (llvm::Twine("line ") + llvm::Twine(number)).str();
}

std::string ErrorTextWidening::lineOf(int number, llvm::StringRef file) {
  return (llvm::Twine("line ") + llvm::Twine(number) + " of " + file).str();
}

std::string ErrorTextWidening::head(const Type *from, const Type *to, llvm::StringRef role,
                                    llvm::StringRef line) {
  std::string roleText = role.empty() ? std::string() : (llvm::Twine(", ") + role).str();

  return (llvm::Twine("'") + from->name() + "' cannot go where '" + to->name() +
          "' is expected here" + roleText + ": " + line)
      .str();
}

std::string ErrorTextWidening::intoArray(llvm::StringRef head, const Type *value,
                                         const Type *narrow, llvm::StringRef name) {
  std::string valueText = ErrorTextWidening::what(value);
  llvm::StringRef copied = name.empty() ? "xs" : name;

  return (llvm::Twine(head) + " puts " + valueText + " into it, and an array of '" +
          narrow->name() + "' cannot hold one; pass a copy, '[..." + copied + "]'")
      .str();
}

std::string ErrorTextWidening::intoInner(llvm::StringRef head, const Type *value,
                                         const Type *narrow) {
  std::string valueText = ErrorTextWidening::what(value);

  return (llvm::Twine(head) + " puts " + valueText + " into an array in it, and an array of '" +
          narrow->name() + "' cannot hold one")
      .str();
}

std::string ErrorTextWidening::readOnly(llvm::StringRef head, const Type *owner,
                                        llvm::StringRef field, int depth) {
  std::string placeText = ErrorTextWidening::place(owner, field, depth);

  return (llvm::Twine(head) + " writes to " + placeText + ", which is read-only in '" +
          owner->name() + "'")
      .str();
}

std::string ErrorTextWidening::otherType(llvm::StringRef head, const Type *value, const Type *owner,
                                         llvm::StringRef field, const Type *narrow, int depth) {
  std::string valueText = ErrorTextWidening::what(value);
  std::string placeText = ErrorTextWidening::place(owner, field, depth);
  std::string narrowText = ErrorTextWidening::article(narrow);

  return (llvm::Twine(head) + " writes " + valueText + " to " + placeText + ", which is " +
          narrowText + " in '" + owner->name() + "'")
      .str();
}

std::string ErrorTextWidening::through(const Type *type) {
  return (llvm::Twine("the write, through '") + type->name() + "'").str();
}

}
