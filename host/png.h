// Save the framebuffer as a PNG (greyscale, optionally scaled up). No zlib:
// the image data goes in stored deflate blocks, so files are uncompressed
// but tiny anyway at this size.
#pragma once

#include <stdbool.h>

bool png_write_fb(const char *path, int scale);
