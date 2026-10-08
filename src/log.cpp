#include "log.h"
#include <stdarg.h>
#include <freertos/semphr.h>

static const int SCREEN_LINES = 8;
static const int LINE_LEN     = 72;

static SemaphoreHandle_t lock;
static char     lines[SCREEN_LINES][LINE_LEN];
static int      lineHead  = 0;   // index of the oldest line
static int      lineCount = 0;
static uint32_t version = 0, shownVersion = 0;
static volatile bool usbCdc = true;

void log_begin()
{
    lock = xSemaphoreCreateMutex();
    Serial.begin(115200);
    USBSerial.begin();
    USBSerial.setTxTimeoutMs(0); // never block when no terminal is attached
}

void log_disable_usb_cdc()
{
    usbCdc = false;
}

static void addLine(const char *s)
{
    int idx = (lineHead + lineCount) % SCREEN_LINES;
    if (lineCount == SCREEN_LINES)
        lineHead = (lineHead + 1) % SCREEN_LINES;
    else
        lineCount++;
    strlcpy(lines[idx], s, LINE_LEN);
    version++;
}

void logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;

    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t ms = millis();
    Serial.printf("[%6lu.%03lu] %s\n", ms / 1000, ms % 1000, buf);
    if (usbCdc)
        USBSerial.printf("[%6lu.%03lu] %s\n", ms / 1000, ms % 1000, buf);
    addLine(buf);
    xSemaphoreGive(lock);
}

void log_hex(const char *prefix, const uint8_t *data, size_t len)
{
    // 16 bytes per line keeps lines short enough for the screen
    char buf[16 * 3 + 1];
    for (size_t off = 0; off < len; off += 16) {
        size_t n = min((size_t)16, len - off);
        for (size_t i = 0; i < n; i++)
            snprintf(buf + i * 3, 4, "%02X ", data[off + i]);
        logf("%s%03u: %s", prefix, (unsigned)off, buf);
    }
}

bool log_screen_text(char *buf, size_t size)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = version != shownVersion;
    if (changed) {
        shownVersion = version;
        size_t pos = 0;
        buf[0] = 0;
        for (int i = 0; i < lineCount && pos < size; i++) {
            pos += snprintf(buf + pos, size - pos, "%s%s", i ? "\n" : "",
                            lines[(lineHead + i) % SCREEN_LINES]);
        }
    }
    xSemaphoreGive(lock);
    return changed;
}
