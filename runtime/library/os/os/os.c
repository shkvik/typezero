#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif

#include "runtime/library/os/os/os.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/process/process/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pwd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#endif

#ifdef _WIN32
static TzStr env_str(const char *name, int *found) {
  TzStr key = tz_str_from_ascii(name, strlen(name)), value;
  *found = tz_process_env(&value, key.obj, key.meta);
  tz_str_release_value(key);
  if (*found) {
    TzBuf b;
    tz_buf_init(&b);
    tz_buf_str(&b, value);
    if (b.length == 0) {
      tz_str_release_value(value);
      *found = 0;
    }
    tz_buf_free(&b);
  }

  return value;
}

typedef LSTATUS(WINAPI *RegGetValueFn)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

static int registry_value(const wchar_t *key, const wchar_t *name, DWORD flags, void *out,
                          DWORD *size) {
  HMODULE advapi = LoadLibraryW(L"advapi32.dll");
  RegGetValueFn get = advapi ? (RegGetValueFn)(void *)GetProcAddress(advapi, "RegGetValueW") : NULL;

  return get && get(HKEY_LOCAL_MACHINE, key, name, flags, NULL, out, size) == ERROR_SUCCESS;
}
#else
static const char *env_text(const char *name) {
  const char *value = getenv(name);

  return value && value[0] ? value : NULL;
}

static TzStr str_of_text(const char *text) {
  TzBuf b;
  tz_buf_init(&b);
  for (const unsigned char *s = (const unsigned char *)text; *s;) {
    uint32_t c = *s;
    size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    size_t k = 1;
    while (len > 1 && k < len && (s[k] & 0xC0) == 0x80) {
      ++k;
    }
    if (len == 0 || k < len) {
      tz_buf_push(&b, 0xFFFD);
      s += k;
      continue;
    }
    if (len > 1) {
      c &= 0x3Fu >> (len - 1);
      for (size_t i = 1; i < len; ++i) {
        c = (c << 6) | (s[i] & 0x3Fu);
      }
    }
    s += len;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(&b, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(&b, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(&b, (uint16_t)c);
    }
  }

  return tz_buf_take(&b);
}
#endif

static void homedir(TzStr *out) {
#ifdef _WIN32
  int found;
  *out = env_str("USERPROFILE", &found);
  if (!found) {
    TzStr drive = env_str("HOMEDRIVE", &found), path;
    int has_path;
    path = env_str("HOMEPATH", &has_path);
    TzBuf b;
    tz_buf_init(&b);
    if (found) {
      tz_buf_str(&b, drive);
    }
    if (has_path) {
      tz_buf_str(&b, path);
    }
    *out = tz_buf_take(&b);
    if (found) {
      tz_str_release_value(drive);
    }
    if (has_path) {
      tz_str_release_value(path);
    }
  }
#else
  const char *home = env_text("HOME");
  struct passwd *pw = home ? NULL : getpwuid(geteuid());
  *out = str_of_text(home ? home : pw && pw->pw_dir ? pw->pw_dir : "");
#endif
}

#ifdef _WIN32
typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);

#endif

double tz_os_number(int64_t which) {
#ifdef _WIN32
  if (which == 2) {
    return (double)GetTickCount64() / 1000;
  }
  MEMORYSTATUSEX memory;
  memory.dwLength = sizeof memory;
  if (!GlobalMemoryStatusEx(&memory)) {
    return 0;
  }

  return which == 0 ? (double)memory.ullTotalPhys : (double)memory.ullAvailPhys;
#elif defined(__linux__)
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return 0;
  }
  if (which == 2) {
    return (double)info.uptime;
  }
  return (double)(which == 0 ? info.totalram : info.freeram) * info.mem_unit;
#else
  if (which == 2) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec;
  }
  long pages = sysconf(which == 0 ? _SC_PHYS_PAGES : _SC_AVPHYS_PAGES);

  return (double)pages * (double)sysconf(_SC_PAGESIZE);
#endif
}

static int32_t processor_count(void) {
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);

  return info.dwNumberOfProcessors > 0 ? (int32_t)info.dwNumberOfProcessors : 1;
#else
  long n = sysconf(_SC_NPROCESSORS_ONLN);

  return n > 0 ? (int32_t)n : 1;
#endif
}

int32_t tz_os_available_parallelism(void) { return processor_count(); }

#ifdef _WIN32
typedef struct {
  LARGE_INTEGER idle, kernel, user, dpc, interrupt;
  ULONG interrupts;
} ProcessorTimes;

