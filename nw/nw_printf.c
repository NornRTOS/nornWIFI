/**
 * @file nw_printf.c
 * Formatting and logging for nornWIF and for Espressif's libraries, which call sprintf,
 * puts, their own *_printf entry points and the adapter's _log_write. Our own
 * implementation keeps newlib's printf family, its reentrancy data and its heap use out
 * of the image: %d %i %u %x %X %o %c %s %p %% with flags - + space 0 #, width, precision,
 * and the length modifiers h hh l ll z. Floating point prints as <f>.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "rtos.h"
#include "uart.h"
#include "nw_internal.h"


/*
 * Formatting
 */

typedef struct out
{
    char *buf;
    size_t n;                   /**< Capacity including the terminator. */
    size_t len;                 /**< Characters produced (as if unlimited). */
} out_t;

static void put(out_t *o, char c)
{
    if(o->len + 1 < o->n)
        o->buf[o->len] = c;
    ++ o->len;
}

static void putPadded(out_t *o, const char *s, size_t len, int width, bool left, char pad)
{
    size_t i;
    if(!left)
        while(width > 0  &&  (size_t)width > len) { put(o, pad); -- width; }
    for(i=0; i<len; ++i)
        put(o, s[i]);
    if(left)
        while(width > 0  &&  (size_t)width > len) { put(o, ' '); -- width; }
}

