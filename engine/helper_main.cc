// Copyright 2026 Notolibra. All rights reserved.
//
// Notolibra Engine Helper: renderer, GPU, utility and other child processes.
// Each starts in the macOS sandbox Chromium chose for its type: the browser
// process sends the Seatbelt policy (sandbox::SeatbeltExecClient), and the
// helper applies it here, before any of the engine's own code runs. This
// follows content_shell's helper (content/shell/app/shell_main_mac.cc).

#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>

#include <cstdint>
#include <memory>

#include "base/allocator/early_zone_registration_apple.h"
#include "sandbox/mac/seatbelt_exec.h"

extern "C" int ne_helper_main(int argc, const char** argv);

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
  return ne_helper_main(argc, argv);
}
