#include "edgemesh/core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int em_validate_response(const char *data, size_t length)
{
    const char *end = NULL, *p, *line_end;
    size_t header_size, body_size, expected = 0;
    int have_length = 0, status;
    if (!data || length < 12 || length > EM_MAX_RESPONSE) return -1;
    for (p = data; (size_t)(data + length - p) >= 4; ++p)
        if (!memcmp(p, "\r\n\r\n", 4)) { end = p + 4; break; }
    if (!end) return -1;
    header_size = (size_t)(end - data);
    if (header_size > EM_MAX_HEADERS) return -1;
    for (line_end = data; (size_t)(end - line_end) >= 2; ++line_end)
        if (line_end[0] == '\r' && line_end[1] == '\n') break;
    if ((size_t)(end - line_end) < 2 ||
        (strncmp(data, "HTTP/1.0 ", 9) && strncmp(data, "HTTP/1.1 ", 9)) ||
        (size_t)(line_end - data) < 13 || data[12] != ' ' ||
        !isdigit((unsigned char)data[9]) || !isdigit((unsigned char)data[10]) || !isdigit((unsigned char)data[11])) return -1;
    status = (data[9] - '0') * 100 + (data[10] - '0') * 10 + (data[11] - '0');
    if (status < 200 || status > 599) return -1;
    for (const char *q = data + 13; q < line_end; ++q)
        if ((unsigned char)*q < 32 || (unsigned char)*q == 127) return -1;
    p = line_end + 2;
    while (p < end - 2) {
        const char *e, *colon;
        for (e = p; (size_t)(end - e) >= 2; ++e)
            if (e[0] == '\r' && e[1] == '\n') break;
        if ((size_t)(end - e) < 2) return -1;
        if (e == p) break;
        colon = memchr(p, ':', (size_t)(e - p));
        if (!colon || colon == p || *p == ' ' || *p == '\t') return -1;
        for (const char *q = p; q < colon; ++q)
            if (!isalnum((unsigned char)*q) && !strchr("!#$%&'*+-.^_`|~", *q)) return -1;
        for (const char *q = colon + 1; q < e; ++q)
            if (((unsigned char)*q < 32 && *q != '\t') || (unsigned char)*q == 127) return -1;
        if ((size_t)(e - p) >= 15 && !strncasecmp(p, "Content-Length:", 15)) {
            const char *value = p + 15; char *tail; unsigned long parsed;
            while (value < e && (*value == ' ' || *value == '\t')) ++value;
            if (have_length++) return -1;
            parsed = strtoul(value, &tail, 10);
            if (tail == value || parsed > EM_MAX_RESPONSE) return -1;
            while (tail < e && (*tail == ' ' || *tail == '\t')) ++tail;
            if (tail != e) return -1;
            expected = (size_t)parsed;
        } else if ((size_t)(e - p) >= 18 && !strncasecmp(p, "Transfer-Encoding:", 18)) return -1;
        p = e + 2;
    }
    body_size = length - header_size;
    return have_length && body_size != expected ? -1 : 0;
}
