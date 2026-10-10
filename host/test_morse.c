// Morse timing for the message chimes.
#include <stdio.h>
#include <string.h>

#include "audio/morse.h"

static int failures;

static void check(const char *text, int wpm, const uint16_t *want, int nwant) {
    uint16_t got[64];
    int n = morse_timing(text, wpm, got, 64);
    if (n != nwant || (n && memcmp(got, want, sizeof *got * (size_t)n))) {
        fprintf(stderr, "morse \"%s\" at %d wpm: got", text, wpm);
        for (int i = 0; i < n; i++)
            fprintf(stderr, " %u", got[i]);
        fprintf(stderr, "\n");
        failures++;
    }
}

int main(void) {
    // At 20 wpm a dot is 60 ms
    check("C", 20, (const uint16_t[]){180, 60, 60, 60, 180, 60, 60}, 7);
    check("RR", 20, (const uint16_t[]){60, 60, 180, 60, 60, 180, 60, 60, 180, 60, 60}, 11);
    check("e e", 20, (const uint16_t[]){60, 420, 60}, 3);
    check("5", 12, (const uint16_t[]){100, 100, 100, 100, 100, 100, 100, 100, 100}, 9);
    check(" ?!", 20, NULL, 0);
    // Cut short, it still ends on a key-down
    uint16_t out[4];
    if (morse_timing("C", 20, out, 4) != 3) {
        fprintf(stderr, "short buffer\n");
        failures++;
    }
    if (failures)
        return 1;
    printf("morse: ok\n");
    return 0;
}
