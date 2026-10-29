#include "compiler/lsp/lsp_channel/lsp_channel.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>

namespace tz {

LspChannel::LspChannel() {
  llvm::sys::ChangeStdinToBinary();
  llvm::sys::ChangeStdoutToBinary();
}

std::optional<std::string> LspChannel::read() {
  size_t length = 0;
  bool sized = false;
  std::string line;
  for (;;) {
    line.clear();
    int c = std::fgetc(stdin);
    while (c != EOF && c != '\n') {
      line.push_back(static_cast<char>(c));
      c = std::fgetc(stdin);
    }
    if (c == EOF) {
      return std::nullopt;
    }
    llvm::StringRef header = llvm::StringRef(line).rtrim("\r");
    if (header.empty()) {
      if (sized) {
        break;
      }
      continue;
    }
    if (header.consume_front_insensitive("content-length:") &&
        !header.trim().getAsInteger(10, length)) {
      sized = true;
    }
  }
  std::string body(length, '\0');
  if (std::fread(body.data(), 1, length, stdin) != length) {
    return std::nullopt;
  }

  return body;
}

void LspChannel::send(const llvm::json::Value &message) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << message;
  std::lock_guard<std::mutex> lock(mutex_);
  std::fprintf(stdout, "Content-Length: %zu\r\n\r\n", text.size());
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

}
