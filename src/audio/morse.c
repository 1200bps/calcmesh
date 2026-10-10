#include "audio/morse.h"

#include <ctype.h>
#include <stddef.h>

// '.' and '-', A-Z then 0-9
static const char *const codes[36] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
    "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
    "..-", "...-", ".--", "-..-", "-.--", "--..",
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----.",
};

int morse_timing(const char *text, int wpm, uint16_t *out, int max) {
    if (wpm < 1)
        wpm = 1;
    uint16_t dot = (uint16_t)(1200 / wpm);
    int n = 0;
    int gap = 0; // dots of silence owed before the next element
    for (; *text; text++) {
        int c = toupper((unsigned char)*text);
        const char *code = isalpha(c) ? codes[c - 'A'] : isdigit(c) ? codes[26 + c - '0'] : NULL;
        if (c == ' ') {
            if (n)
                gap = 7;
            continue;
        }
        if (!code)
            continue;
        if (n && gap < 3)
            gap = 3;
        for (; *code; code++) {
            if (n) {
                if (n + 2 > max)
                    return n;
                out[n++] = (uint16_t)(dot * (gap ? gap : 1));
            } else if (n + 1 > max) {
                return n;
            }
            out[n++] = (uint16_t)(dot * (*code == '-' ? 3 : 1));
            gap = 0;
        }
    }
    return n;
}
