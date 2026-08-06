/* Atlas - entry point.
 * Copyright 2026 The Atlas Authors. Licensed under the Apache License 2.0.
 */
#include <stdio.h>

#include "atlas/cli.h"

int main(int argc, char **argv) {
    return atlas_cli_main(argc, argv, stdout, stderr);
}
