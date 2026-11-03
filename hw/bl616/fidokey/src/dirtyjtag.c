/*
 * DirtyJTAG v2 protocol handler
 *
 * Ported from DirtyJTAG/src/cmd.c (MIT license)
 * Adapted for CherryUSB on BL616
 *
 * Processes DirtyJTAG v2 commands received on a vendor bulk endpoint
 * and responds with JTAG data.
 */
#include <string.h>
#include "usbd_core.h"
#include "dirtyjtag.h"
#include "jtag_gpio.h"
#include "ctaphid.h"
#include "bflb_irq.h"
#include "FreeRTOS.h"
#include "task.h"


#define JTAG_OUT_EP  0x01
#define JTAG_IN_EP   0x82

/* DirtyJTAG v2 command IDs */
enum {
    CMD_STOP   = 0x00,
    CMD_INFO   = 0x01,
    CMD_FREQ   = 0x02,
    CMD_XFER   = 0x03,
    CMD_SETSIG = 0x04,
    CMD_GETSIG = 0x05,
    CMD_CLK    = 0x06,
};

/* Command modifiers */
enum {
    NO_READ       = 0x80,
    EXTEND_LENGTH = 0x40,
    READOUT       = 0x80,
};

/* Signal identifiers */
enum {
    SIG_TCK  = 1 << 1,
    SIG_TDI  = 1 << 2,
    SIG_TDO  = 1 << 3,
    SIG_TMS  = 1 << 4,
    SIG_TRST = 1 << 5,
    SIG_SRST = 1 << 6,
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t rx_buf[64];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t tx_buf[64];
volatile uint32_t polkey_stage; /* progress marker of the CTAPHID task, readable with command 0xF2 even if the task hangs */
static volatile bool ep_tx_busy;

/* ---- JSON tunnel for the browser extension (WebUSB): [0xF0][len lo][len hi][payload...] in both directions.
   The message is reassembled here (USB IRQ) and handed to the CTAPHID task, which runs vault_process(). */
#define CMD_VAULT 0xF0      /* request frame / final (empty) answer frame */
#define CMD_STAGE 0xF2      /* debug: report polkey_stage */
#define CMD_VAULT_MORE 0xF1 /* answer frame with a message, more frames follow */
static uint8_t vt_rx[CTAPHID_MAX_MSG];
static size_t vt_rx_len, vt_rx_got;
static volatile bool vt_rx_active, vt_pending;
static uint8_t vt_tx[CTAPHID_MAX_MSG + 3];
static size_t vt_tx_len, vt_tx_off;
static volatile bool vt_tx_active;
static size_t vt_skip; /* bytes of a rejected message still to swallow so they are not taken for JTAG commands */

static void vt_pump(void)
{
    if (vt_tx_off >= vt_tx_len) {
        vt_tx_active = false;
        return;
    }
    size_t n = vt_tx_len - vt_tx_off;
    if (n > 64) n = 64;
    memcpy(tx_buf, vt_tx + vt_tx_off, n);
    vt_tx_off += n;
    ep_tx_busy = true;
    usbd_ep_start_write(0, JTAG_IN_EP, tx_buf, n);
}

bool dirtyjtag_vault_take(const uint8_t **msg, size_t *len)
{
    if (!vt_pending) return false;
    *msg = vt_rx;
    *len = vt_rx_len;
    return true;
}

void dirtyjtag_vault_done(void)
{
    vt_pending = false;
}

/* Each reply is one frame: 0xF1 = a message, more may follow; 0xF0 (empty) closes the answer. Waits for the
   previous frame to leave so that long answers (backup export) stream without a big buffer. */
static bool vt_wait_idle(void)
{
    for (int i = 0; i < 3000 && vt_tx_active; i++) vTaskDelay(1);
    return !vt_tx_active;
}

static void vt_send_frame(uint8_t type, const uint8_t *msg, size_t len)
{
    if (!vt_wait_idle()) return; /* the host stopped reading */
    uintptr_t f = bflb_irq_save();
    vt_tx[0] = type;
    vt_tx[1] = (uint8_t)len;
    vt_tx[2] = (uint8_t)(len >> 8);
    memcpy(vt_tx + 3, msg, len);
    vt_tx_len = len + 3;
    vt_tx_off = 0;
    vt_tx_active = true;
    vt_pump();
    bflb_irq_restore(f);
}

void dirtyjtag_vault_reply(const uint8_t *msg, size_t len)
{
    if (len > CTAPHID_MAX_MSG) len = 0;
    vt_send_frame(CMD_VAULT_MORE, msg, len);
}

void dirtyjtag_vault_end(void)
{
    vt_send_frame(CMD_VAULT, NULL, 0);
}

static void vt_rx_packet(const uint8_t *p, uint32_t n)
{
    if (!vt_rx_active) { /* first packet: header + start of the payload */
        vt_rx_len = (size_t)p[1] | ((size_t)p[2] << 8);
        vt_rx_got = 0;
        p += 3;
        n -= 3;
        if (vt_rx_len > sizeof(vt_rx) || vt_pending) { /* oversized or previous message still in work: drop */
            vt_skip = vt_rx_len > n ? vt_rx_len - n : 0;
            return;
        }
        vt_rx_active = true;
    }
    size_t room = vt_rx_len - vt_rx_got;
    if (n > room) n = (uint32_t)room;
    memcpy(vt_rx + vt_rx_got, p, n);
    vt_rx_got += n;
    if (vt_rx_got == vt_rx_len) {
        vt_rx_active = false;
        vt_pending = true;
    }
}

static void cmd_handle(const uint8_t *rxbuf, uint32_t count)
{
    const uint8_t *commands = rxbuf;
    uint8_t *output_buffer = tx_buf;

    while ((commands < (rxbuf + count)) && (*commands != CMD_STOP)) {
        switch ((*commands) & 0x0F) {
        case CMD_INFO: {
            char info_string[10] = "DJTAG2\n";
            memcpy(output_buffer, info_string, 10);
            output_buffer += 10;
            break;
        }

        case CMD_FREQ:
            jtag_set_frequency((commands[1] << 8) | commands[2]);
            commands += 2;
            break;

        case CMD_XFER: {
            bool no_read = *commands & NO_READ;
            uint16_t transferred_bits = commands[1];
            if (*commands & EXTEND_LENGTH)
                transferred_bits += 256;
            /* Cap at 62*8 = 496 bits to avoid buffer overflow */
            if (transferred_bits > 62 * 8)
                transferred_bits = 62 * 8;

            uint16_t trbytes = (transferred_bits + 7) / 8;

            if (!no_read) {
                memset(output_buffer, 0, trbytes);
            }

            jtag_transfer(transferred_bits, commands + 2, no_read ? NULL : output_buffer);

            commands += 1 + trbytes;
            output_buffer += (no_read ? 0 : trbytes);
            break;
        }

        case CMD_SETSIG: {
            uint8_t signal_mask = commands[1];
            uint8_t signal_status = commands[2];

            if (signal_mask & SIG_TCK)
                jtag_set_tck(signal_status & SIG_TCK);
            if (signal_mask & SIG_TDI)
                jtag_set_tdi(signal_status & SIG_TDI);
            if (signal_mask & SIG_TMS)
                jtag_set_tms(signal_status & SIG_TMS);
            if (signal_mask & SIG_TRST)
                jtag_set_trst(signal_status & SIG_TRST);
            if (signal_mask & SIG_SRST)
                jtag_set_srst(signal_status & SIG_SRST);
            commands += 2;
            break;
        }

        case CMD_GETSIG: {
            uint8_t signal_status = 0;
            if (jtag_get_tdo())
                signal_status |= SIG_TDO;
            output_buffer[0] = signal_status;
            output_buffer += 1;
            break;
        }

        case CMD_CLK: {
            bool readout = !!(*commands & READOUT);
            uint8_t signals = commands[1];
            uint8_t clk_pulses = commands[2];

            bool readout_val = jtag_strobe(clk_pulses, signals & SIG_TMS, signals & SIG_TDI);
            if (readout) {
                output_buffer[0] = readout_val ? 0xFF : 0;
                output_buffer += 1;
            }
            commands += 2;
            break;
        }

        default:
            goto done;
        }

        commands++;
    }

done:
    /* Send response if there's data */
    if (output_buffer != tx_buf) {
        ep_tx_busy = true;
        usbd_ep_start_write(0, JTAG_IN_EP, tx_buf, output_buffer - tx_buf);
    }
}

static void jtag_bulk_out_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    if (nbytes >= 1 && rx_buf[0] == CMD_STAGE && !vt_rx_active && !vt_skip) {
        uint32_t st = polkey_stage;
        tx_buf[0] = CMD_STAGE;
        tx_buf[1] = (uint8_t)st;
        tx_buf[2] = (uint8_t)(st >> 8);
        tx_buf[3] = (uint8_t)(st >> 16);
        tx_buf[4] = (uint8_t)(st >> 24);
        ep_tx_busy = true;
        usbd_ep_start_write(0, JTAG_IN_EP, tx_buf, 5);
    } else if (vt_skip) {
        vt_skip = vt_skip > nbytes ? vt_skip - nbytes : 0;
    } else if (vt_rx_active) {
        vt_rx_packet(rx_buf, nbytes);
    } else if (nbytes >= 3 && rx_buf[0] == CMD_VAULT) {
        vt_rx_packet(rx_buf, nbytes);
    } else {
        cmd_handle(rx_buf, nbytes);
    }

