/**
 * @file
 *
 * Compiles the single-header tinyexr library (OpenEXR .exr load/save).
 * ZIP decompression uses miniz; the implementation (miniz.c) is compiled in
 * the io module, so only the miniz.h declarations are vendored here.
 */

#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