typedef LONG(WINAPI *NtQuerySystemInformationFn)(int, void *, ULONG, ULONG *);
#endif

TzArray *tz_os_cpus(const TzType *element, TzMaker make) {
  int32_t count = processor_count();
  TzArray *cpus = tz_arr_new(element, 0, count);
#ifdef _WIN32
  ProcessorTimes *times = (ProcessorTimes *)tz_alloc(sizeof(ProcessorTimes) * (size_t)count);
  memset(times, 0, sizeof(ProcessorTimes) * (size_t)count);
  NtQuerySystemInformationFn query = (NtQuerySystemInformationFn)(void *)GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
  ULONG got = 0;
  if (query) {
    query(8, times, (ULONG)(sizeof(ProcessorTimes) * (size_t)count), &got);
  }
  for (int32_t i = 0; i < count; ++i) {
    wchar_t key[128];
    swprintf(key, 128, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", (int)i);
    DWORD speed = 0, size = sizeof speed;
    registry_value(key, L"~MHz", RRF_RT_REG_DWORD, &speed, &size);
    wchar_t model[256];
    size = sizeof model;
    if (!registry_value(key, L"ProcessorNameString", RRF_RT_REG_SZ, model, &size)) {
      model[0] = 0;
    }
    TzCpuParts parts;
    parts.model = tz_str_from_utf16((const uint16_t *)model, wcslen(model));
    parts.speed = (double)speed;
    parts.user = (double)(times[i].user.QuadPart / 10000);
    parts.nice = 0;
    parts.sys = (double)((times[i].kernel.QuadPart - times[i].idle.QuadPart) / 10000);
    parts.idle = (double)(times[i].idle.QuadPart / 10000);
    parts.irq = (double)(times[i].interrupt.QuadPart / 10000);
    void *cpu = make(&parts);
    tz_arr_push_value(cpus, &cpu);
  }
  tz_free(times);
#else
  char model[256] = "";
  FILE *info = fopen("/proc/cpuinfo", "r");
  if (info) {
    char line[512];
    while (fgets(line, sizeof line, info)) {
      if (strncmp(line, "model name", 10) == 0) {
        char *colon = strchr(line, ':');
        if (colon) {
          snprintf(model, sizeof model, "%s", colon + 2);
          model[strcspn(model, "\n")] = 0;
        }
        break;
      }
    }
    fclose(info);
  }
  for (int32_t i = 0; i < count; ++i) {
    TzCpuParts parts;
    memset(&parts, 0, sizeof parts);
    parts.model = str_of_text(model);
    void *cpu = make(&parts);
    tz_arr_push_value(cpus, &cpu);
  }
#endif

  return cpus;
}

TzArray *tz_os_loadavg(const TzType *element) {
  double load[3] = {0, 0, 0};
#ifndef _WIN32
  getloadavg(load, 3);
#endif
  TzArray *a = tz_arr_new(element, 0, 3);
  for (int i = 0; i < 3; ++i) {
    tz_arr_push_value(a, &load[i]);
  }

  return a;
}

#ifdef _WIN32
typedef BOOL(WINAPI *GetUserNameFn)(LPWSTR, LPDWORD);
#endif

void *tz_os_user_info(TzMaker make) {
  TzUserInfoParts parts;
  memset(&parts, 0, sizeof parts);
#ifdef _WIN32
  parts.uid = parts.gid = -1;
  HMODULE advapi = LoadLibraryW(L"advapi32.dll");
  GetUserNameFn get = advapi ? (GetUserNameFn)(void *)GetProcAddress(advapi, "GetUserNameW") : NULL;
  wchar_t name[257];
  DWORD size = 257;
  if (!get || !get(name, &size)) {
    size = 1;
  }
  parts.username = tz_str_from_utf16((const uint16_t *)name, size - 1);
  homedir(&parts.homedir);
#else
  struct passwd *pw = getpwuid(geteuid());
  parts.uid = (int32_t)geteuid();
  parts.gid = (int32_t)getegid();
  parts.username = str_of_text(pw && pw->pw_name ? pw->pw_name : "");
  parts.homedir = str_of_text(pw && pw->pw_dir ? pw->pw_dir : "");
  if (pw && pw->pw_shell) {
    parts.shell.value = str_of_text(pw->pw_shell);
    parts.shell.has = 1;
  }
#endif

  return make(&parts);
}
