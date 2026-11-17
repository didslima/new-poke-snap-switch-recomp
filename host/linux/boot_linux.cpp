// SPDX-License-Identifier: MIT
// Linux runner executable entry point (stage 5B).
#include "runner_linux.h"

int main(int argc, char **argv) {
    return snap_linux::bootstrap_main(argc, argv);
}
