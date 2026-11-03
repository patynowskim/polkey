/*
 * CTAPHID transport layer for the PolKey FIDO2 authenticator (BL616)
 *
 * OUT path (USB IRQ): reassemble 64-byte reports into a full message, hand it
 * to the ctaphid task. Malformed input raises protocol errors that the task
 * sends once the transport is free.
 * IN path (USB IRQ + task): one response at a time, streamed as INIT + CONT
 * packets; the IN-complete callback pushes the next packet.
 */
#include <string.h>
#include "usbd_core.h"
#include "usbd_hid.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_irq.h"
#include "bflb_core.h"
#include "bflb_sec_trng.h"
#include "ctaphid.h"
#include "ctap2.h"
#include "fpga.h"
#include "oled.h"
#include "store.h"
#include "pin.h"
#include "vault.h"
#include "kem_cmd.h"
#include "ctap2_int.h"
#include "dirtyjtag.h"

#define HID_OUT_EP 0x03
#define HID_IN_EP  0x84
#define HID_MPS    64u

#define CTAPHID_MAX_CHANNELS 4

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t out_pkt[HID_MPS];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t in_pkt[HID_MPS];

/* ---------------- channel table ---------------- */
static uint32_t channels[CTAPHID_MAX_CHANNELS];
static uint8_t next_channel_slot;

static bool channel_allocated(uint32_t cid)
{
    if (cid == 0) return false;
    for (uint8_t i = 0; i < CTAPHID_MAX_CHANNELS; i++) {
        if (channels[i] == cid) return true;
    }
    return false;
}

static void channel_remember(uint32_t cid)
{
    for (uint8_t i = 0; i < CTAPHID_MAX_CHANNELS; i++) {
        if (channels[i] == cid) return;
    }
    channels[next_channel_slot] = cid;
    next_channel_slot = (next_channel_slot + 1) % CTAPHID_MAX_CHANNELS;
}

/* ---------------- reassembly (OUT callback context) ---------------- */
static uint8_t msg_buf[CTAPHID_MAX_MSG];
static uint16_t msg_expected;
static uint16_t msg_received;
static uint8_t msg_seq;
static uint8_t msg_cmd;
static uint32_t msg_cid;
static TickType_t msg_last_tick;

/* ---------------- pending message for the task ---------------- */
static uint8_t pending_buf[CTAPHID_MAX_MSG];
static uint16_t pending_len;
static uint8_t pending_cmd;
static uint32_t pending_cid;
static volatile bool pending_ready;

/* ---------------- pending protocol error ---------------- */
static volatile bool err_ready;
static uint32_t err_cid;
static uint8_t err_code;

/* ---------------- response transmission ---------------- */
static uint8_t resp_buf[CTAPHID_MAX_MSG];
static uint16_t resp_len;
static uint16_t resp_off;
static uint8_t resp_seq;
static uint32_t resp_cid;
static uint8_t resp_cmd;
static volatile bool resp_active;
static volatile bool in_busy;

static volatile bool cancel_flag;

bool ctaphid_cancel_requested(void)
{
    return cancel_flag;
}

static void tx_pump_locked(void)
{
    if (!resp_active || in_busy) return;

    memset(in_pkt, 0, HID_MPS);
    in_pkt[0] = (resp_cid >> 24) & 0xFF;
    in_pkt[1] = (resp_cid >> 16) & 0xFF;
    in_pkt[2] = (resp_cid >> 8) & 0xFF;
    in_pkt[3] = resp_cid & 0xFF;

    uint16_t chunk;
    if (resp_off == 0) {
        in_pkt[4] = resp_cmd;
        in_pkt[5] = (resp_len >> 8) & 0xFF;
        in_pkt[6] = resp_len & 0xFF;
        chunk = (resp_len > 57) ? 57 : resp_len;
        memcpy(&in_pkt[7], resp_buf, chunk);
    } else {
        in_pkt[4] = resp_seq & 0x7F;
        uint16_t remaining = resp_len - resp_off;
        chunk = (remaining > 59) ? 59 : remaining;
        memcpy(&in_pkt[5], resp_buf + resp_off, chunk);
        resp_seq++;
    }

    resp_off += chunk;
    if (resp_off >= resp_len) {
        resp_active = false;
    }

    in_busy = true;
    usbd_ep_start_write(0, HID_IN_EP, in_pkt, HID_MPS);
}

void ctaphid_send(uint32_t channel, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    if (len > CTAPHID_MAX_MSG) return;

    uintptr_t flag = bflb_irq_save();
    if (resp_active || in_busy) {
        /* CTAPHID is a serial transport; hosts wait for the previous response. */
        bflb_irq_restore(flag);
        return;
    }
    resp_cid = channel;
    resp_cmd = cmd;
    resp_len = len;
    resp_off = 0;
    resp_seq = 0;
    if (len && data) {
        memcpy(resp_buf, data, len);
    }
    resp_active = true;
    tx_pump_locked();
    bflb_irq_restore(flag);
}

