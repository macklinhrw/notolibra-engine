// Copyright 2026 Notolibra. All rights reserved.
//
// Notolibra Engine Helper: renderer, GPU, utility and other child processes.
// Each starts in the macOS sandbox Chromium chose for its type: the browser
// process sends the Seatbelt policy (sandbox::SeatbeltExecClient), and the
// helper applies it here, before any of the engine's own code runs. This
// follows content_shell's helper (content/shell/app/shell_main_mac.cc).
//
// The framework is loaded with dlopen after EarlyMallocZoneRegistration, as
// content_shell and Chrome do, not linked: in non-component builds the
// framework carries PartitionAlloc's allocator shim, whose static
// initializers register its malloc zone as the framework loads, and the
// early registration aborts if that already happened.

#ifdef UNSAFE_BUFFERS_BUILD
// C strings from dyld, dlerror and dirname, as in content_shell's helper.
#pragma allow_unsafe_buffers
#endif

#include <dlfcn.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>

#include <cstdint>
#include <memory>

#include "base/allocator/early_zone_registration_apple.h"
#include "sandbox/mac/seatbelt_exec.h"

using HelperMainPtr = int (*)(int, const char**);

int main(int argc, const char** argv) {
  partition_alloc::EarlyMallocZoneRegistration();

  uint32_t exec_path_size = 0;
  if (_NSGetExecutablePath(nullptr, &exec_path_size) != -1) {
    fprintf(stderr, "_NSGetExecutablePath: get length failed\n");
    abort();
  }
  std::unique_ptr<char[]> exec_path(new char[exec_path_size]);
  if (_NSGetExecutablePath(exec_path.get(), &exec_path_size) != 0) {
    fprintf(stderr, "_NSGetExecutablePath: get path failed\n");
    abort();
  }
  sandbox::SeatbeltExecServer::CreateFromArgumentsResult seatbelt =
      sandbox::SeatbeltExecServer::CreateFromArguments(
          exec_path.get(), argc, const_cast<char**>(argv));
  if (seatbelt.sandbox_required) {
    if (!seatbelt.server) {
      fprintf(stderr, "Failed to create the seatbelt sandbox server.\n");
      abort();
    }
    if (!seatbelt.server->InitializeSandbox()) {
      fprintf(stderr, "Failed to initialize the sandbox.\n");
      abort();
    }
  }
  // NotolibraEngine.framework/Versions/A/Helpers/<helper>.app/Contents/MacOS
  // is four levels below the framework's binary, Versions/A/NotolibraEngine.
  const char* parent_dir = dirname(exec_path.get());
  if (!parent_dir) {
    fprintf(stderr, "dirname failed\n");
    abort();
  }
  char framework_path[4096];
  snprintf(framework_path, sizeof(framework_path), "%s/../../../../NotolibraEngine", parent_dir);
  void* library = dlopen(framework_path, RTLD_LAZY | RTLD_LOCAL | RTLD_FIRST);
  if (!library) {
    fprintf(stderr, "dlopen %s: %s\n", framework_path, dlerror());
    abort();
  }
  const auto helper_main = reinterpret_cast<HelperMainPtr>(dlsym(library, "ne_helper_main"));
  if (!helper_main) {
    fprintf(stderr, "dlsym ne_helper_main: %s\n", dlerror());
    abort();
  }
  // exit, not return: keeps main in stack traces under tail calls.
  exit(helper_main(argc, argv));
}
