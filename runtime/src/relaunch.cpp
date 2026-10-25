#include "relaunch.h"

#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cwchar>
#else
#include <spawn.h>
#include <unistd.h>

#include <climits>
#include <fstream>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

#include <rex/logging.h>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace azura {

namespace {

#if defined(_WIN32)

// Environment variable through which a restarted game receives the handle of the process it waits
// for: its value is the handle's number, valid in the new process because it is inherited.
constexpr wchar_t kWaitVariable[] = L"AZURA_RELAUNCH_WAIT";

// How long the new process waits for the previous one at most. It normally exits within seconds;
// one that hangs while closing does not keep the new one from ever starting.
constexpr DWORD kWaitTimeoutMs = 30000;

#else

// Path of the running executable and its arguments (without the program name), or an empty path
// when they cannot be found.
std::string Executable(std::vector<std::string>& arguments) {
#if defined(__APPLE__)
  char path[PATH_MAX] = {0};
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) != 0) return {};
  const int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
  return path;
#elif defined(__linux__)
  char path[PATH_MAX] = {0};
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0) return {};
  // /proc/self/cmdline holds the arguments separated by zero bytes, the program name first.
  std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
  size_t start = all.find('\0');
  while (start != std::string::npos && start + 1 < all.size()) {
    const size_t end = all.find('\0', start + 1);
    arguments.push_back(all.substr(start + 1, end - start - 1));
    start = end;
  }
  return std::string(path, static_cast<size_t>(length));
#else
  (void)arguments;
  return {};
#endif
}

#endif

}  // namespace

bool StartedByLauncher() {
  const char* value = std::getenv("AZURA_LAUNCHER");
  return value && std::string(value) == "1";
}

#if defined(_WIN32)

void RelaunchAfterExit() {
  std::wstring executable(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  if (length == 0 || length >= executable.size()) {
    REXLOG_ERROR("[Azura] Cannot restart: the executable path is unknown");
    return;
  }
  executable.resize(length);

  // A handle to this process that the new one inherits and waits on until this one has exited.
  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE,
                       TRUE, 0)) {
    REXLOG_ERROR("[Azura] Cannot restart: DuplicateHandle failed ({})", GetLastError());
    return;
  }

  // The new process inherits that handle only, not every inheritable handle this one has open.
  SIZE_T attributes_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
  std::vector<char> attributes_buffer(attributes_size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_buffer.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size) ||
      !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self), nullptr,
                                 nullptr)) {
    REXLOG_ERROR("[Azura] Cannot restart: cannot limit the inherited handles ({})", GetLastError());
    CloseHandle(self);
    return;
  }

  // Same command line (program name and arguments, quoted as they came), working folder and
  // environment, plus the variable naming the handle. CreateProcessW may write to the command line,
  // so it gets a copy.
  std::wstring command_line = GetCommandLineW();
  const std::wstring handle_value = std::to_wstring(reinterpret_cast<std::uintptr_t>(self));
  SetEnvironmentVariableW(kWaitVariable, handle_value.c_str());
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                      EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                                      &process);
  const DWORD error = started ? 0 : GetLastError();
  SetEnvironmentVariableW(kWaitVariable, nullptr);
  DeleteProcThreadAttributeList(attributes);
  CloseHandle(self);

  if (!started) {
    REXLOG_ERROR("[Azura] Cannot restart: CreateProcessW failed ({})", error);
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  REXLOG_INFO("[Azura] The game starts again once this process exits");
}

void WaitForRelaunchingProcess() {
  wchar_t value[32] = {};
  const DWORD length = GetEnvironmentVariableW(kWaitVariable, value, static_cast<DWORD>(std::size(value)));
  if (length == 0 || length >= std::size(value)) return;
  // Programs this game starts do not inherit it.
  SetEnvironmentVariableW(kWaitVariable, nullptr);
  const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(value, nullptr, 10)));
  WaitForSingleObject(handle, kWaitTimeoutMs);
  CloseHandle(handle);
}

#else

void RelaunchAfterExit() {
  std::vector<std::string> arguments;
  const std::string executable = Executable(arguments);
  if (executable.empty()) {
    REXLOG_ERROR("[Azura] Cannot restart: the executable path is unknown");
    return;
  }
  // A shell waits for this process to exit, then runs the executable with the same arguments:
  //   sh -c '<script>' <pid> <executable> <arguments...>
  const std::string pid = std::to_string(getpid());
  const char* script = "while kill -0 \"$0\" 2>/dev/null; do sleep 0.2; done; exec \"$@\"";
  std::vector<const char*> argv = {"/bin/sh", "-c", script, pid.c_str(), executable.c_str()};
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  argv.push_back(nullptr);

  pid_t child = 0;
  const int error =
      posix_spawn(&child, "/bin/sh", nullptr, nullptr, const_cast<char* const*>(argv.data()), environ);
  if (error != 0) {
    REXLOG_ERROR("[Azura] Cannot restart: posix_spawn failed ({})", error);
  } else {
    REXLOG_INFO("[Azura] The game starts again once this process exits");
  }
}

// The shell RelaunchAfterExit starts already waits for the previous process.
void WaitForRelaunchingProcess() {}

#endif

}  // namespace azura
