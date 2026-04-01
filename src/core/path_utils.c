#include "core/path_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void sanitize_stream_name(const char *input, char *output, size_t output_size) {
    size_t i = 0;
    for (; i < output_size - 1 && input[i] != '\0'; i++) {
        // Make sure non-ASCII values are converted to positive inputs to ctype functions
        unsigned char c = (unsigned char) input[i];
        // Allowed characters: [A-Za-z0-9_\-]
        if (isalpha(c) || isdigit(c) || c == '-') {
            output[i] = c;
        } else {
            output[i] = '_';
        }
    }
    output[i] = '\0';
}
