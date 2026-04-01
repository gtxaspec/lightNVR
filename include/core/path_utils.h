#ifndef LIGHTNVR_PATH_UTILS_H
#define LIGHTNVR_PATH_UTILS_H

#include <stddef.h>

/**
 * Sanitize a stream name for use as a path or object name.
 * Replaces non-alphanumeric characters with underscores and lowercases.
 *
 * @param input The input string
 * @param output The output buffer
 * @param output_size The size of the output buffer
 */
void sanitize_stream_name(const char *input, char *output, size_t output_size);

#endif /* LIGHTNVR_PATH_UTILS_H */