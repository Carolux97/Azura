#include "relaunch.h"

#include <spawn.h>
#include <unistd.h>

#include <climits>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

#include <rex/logging.h>

extern char** environ;

namespace azura {

namespace {

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

}  // namespace

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

}  // namespace azura
