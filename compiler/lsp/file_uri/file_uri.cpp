#include "compiler/lsp/file_uri/file_uri.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

namespace tz {

namespace {

namespace path = llvm::sys::path;

bool isDrive(llvm::StringRef text) {
  return text.size() >= 2 && llvm::isAlpha(text[0]) && text[1] == ':';
}

}

std::string FileUri::toPath(llvm::StringRef uri) {
  if (!uri.consume_front_insensitive("file://")) {
    return "";
  }
  std::string decoded;
  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && llvm::isHexDigit(uri[i + 1]) &&
        llvm::isHexDigit(uri[i + 2])) {
      unsigned byte = llvm::hexDigitValue(uri[i + 1]) * 16 + llvm::hexDigitValue(uri[i + 2]);
      decoded.push_back(static_cast<char>(byte));
      i += 2;
    } else {
      decoded.push_back(uri[i]);
    }
  }
  llvm::StringRef rest = decoded;
  std::string result;
  if (!rest.starts_with("/")) {
    result = "//" + rest.str();
  } else if (isDrive(rest.drop_front())) {
    result = rest.drop_front().str();
  } else {
    result = rest.str();
  }
  llvm::SmallString<256> native(result);
  path::native(native);

  return std::string(native);
}

std::string FileUri::fromPath(llvm::StringRef file) {
  std::string text = path::convert_to_slash(file);
  std::string uri = "file://";
  if (isDrive(text)) {
    uri += "/";
    uri.push_back(static_cast<char>(llvm::toLower(text[0])));
    uri += "%3A";
    text = text.substr(2);
  } else if (!llvm::StringRef(text).starts_with("/")) {
    uri += "/";
  }
  for (char c : text) {
    unsigned char byte = static_cast<unsigned char>(c);
    if (llvm::isAlnum(c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~') {
      uri.push_back(c);
    } else {
      uri += "%" + llvm::utohexstr(byte, false, 2);
    }
  }

  return uri;
}

}
