// Copyright 2026 Notolibra. All rights reserved.
//
// Notolibra Engine Helper: renderer, GPU, utility and other child processes.
// Phase 0 runs them unsandboxed (--no-sandbox), so there is no Seatbelt setup
// here yet; content_shell's helper (content/shell/app/shell_main_mac.cc)
// shows where it goes.

#include "base/allocator/early_zone_registration_apple.h"

extern "C" int ne_helper_main(int argc, const char** argv);

int main(int argc, const char** argv) {
  partition_alloc::EarlyMallocZoneRegistration();
  return ne_helper_main(argc, argv);
}
