#include "runtime/core/output/stack_trace.h"
#include "runtime/core/output/output.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

enum { kMaxFrames = 64, kMaxShown = 24 };

typedef DWORD(WINAPI *SymSetOptionsFn)(DWORD);
typedef BOOL(WINAPI *SymInitializeWFn)(HANDLE, PCWSTR, BOOL);
typedef BOOL(WINAPI *SymCleanupFn)(HANDLE);
typedef DWORD64(WINAPI *SymLoadModuleExWFn)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD,
                                            PMODLOAD_DATA, DWORD);
typedef BOOL(WINAPI *SymFromAddrWFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
typedef BOOL(WINAPI *SymGetLineFromAddrW64Fn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINEW64);
typedef DWORD(WINAPI *SymAddrIncludeInlineTraceFn)(HANDLE, DWORD64);
typedef BOOL(WINAPI *SymQueryInlineTraceFn)(HANDLE, DWORD64, DWORD, DWORD64, DWORD64, LPDWORD,
                                            LPDWORD);
typedef BOOL(WINAPI *SymFromInlineContextWFn)(HANDLE, DWORD64, ULONG, PDWORD64, PSYMBOL_INFOW);
typedef BOOL(WINAPI *SymGetLineFromInlineContextWFn)(HANDLE, DWORD64, ULONG, DWORD64, PDWORD,
                                                     PIMAGEHLP_LINEW64);

typedef struct {
  DWORD64 frames[kMaxFrames];
  int count;
  int exact;
  uintptr_t base;
  uintptr_t end;
  int shown;
  int repeats;
  size_t lastLength;
  char last[1600];
  HANDLE process;
  SymFromAddrWFn fromAddr;
  SymGetLineFromAddrW64Fn lineFromAddr;
  SymAddrIncludeInlineTraceFn inlineCount;
  SymQueryInlineTraceFn inlineQuery;
  SymFromInlineContextWFn fromInline;
  SymGetLineFromInlineContextWFn lineFromInline;
  union {
    SYMBOL_INFOW info;
    char space[sizeof(SYMBOL_INFOW) + 512 * sizeof(WCHAR)];
  } symbol;
} Trace;

static void put(const char *text, size_t len) { tz_err_raw(text, len); }

static size_t utf8_of(const WCHAR *text, char *out, size_t size) {
  int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)size, NULL, NULL);
  if (n <= 0) {
    out[0] = 0;
    return 0;
  }

  return (size_t)n - 1;
}

static int is_tz_file(const WCHAR *file) {
  size_t n = wcslen(file);

  return n > 3 && _wcsicmp(file + n - 3, L".tz") == 0;
}

static void flush_repeats(Trace *trace) {
  if (trace->repeats == 0) {
    return;
  }
  char text[80];
  int n =
      snprintf(text, sizeof text, "    [previous line repeated %d more times]\n", trace->repeats);
  if (n > 0) {
    put(text, (size_t)n);
  }
  trace->repeats = 0;
}

static void show(Trace *trace, const WCHAR *name, const IMAGEHLP_LINEW64 *line) {
  if (!is_tz_file(line->FileName) || trace->shown > kMaxShown) {
    return;
  }
  char text[1600], function[512], file[1024];
  utf8_of(name, function, sizeof function);
  utf8_of(line->FileName, file, sizeof file);
  int n =
      function[0] == '<' || function[0] == 0
          ? snprintf(text, sizeof text, "    at %s:%lu\n", file, line->LineNumber)
          : snprintf(text, sizeof text, "    at %s (%s:%lu)\n", function, file, line->LineNumber);
  if (n <= 0) {
    return;
  }
  size_t length = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1;
  if (length == trace->lastLength && memcmp(text, trace->last, length) == 0) {
    ++trace->repeats;
    return;
  }
  flush_repeats(trace);
  if (trace->shown == kMaxShown) {
    put("    ...\n", 8);
    ++trace->shown;
    return;
  }
  put(text, length);
  memcpy(trace->last, text, length);
  trace->lastLength = length;
  ++trace->shown;
}

static int walk(CONTEXT *context, DWORD64 *frames, int max) {
  int n = 0;
#if defined(_M_X64) || defined(__x86_64__)
  while (n < max && context->Rip != 0) {
    frames[n++] = context->Rip;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context->Rip, &base, NULL);
    if (!entry) {
      context->Rip = *(DWORD64 *)context->Rsp;
      context->Rsp += 8;
      continue;
    }
    void *handler = NULL;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context->Rip, entry, context, &handler, &frame, NULL);
  }
#else
  (void)context;
  n = RtlCaptureStackBackTrace(0, (DWORD)max, (void **)frames, NULL);
#endif

  return n;
}

Trace tz_stack_trace;
volatile LONG tz_stack_busy;

#else

#endif
