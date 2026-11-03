#include "oled.h"
#include "fpga.h"
#include "font5x7.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define W 128
#define PAGES 4
#define CHAR_W 6
#define LINE_CHARS 40

static char lines[2][LINE_CHARS + 1];
static bool dirty = true, inited;
static TickType_t shown_at, retry_at;
static uint8_t fb[W * PAGES];

static const uint8_t init_seq[] = {
    0xAE, 0xD5, 0x80, 0xA8, 0x1F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x02,
    0x81, 0x8F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
};

void oled_show(const char *l1, const char *l2)
{
    char n1[LINE_CHARS + 1], n2[LINE_CHARS + 1];
    strncpy(n1, l1 ? l1 : "", LINE_CHARS); n1[LINE_CHARS] = 0;
    strncpy(n2, l2 ? l2 : "", LINE_CHARS); n2[LINE_CHARS] = 0;
    if (!strcmp(n1, lines[0]) && !strcmp(n2, lines[1])) return;
    strcpy(lines[0], n1); strcpy(lines[1], n2);
    shown_at = xTaskGetTickCount();
    dirty = true;
    oled_tick();
}

static void glyph_row(int row, const char *s, int xoff)
{
    /* row 0 = pages 0..1 (y 0..15), row 1 = pages 2..3; the 7 px glyph is drawn at 2x height inside a 16 px band */
    for (int ci = 0; s[ci]; ci++) {
        char ch = s[ci];
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *g = &font5x7[(ch - 32) * 5];
        for (int cx = 0; cx < 5; cx++) {
            int x = xoff + ci * CHAR_W + cx;
            if (x < 0 || x >= W) continue;
            uint16_t col = 0;                                       /* stretch 7 rows to 14 (each row twice) */
            for (int b = 0; b < 7; b++) if (g[cx] & (1u << b)) col |= (uint16_t)(3u << (2 * b));
            fb[(row * 2) * W + x] = (uint8_t)col;
            fb[(row * 2 + 1) * W + x] = (uint8_t)(col >> 8);
        }
    }
}

static bool push(void)
{
    static const uint8_t addr[] = { 0x21, 0x00, 0x7F, 0x22, 0x00, 0x03 };
    return fpga_oled_write(0x02, addr, sizeof(addr)) && fpga_oled_write(0x03, fb, sizeof(fb));
}

static bool init_display(void)
{
    if (!fpga_pf_supported()) return false;
    if (!fpga_oled_write(0x04, NULL, 0)) return false;              /* RES low */
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!fpga_oled_write(0x06, NULL, 0)) return false;              /* RES high */
    vTaskDelay(pdMS_TO_TICKS(5));
    return fpga_oled_write(0x02, init_seq, sizeof(init_seq));       /* DC = 0: commands */
}

void oled_tick(void)
{
    if (!inited) {
        if (retry_at && (int32_t)(xTaskGetTickCount() - retry_at) < 0) return;
        if (!init_display()) { retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000); return; }
        inited = true; dirty = true;
    }
    int len0 = (int)strlen(lines[0]) * CHAR_W, len1 = (int)strlen(lines[1]) * CHAR_W;
    int over = (len0 > W ? len0 - W : 0), over1 = (len1 > W ? len1 - W : 0);
    bool scrolling = over > 0 || over1 > 0;
    if (!dirty && !scrolling) return;
    int x0 = 0, x1 = 0;
    if (scrolling) {                                                /* ping-pong scroll: pause, move 1 px / tick, pause, back */
        uint32_t t = (uint32_t)(xTaskGetTickCount() - shown_at) / pdMS_TO_TICKS(40);
        uint32_t pause = 12;
        int maxo = over > over1 ? over : over1;
        uint32_t cyc = 2 * (pause + (uint32_t)maxo);
        uint32_t m = t % cyc;
        int pos = m < pause ? 0 : m < pause + (uint32_t)maxo ? (int)(m - pause) : m < 2 * pause + (uint32_t)maxo ? maxo : maxo - (int)(m - 2 * pause - (uint32_t)maxo);
        x0 = -(pos < over ? pos : over);
        x1 = -(pos < over1 ? pos : over1);
    }
    memset(fb, 0, sizeof(fb));
    glyph_row(0, lines[0], x0);
    glyph_row(1, lines[1], x1);
    if (push()) dirty = false; else { inited = false; retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(3000); }
}
