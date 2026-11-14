#include "compiler/semantic/code/unions/code_unions_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUnionMember::narrow(llvm::StringRef name, llvm::StringRef type,
                                         llvm::StringRef typeOf) {
  return (llvm::Twine("property '") + name + "' does not exist on '" + type +
          "': check what it holds first, e.g. 'typeof x == \"" + typeOf + "\"'")
      .str();
}

std::string ErrorTextUnionMember::noField(llvm::StringRef type, llvm::StringRef name) {
  std::string message = (llvm::Twine("'") + type + "' has no field '" + name + "'").str();
  if (name == "errno") {
    message += ": tz gives no errno; the reason is in 'kind', the system's code in 'code'";
  }

  return message;
}

std::string ErrorTextUnionMember::mayLack(llvm::StringRef name, llvm::StringRef lacking,
                                          llvm::StringRef type, llvm::StringRef hint) {
  return (llvm::Twine("property '") + name + "' does not exist on '" + lacking + "', which '" +
          type + "' may be; narrow it first" + hint)
      .str();
}

std::string ErrorTextUnionMember::tagHint(llvm::StringRef field, llvm::StringRef tag) {
  return (llvm::Twine(", e.g. 'if (") + field + " == " + tag + ")'").str();
}

std::string ErrorTextUnionMember::classHint(llvm::StringRef cls) {
  return (llvm::Twine(", e.g. 'if (x instanceof ") + cls + ")'").str();
}

std::string ErrorTextUnionMember::inHint(llvm::StringRef name) {
  return (llvm::Twine(", e.g. 'if (\"") + name + "\" in x)'").str();
}

std::string ErrorTextUnionMember::clash(llvm::StringRef type, llvm::StringRef name,
                                        llvm::StringRef problem) {
  return (llvm::Twine("the members of '") + type + "' have fields '" + name +
          "' of types that one value cannot hold: " + problem + "; narrow it first")
      .str();
}

std::string ErrorTextUnionTarget::readOnly(llvm::StringRef name, llvm::StringRef member) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is a read-only field of '" +
          member + "'")
      .str();
}

std::string ErrorTextUnionTarget::differ(llvm::StringRef type, llvm::StringRef name,
                                         llvm::StringRef first, llvm::StringRef second) {
  return (llvm::Twine("the members of '") + type + "' have fields '" + name +
          "' of different types, '" + first + "' and '" + second +
          "', so no value fits them all; narrow it first")
      .str();
}

std::string ErrorTextUnionMethod::variants(llvm::StringRef name, llvm::StringRef member,
                                           llvm::StringRef receiver) {
  return (llvm::Twine("'") + name + "' of '" + member +
          "' has several variants, which a call on '" + receiver +
          "' cannot choose from; narrow it first")
      .str();
}

std::string ErrorTextUnionMethod::isBase(llvm::StringRef name, llvm::StringRef receiver,
                                         llvm::StringRef member) {
  return (llvm::Twine("'") + name + "' is called on '" + receiver + "', and '" + member +
          "' is an interface that classes implement; narrow it "
          "first, or declare the members to be one type with '" +
          name + "'")
      .str();
}

std::string ErrorTextUnionMethod::notMethod(llvm::StringRef receiver, llvm::StringRef member,
                                            llvm::StringRef name, bool field) {
  std::string why = field ? (llvm::Twine("whose '") + name + "' is not a function").str()
                          : (llvm::Twine("which has no method '") + name + "'").str();

  return (llvm::Twine("'") + receiver + "' may be a '" + member + "', " + why + "; narrow it first")
      .str();
}

std::string ErrorTextUnionMethod::differ(llvm::StringRef receiver, llvm::StringRef name,
                                         llvm::StringRef first, llvm::StringRef second) {
  return (llvm::Twine("the members of '") + receiver + "' have methods '" + name +
          "' of different types, '" + first + "' and '" + second + "'; narrow it first")
      .str();
}

}
