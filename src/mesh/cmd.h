// Text commands for driving the mesh layer from a console: the firmware's USB
// console and meshcat share them.
#pragma once

#include <stdbool.h>

// Run one command line (modified in place). Returns false, with the line as
// it was, if the first word isn't one of ours, so the caller can try its own
// commands.
bool mesh_cmd_run(char *line);

// Print a line per command.
void mesh_cmd_help(void);