int nw_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    out_t o = { buf, n, 0 };
    char tmp[24];

    while(*fmt != 0)
    {
        bool left = false, zero = false, plus = false, space = false, alt = false;
        int width = 0, prec = -1, lenMod = 0;    /* lenMod: 0 int, 1 long, 2 long long, -1 short, -2 char */
        char c = *fmt++;

        if(c != '%') { put(&o, c); continue; }
        for(;;)
        {
            c = *fmt++;
            if(c == '-') left = true;
            else if(c == '0') zero = true;
            else if(c == '+') plus = true;
            else if(c == ' ') space = true;
            else if(c == '#') alt = true;
            else break;
        }
        if(c == '*') { width = va_arg(ap, int); if(width < 0) { left = true; width = -width; } c = *fmt++; }
        else while(c >= '0'  &&  c <= '9') { width = width * 10 + (c - '0'); c = *fmt++; }
        if(c == '.')
        {
            prec = 0;
            c = *fmt++;
            if(c == '*') { prec = va_arg(ap, int); c = *fmt++; }
            else while(c >= '0'  &&  c <= '9') { prec = prec * 10 + (c - '0'); c = *fmt++; }
        }
        for(;;)
        {
            if(c == 'l') { ++ lenMod; c = *fmt++; }
            else if(c == 'h') { -- lenMod; c = *fmt++; }
            else if(c == 'z'  ||  c == 'j'  ||  c == 't') { lenMod = 1; c = *fmt++; }
            else break;
        }
        switch(c)
        {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p':
        {
            unsigned long long v;
            bool neg = false;
            const bool isSigned = c == 'd'  ||  c == 'i';
            unsigned base = (c == 'x'  ||  c == 'X'  ||  c == 'p') ? 16u : c == 'o' ? 8u : 10u;
            const char *digits = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            size_t len = 0, numLen;
            char prefix[3];
            size_t prefixLen = 0;

            if(c == 'p') { v = (uintptr_t)va_arg(ap, void *); alt = true; lenMod = 0; }
            else if(lenMod >= 2) { long long sv = va_arg(ap, long long); if(isSigned  &&  sv < 0) { neg = true; v = (unsigned long long)(-(sv + 1)) + 1u; } else v = (unsigned long long)sv; }
            else if(lenMod == 1) { long sv = va_arg(ap, long); if(isSigned  &&  sv < 0) { neg = true; v = (unsigned long)(-(sv + 1)) + 1u; } else v = (unsigned long)sv; }
            else { int sv = va_arg(ap, int); if(isSigned) { if(sv < 0) { neg = true; v = (unsigned)(-(sv + 1)) + 1u; } else v = (unsigned)sv; }
                   else { unsigned uv = (unsigned)sv; if(lenMod == -1) uv &= 0xFFFFu; else if(lenMod == -2) uv &= 0xFFu; v = uv; } }
            do { tmp[len++] = digits[v % base]; v /= base; } while(v != 0  &&  len < sizeof(tmp));
            while(prec > 0  &&  len < (size_t)prec  &&  len < sizeof(tmp)) tmp[len++] = '0';
            if(neg) prefix[prefixLen++] = '-';
            else if(isSigned  &&  plus) prefix[prefixLen++] = '+';
            else if(isSigned  &&  space) prefix[prefixLen++] = ' ';
            if(alt  &&  base == 16) { prefix[prefixLen++] = '0'; prefix[prefixLen++] = c == 'X' ? 'X' : 'x'; }
            numLen = len + prefixLen;
            if(!left  &&  zero  &&  prec < 0)
                while(width > 0  &&  (size_t)width > numLen) { tmp[len++] = '0'; ++ numLen; if(len >= sizeof(tmp)) break; }
            {
                char all[32];
                size_t i, k = 0;
                for(i=0; i<prefixLen; ++i) all[k++] = prefix[i];
                while(len > 0) all[k++] = tmp[--len];
                putPadded(&o, all, k, width, left, ' ');
            }
            break;
        }
        case 'c':
        {
            char ch = (char)va_arg(ap, int);
            putPadded(&o, &ch, 1, width, left, ' ');
            break;
        }
        case 's':
        {
            const char *s = va_arg(ap, const char *);
            size_t len;
            if(s == NULL) s = "(null)";
            len = strlen(s);
            if(prec >= 0  &&  len > (size_t)prec) len = (size_t)prec;
            putPadded(&o, s, len, width, left, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            (void)va_arg(ap, double);
            putPadded(&o, "<f>", 3, width, left, ' ');
            break;
        case '%':
            put(&o, '%');
            break;
        case 0:
            fmt --;
            break;
        default:
            put(&o, '%');
            put(&o, c);
            break;
        }
    }
    if(o.n > 0)
        o.buf[o.len < o.n ? o.len : o.n - 1] = 0;
    return (int)o.len;
}

int nw_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = nw_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* The libc names, so that the radio libraries (and newlib's headers) resolve to these. */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return nw_vsnprintf(buf, n, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = nw_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return nw_vsnprintf(buf, 0x7FFFFFFFu, fmt, ap); }
int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = nw_vsnprintf(buf, 0x7FFFFFFFu, fmt, ap);
    va_end(ap);
    return r;
}


/*
 * Output
 */

static rtos_sem_t _uartSem = RTOS_SEM_INIT_BINARY(1);
static bool _uartSemReady = false;

static void outputLine(const char *s)
{
    const bool lock = !nw_inIsr()  &&  _uartSemReady;
    if(lock)
        rtos_semTake(&_uartSem, RTOS_FOREVER);
    uart_puts(s);
    if(lock)
        rtos_semGive(&_uartSem);
}

void nw_puts(const char *s)
{
    outputLine(s);
}

void nw_logv(int level, const char *tag, const char *fmt, va_list ap)
{
    static const char levels[] = "?EWIDV";
    char line[160];
    size_t k;

    if(level < 1  ||  level > NW_CFG_LOG_LEVEL)
        return;
    k = (size_t)nw_snprintf(line, sizeof(line), "%c (%lu) %s: ", levels[level > 5 ? 0 : level],
                            (unsigned long)(rtos_getTick() * 1000ul / RTOS_CFG_TICK_HZ), tag);
    if(k >= sizeof(line) - 4) k = sizeof(line) - 4;
    k += (size_t)nw_vsnprintf(line + k, sizeof(line) - 3 - k, fmt, ap);
    if(k > sizeof(line) - 3) k = sizeof(line) - 3;
    while(k > 0  &&  (line[k - 1] == '\n'  ||  line[k - 1] == '\r'))
        -- k;
    line[k++] = '\r';
    line[k++] = '\n';
    line[k] = 0;
    outputLine(line);
}

/** The libraries format their own prefix ("I (123) wifi: ") into the text they hand to
    _log_write, so this prints the text as it is, up to and including its newline. */
void nw_logRawv(int level, const char *fmt, va_list ap)
{
    char line[160];
    size_t k;

    if(level < 1  ||  level > NW_CFG_LOG_LEVEL)
        return;
    k = (size_t)nw_vsnprintf(line, sizeof(line) - 3, fmt, ap);
    if(k > sizeof(line) - 4) k = sizeof(line) - 4;
    if(k > 0  &&  line[k - 1] == '\n')
    {
        -- k;
        line[k++] = '\r';
        line[k++] = '\n';
    }
    line[k] = 0;
    outputLine(line);
}

void nw_log(int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nw_logv(level, tag, fmt, ap);
    va_end(ap);
}

/** Called once a task context exists (nw_osi_init). */
void nw_printf_init(void)
{
    _uartSemReady = true;
}

/* ESP-IDF's log API, used by the adapter table and the glue. */
void esp_log_write(unsigned int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    (void)tag;
    va_start(ap, fmt);
    nw_logRawv((int)level, fmt, ap);
    va_end(ap);
}

void esp_log_writev(unsigned int level, const char *tag, const char *fmt, va_list ap)
{
    (void)tag;
    nw_logRawv((int)level, fmt, ap);
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)(rtos_getTick() * 1000ul / RTOS_CFG_TICK_HZ);
}

/* The libraries' own printf entry points: info level under their tag. */
#define LIB_PRINTF(name, tag)                                                   \
    int name(const char *fmt, ...)                                              \
    {                                                                           \
        va_list ap;                                                             \
        va_start(ap, fmt);                                                      \
        nw_logv(3, tag, fmt, ap);                                               \
        va_end(ap);                                                             \
        return 0;                                                               \
    }
LIB_PRINTF(pp_printf, "pp")
LIB_PRINTF(core_printf, "core")
LIB_PRINTF(net80211_printf, "net80211")
LIB_PRINTF(phy_printf, "phy")
LIB_PRINTF(rtc_printf, "rtc")
LIB_PRINTF(coexist_printf, "coexist")
LIB_PRINTF(sc_printf, "smartconfig")
LIB_PRINTF(mesh_printf, "mesh")
LIB_PRINTF(wapi_printf, "wapi")

int puts(const char *s)
{
    nw_log(3, "lib", "%s", s);
    return 0;
}

int putchar(int c)
{
    uart_putc((char)c);
    return c;
}

int esp_rom_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nw_logRawv(3, fmt, ap);
    va_end(ap);
    return 0;
}