/* ---------------- OUT path ---------------- */
static void queue_error(uint32_t cid, uint8_t code)
{
    err_cid = cid;
    err_code = code;
    err_ready = true;
}

void ctaphid_handle_packet(const uint8_t *b)
{
    TickType_t now = xTaskGetTickCount();

    if (msg_expected && (now - msg_last_tick) > pdMS_TO_TICKS(500)) {
        msg_expected = 0;
        msg_received = 0;
        msg_seq = 0;
    }

    uint32_t cid = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    if (b[4] & 0x80) {
        uint8_t cmd = b[4];

        if (cmd == CTAPHID_CANCEL) {
            cancel_flag = true;
            return;
        }

        msg_cmd = cmd;
        msg_cid = cid;
        msg_expected = ((uint16_t)b[5] << 8) | b[6];
        msg_seq = 0;
        msg_last_tick = now;

        if (msg_expected > CTAPHID_MAX_MSG) {
            msg_expected = 0;
            msg_received = 0;
            queue_error(cid, CTAPHID_ERR_INVALID_LEN);
            return;
        }

        msg_received = (msg_expected > 57) ? 57 : msg_expected;
        memcpy(msg_buf, &b[7], msg_received);

        if (msg_expected == 0) { /* zero-length message (e.g. WINK): complete immediately */
            if (!pending_ready) {
                pending_len = 0;
                pending_cmd = msg_cmd;
                pending_cid = msg_cid;
                pending_ready = true;
            } else {
                queue_error(cid, CTAPHID_ERR_CHANNEL_BUSY);
            }
            return;
        }
    } else {
        if (msg_expected == 0 || cid != msg_cid) return;

        if (b[4] != msg_seq) {
            msg_expected = 0;
            msg_received = 0;
            queue_error(cid, CTAPHID_ERR_INVALID_SEQ);
            return;
        }

        msg_seq++;
        uint16_t remaining = msg_expected - msg_received;
        uint16_t chunk = (remaining > 59) ? 59 : remaining;
        memcpy(msg_buf + msg_received, &b[5], chunk);
        msg_received += chunk;
        msg_last_tick = now;
    }

    if (msg_expected > 0 && msg_received >= msg_expected) {
        if (!pending_ready) {
            memcpy(pending_buf, msg_buf, msg_expected);
            pending_len = msg_expected;
            pending_cmd = msg_cmd;
            pending_cid = msg_cid;
            pending_ready = true;
        } else {
            queue_error(cid, CTAPHID_ERR_CHANNEL_BUSY);
        }
        msg_expected = 0;
        msg_received = 0;
    }
}

/* ---------------- command processing (task context) ---------------- */
static uint32_t random_cid(void)
{
    static uint32_t counter;
    static struct bflb_device_s *trng;
    uint8_t r[32];

    counter++;
    if (trng == NULL) {
        trng = bflb_device_get_by_name("sec_trng");
    }
    if (trng != NULL && bflb_trng_read(trng, r) == 0) {
        uint32_t v = ((uint32_t)r[0]) | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
        v ^= counter;
        if (v != 0 && v != 0xFFFFFFFF) return v;
    }
    return 0x10000000u + counter; /* fallback: unique at least */
}