    /* Re-arm OUT endpoint for next command */
    usbd_ep_start_read(0, JTAG_OUT_EP, rx_buf, sizeof(rx_buf));
}

static void jtag_bulk_in_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;
    ep_tx_busy = false;
    if (vt_tx_active) vt_pump();
}

static struct usbd_endpoint jtag_out_ep = {
    .ep_addr = JTAG_OUT_EP,
    .ep_cb = jtag_bulk_out_cb,
};

static struct usbd_endpoint jtag_in_ep = {
    .ep_addr = JTAG_IN_EP,
    .ep_cb = jtag_bulk_in_cb,
};

static struct usbd_interface jtag_intf;

void dirtyjtag_init(uint8_t busid)
{
    usbd_add_interface(busid, &jtag_intf);
    usbd_add_endpoint(busid, &jtag_out_ep);
    usbd_add_endpoint(busid, &jtag_in_ep);
}

void dirtyjtag_start(void)
{
    vt_rx_active = false;     /* same reason as in ctaphid_start(): drop transfers interrupted by a USB reset */
    vt_skip = 0;
    vt_pending = false;
    vt_tx_active = false;
    ep_tx_busy = false;
    usbd_ep_start_read(0, JTAG_OUT_EP, rx_buf, sizeof(rx_buf));
}
