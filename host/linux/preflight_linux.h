// SPDX-License-Identifier: MIT
// Linux preflight validator for guest AArch64 ELFs and AOT x86-64 static archives (stage 5D-B1).
// Performs static inspection only: never maps ELFs at guest addresses, never calls loader::load(),
// os_init(), or executes guest code.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace snap_linux {

enum class PreflightStatus {
    Pass = 0,
    Fail = 1,
    Blocked = 2,
};

struct ArtifactCheck {
    std::string label;
    std::string path;
    PreflightStatus status = PreflightStatus::Blocked;
    uint64_t size_bytes = 0;
    std::string detail;
    std::vector<std::string> warnings;
    std::vector<std::string> unprovided_symbols;
};

struct PreflightReport {
    PreflightStatus overall = PreflightStatus::Blocked;
    ArtifactCheck main_elf;
    ArtifactCheck sdk_elf;
    ArtifactCheck main_lib;
    ArtifactCheck sdk_lib;
};

// Validates a guest AArch64 ELF against host/elf_check.cpp and host/loader.cpp mapping constraints.
ArtifactCheck validate_guest_elf(const std::string &path, const char *label, uint64_t base_addr, uint64_t max_span);

// Validates an AOT x86-64 static archive (.a) for ELF64 x86-64 objects, expected tables,
// SNAP_RUNTIME_ABI compatibility, absence of Windows import symbols, and external dependencies.
ArtifactCheck validate_aot_archive(const std::string &path, const char *module_name);

// Runs full preflight across main ELF, sdk ELF, main AOT archive, and sdk AOT archive.
PreflightReport run_preflight(const std::string &main_elf, const std::string &sdk_elf,
                              const std::string &main_lib, const std::string &sdk_lib);

// Prints a structured PASS/WARN/BLOCKED/FAIL report.
void print_preflight_report(const PreflightReport &r, FILE *out);

}  // namespace snap_linux