static void process_message(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    switch (cmd) {
    case CTAPHID_INIT: {
        if (len < 8) {
            uint8_t err = CTAPHID_ERR_INVALID_LEN;
            ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
            return;
        }

        uint8_t resp[17];
        memcpy(resp, data, 8);

        uint32_t new_cid;
        if (cid == 0xFFFFFFFF || !channel_allocated(cid)) {
            new_cid = random_cid();
        } else {
            new_cid = cid;
        }
        channel_remember(new_cid);

        resp[8] = (new_cid >> 24) & 0xFF;
        resp[9] = (new_cid >> 16) & 0xFF;
        resp[10] = (new_cid >> 8) & 0xFF;
        resp[11] = new_cid & 0xFF;
        resp[12] = 0x02; /* CTAPHID protocol version */
        resp[13] = 0x01; /* device version major */
        resp[14] = 0x00; /* device version minor */
        resp[15] = 0x00; /* device version build */
        resp[16] = 0x04; /* capabilities: CBOR */

        ctaphid_send(cid, CTAPHID_INIT, resp, sizeof(resp));
        return;
    }

    case CTAPHID_PING:
        ctaphid_send(cid, CTAPHID_PING, data, len);
        return;

    case CTAPHID_WINK:
        if (!channel_allocated(cid)) {
            uint8_t err = CTAPHID_ERR_INVALID_CHANNEL;
            ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
            return;
        }
        ctaphid_send(cid, CTAPHID_WINK, NULL, 0);
        return;

    case CTAPHID_CANCEL:
        cancel_flag = true;
        return; /* no response */

    case CTAPHID_MSG:
        if (!channel_allocated(cid)) {
            uint8_t err = CTAPHID_ERR_INVALID_CHANNEL;
            ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
            return;
        }
        ctap2_u2f_handle(cid, data, len);
        return;

    case CTAPHID_KEM:
        if (!channel_allocated(cid)) {
            uint8_t err = CTAPHID_ERR_INVALID_CHANNEL;
            ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
            return;
        }
        kem_handle(cid, data, len);
        return;

    case CTAPHID_VAULT:
        if (!channel_allocated(cid)) {
            uint8_t err = CTAPHID_ERR_INVALID_CHANNEL;
            ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
            return;
        }
        vault_handle(cid, data, len);
        return;

    case CTAPHID_CBOR:
        if (!channel_allocated(cid)) {
            uint8_t err = CTAPHID_ERR_INVALID_CHANNEL;
            ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
            return;
        }
        ctap2_handle(cid, data, len);
        return;

    default: {
        uint8_t err = CTAPHID_ERR_INVALID_CMD;
        ctaphid_send(cid, CTAPHID_ERROR, &err, 1);
        return;
    }
    }
}

static void tunnel_reply(void *ctx, const uint8_t *msg, size_t len)
{
    (void)ctx;
    dirtyjtag_vault_reply(msg, len);
}

void ctaphid_task(void *param)
{
    (void)param;

    /* Crypto, credential storage and the FPGA link come up here, after USB:
       the device enumerates even if the accelerator is absent. */
    STAGE(1);
    ctap2_init();
    STAGE(2);
    store_init();
    STAGE(3);
    pin_init();
    STAGE(4);
    vault_init();
    STAGE(5);
    fpga_init();
    STAGE(6);
    oled_show("PolKey", "Ready");

    for (;;) {
        STAGE(7);
        ctap2_ux_housekeeping();
        const uint8_t *vreq;
        size_t vlen;
        if (dirtyjtag_vault_take(&vreq, &vlen)) { /* request from the browser extension (WebUSB tunnel) */
            vault_process(vreq, vlen, tunnel_reply, NULL);
            dirtyjtag_vault_end();
            dirtyjtag_vault_done();
        }
        if (err_ready) {
            err_ready = false;
            uint8_t code = err_code;
            ctaphid_send(err_cid, CTAPHID_ERROR, &code, 1);
        } else if (pending_ready) {
            pending_ready = false;
            cancel_flag = false;
            process_message(pending_cid, pending_cmd, pending_buf, pending_len);
        } else {
            vTaskDelay(1);
        }
    }
}

/* ---------------- USB plumbing ---------------- */
static void hid_out_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    ctaphid_handle_packet(out_pkt);
    usbd_ep_start_read(0, HID_OUT_EP, out_pkt, HID_MPS);
}

static void hid_in_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    uintptr_t flag = bflb_irq_save();
    in_busy = false;
    tx_pump_locked();
    bflb_irq_restore(flag);
}

static struct usbd_endpoint hid_out_ep = {
    .ep_addr = HID_OUT_EP,
    .ep_cb = hid_out_cb,
};

static struct usbd_endpoint hid_in_ep = {
    .ep_addr = HID_IN_EP,
    .ep_cb = hid_in_cb,
};

static struct usbd_interface hid_intf;

/* FIDO HID report descriptor: usage page 0xF1D0, 64-byte input + output */
static const uint8_t fido_report_descriptor[34] = {
    0x06, 0xD0, 0xF1,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x20,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x81, 0x02,
    0x09, 0x21,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x91, 0x02,
    0xC0
};

void ctaphid_usb_init(uint8_t busid)
{
    usbd_hid_init_intf(busid, &hid_intf, fido_report_descriptor, sizeof(fido_report_descriptor));
    usbd_add_interface(busid, &hid_intf);
    usbd_add_endpoint(busid, &hid_out_ep);
    usbd_add_endpoint(busid, &hid_in_ep);
}

void ctaphid_start(void)
{
    /* (Re)configuration after a USB reset or a cable glitch: forget half-finished transfers, otherwise a response
       that was in flight keeps in_busy set forever and every later answer is dropped. */
    uintptr_t flag = bflb_irq_save();
    msg_expected = 0;
    msg_received = 0;
    pending_ready = false;
    err_ready = false;
    resp_active = false;
    in_busy = false;
    cancel_flag = false;
    bflb_irq_restore(flag);
    usbd_ep_start_read(0, HID_OUT_EP, out_pkt, HID_MPS);
}
