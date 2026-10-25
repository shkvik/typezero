#include "compiler/packages/net/net.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace tz::pkg {

std::string fileUrlPath(llvm::StringRef url) {
  if (!url.consume_front("file://"))
    return "";
  // "file:///C:/x" has an empty host; "file://C:/x" is written too.
  if (url.size() > 2 && url[0] == '/' && llvm::isAlpha(url[1]) && url[2] == ':')
    url = url.drop_front();
  std::string path;
  for (size_t i = 0; i < url.size(); ++i) {
    unsigned hi, lo;
    if (url[i] == '%' && i + 2 < url.size() && (hi = llvm::hexDigitValue(url[i + 1])) != ~0U &&
        (lo = llvm::hexDigitValue(url[i + 2])) != ~0U) {
      path += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      path += url[i];
    }
  }
  return path;
}

namespace {

FetchResult readFile(const std::string &path, std::string &body, std::string &error) {
  auto buffer = llvm::MemoryBuffer::getFile(path, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!buffer) {
    if (buffer.getError() == std::errc::no_such_file_or_directory)
      return FetchResult::NotFound;
    error = buffer.getError().message();
    return FetchResult::Failed;
  }
  body = (*buffer)->getBuffer().str();
  return FetchResult::Ok;
}

#ifdef _WIN32

std::string winhttpError(DWORD code) {
  wchar_t *text = nullptr;
  DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                GetModuleHandleW(L"winhttp.dll"), code, 0,
                                reinterpret_cast<wchar_t *>(&text), 0, nullptr);
  std::string message;
  if (length && text) {
    llvm::convertWideToUTF8(std::wstring(text, length), message);
    LocalFree(text);
  }
  message = llvm::StringRef(message).trim().str();
  return message.empty() ? "error " + std::to_string(code) : message;
}

// One session for the whole run: WinHTTP keeps its connections open between
// requests, and a package of a hundred files is a hundred requests.
HINTERNET session() {
  static HINTERNET handle = [] {
    HINTERNET h = WinHttpOpen(L"tz", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) // before Windows 8.1
      h = WinHttpOpen(L"tz", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return h;
  }();
  return handle;
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h)
      WinHttpCloseHandle(h);
  }
};

FetchResult httpGet(const std::string &url, std::string &body, std::string &error) {
  std::wstring wide;
  if (!llvm::ConvertUTF8toWide(url, wide)) {
    error = "the address is not valid UTF-8";
    return FetchResult::Failed;
  }
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
    error = winhttpError(GetLastError());
    return FetchResult::Failed;
  }
  std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.lpszExtraInfo)
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  HINTERNET s = session();
  if (!s) {
    error = winhttpError(GetLastError());
    return FetchResult::Failed;
  }
  Handle connection{WinHttpConnect(s, host.c_str(), parts.nPort, 0)};
  Handle request;
  if (connection.h)
    request.h = WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE
                                                                          : 0);
  if (!request.h ||
      !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                          0, 0, 0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    error = winhttpError(GetLastError());
    return FetchResult::Failed;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  body.clear();
  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.h, &available)) {
      error = winhttpError(GetLastError());
      return FetchResult::Failed;
    }
    if (available == 0)
      break;
    size_t old = body.size();
    body.resize(old + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.h, body.data() + old, available, &read)) {
      error = winhttpError(GetLastError());
      return FetchResult::Failed;
    }
    body.resize(old + read);
  }
  if (status == 404 || status == 410)
    return FetchResult::NotFound;
  if (status != 200) {
    error = "the server answered " + std::to_string(status);
    return FetchResult::Failed;
  }
  return FetchResult::Ok;
}

#else

// Without WinHTTP, the curl program does the request.
FetchResult httpGet(const std::string &url, std::string &body, std::string &error) {
  llvm::ErrorOr<std::string> curl = llvm::sys::findProgramByName("curl");
  if (!curl) {
    error = "downloading needs the curl program, and it is not installed";
    return FetchResult::Failed;
  }
  llvm::SmallString<128> out, status;
  llvm::sys::fs::createTemporaryFile("tz-fetch", "body", out);
  llvm::sys::fs::createTemporaryFile("tz-fetch", "status", status);
  llvm::FileRemover removeOut(out), removeStatus(status);
  llvm::StringRef args[] = {*curl, "-sSL", "-o", out, "-w", "%{http_code}", url};
  std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(status),
                                                llvm::StringRef(status)};
  std::string message;
  int code = llvm::sys::ExecuteAndWait(*curl, args, std::nullopt, redirects, 0, 0, &message);
  std::string answer;
  readFile(std::string(status), answer, message);
  if (code != 0) {
    error = llvm::StringRef(answer).trim().str();
    return FetchResult::Failed;
  }
  llvm::StringRef http = llvm::StringRef(answer).trim();
  if (http == "404" || http == "410")
    return FetchResult::NotFound;
  if (http != "200") {
    error = "the server answered " + http.str();
    return FetchResult::Failed;
  }
  return readFile(std::string(out), body, error);
}

#endif

} // namespace

FetchResult fetchUrl(const std::string &url, std::string &body, std::string &error) {
  llvm::StringRef ref(url);
  if (ref.starts_with("file://"))
    return readFile(fileUrlPath(ref), body, error);
  if (ref.starts_with("https://") || ref.starts_with("http://"))
    return httpGet(url, body, error);
  error = "'" + url + "' is not an http://, https:// or file:// address";
  return FetchResult::Failed;
}

} // namespace tz::pkg
