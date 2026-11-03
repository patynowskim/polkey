/* SSD1306 128x32 OLED behind the FPGA SPI bridge (fpga_oled_write); text only, 5x7 font, two lines */
#ifndef OLED_H
#define OLED_H

void oled_show(const char *line1, const char *line2);  /* no-op without display / bridge */
void oled_tick(void);                                  /* call regularly (~50 ms): initialises on demand, scrolls long lines, refreshes */

#endif
