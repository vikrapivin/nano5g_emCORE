//
//
//    Copyright 2010 TheSeven
//
//
//    This file is part of emCORE.
//
//    emCORE is free software: you can redistribute it and/or
//    modify it under the terms of the GNU General Public License as
//    published by the Free Software Foundation, either version 2 of the
//    License, or (at your option) any later version.
//
//    emCORE is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//    See the GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License along
//    with emCORE.  If not, see <http://www.gnu.org/licenses/>.
//
//


#include "global.h"
#include "malloc.h"
#include "usbdebug.h"
#include "panic.h"
#include "usb.h"
#include "thread.h"
#include "console.h"
#include "util.h"
#include "contextswitch.h"
#include "power.h"
#include "mmu.h"
#include "shutdown.h"
#include "execimage.h"
#ifdef HAVE_I2C
#include "i2c.h"
#endif
#ifdef HAVE_BOOTFLASH
#include "bootflash.h"
#endif
#ifdef HAVE_HWKEYAES
#include "hwkeyaes.h"
#ifdef HAVE_HWKEYAES_EXT
#include "target/ipodnano5g/hwkeyaes_ext.h"
#endif
#endif
#ifdef HAVE_HMACSHA1
#include "hmacsha1.h"
#endif
#ifdef USB_HAVE_TARGET_SPECIFIC_REQUESTS
#include "usbtarget.h"
#endif
#ifdef HAVE_STORAGE
#include "storage.h"
#include "disk.h"
#include "file.h"
#include "dir.h"
#include "libc/include/errno.h"
#endif

/* NAND_POC_DISABLE (make NAND_POC_DISABLE=1): no NAND worker, no NAND storage, commands 120..127 unsupported */
#if defined(TARGET_ipodnano4g) && !defined(NAND_POC_DISABLE)
#define HAVE_NAND_POC
#include "target/ipodnano4g/nand_poc.h"
#endif

/* nano 5G: read-only NAND harness (target/ipodnano5g/nand5g.c), commands 120..127; make N5G_NAND_DISABLE=1 removes it */
#if defined(TARGET_ipodnano5g) && !defined(N5G_NAND_DISABLE)
#define HAVE_NAND5G
#include "target/ipodnano5g/nand5g.h"
#endif

/* WEDGE-FIX-2026-09-20 (USB only, default off): runtime gate for the EP0 tolerance
   fixes in this file (bit4, bit6) and in synopsysotg.c (bits 0..3). Set with CMD 68. */
uint32_t wedge_fix_enable = 0;

/* M1.3 (bit6) EP0 reply-pipeline bound. */
#define WEDGE_REPLY_WATCHDOG_MAX 100000u
static volatile uint32_t dbg_reply_midflight;
static uint32_t dbg_reply_watchdog;

/* ---- USB DEBUG COMMAND NUMBERS (must match tools/usb_cmds.py) ----
   The sync switch below is matched FIRST; any number not in it falls through to
   the async dbgthread switch (stock emCORE monitor commands). Retired numbers of
   the removed NAND code (see usb_cmds.py RETIRED) must never be reused. */
enum {
    CMD_GETINFO       = 1,   /* sub 0=VERSION, 1=USER MEM */
    CMD_READ_MEM      = 4,
    CMD_WRITE_MEM     = 5,
    CMD_READ_CONSOLE  = 10,
    CMD_WRITE_CONSOLE = 11,
    CMD_GETPROCINFO   = 15,
    CMD_FREEZE        = 16,
    CMD_SUSPEND       = 17,
    CMD_KILL          = 18,
    CMD_CREATETHREAD  = 19,
    CMD_FLUSHCACHE    = 20,
    CMD_WEDGE_FIX     = 68,   /* set wedge-fix bitmask (buf[1]=new mask, dbgbuf[1]=old) */
    CMD_RESET         = 104,  /* sync RESET */
    /* nandlib proof of concept (target/ipodnano4g/nand_poc.c, ).
       Every command only queues a job for the NAND worker thread (or reads state);
       nothing touches NAND in IRQ context. */
    CMD_NAND_STATUS        = 120,
    CMD_NAND_STEP          = 121,
    CMD_NAND_READ_RESERVED = 122,
    CMD_NAND_READ_PAGE     = 123,
    CMD_NAND_FTL_OPEN      = 124,
    CMD_NAND_FTL_READ      = 125,
    CMD_NAND_GET_STRUCT    = 126,
    CMD_NAND_WATCHDOG      = 127,  /* (seconds), 0 = disarm; reports "unknown register semantics" */
    CMD_NAND_WRITE_INFO    = 128,  /* P2b (NAND_POC_WRITE build): {flags, staging addr, staging bytes} */
    CMD_NAND_SNAPSHOT      = 129,
    CMD_NAND_OPEN_RW       = 130,  /* 130..136 need the confirmation token in word 3 (nand_poc_token) */
    CMD_NAND_ERASE_BLOCK   = 131,
    CMD_NAND_PROGRAM_PAGE  = 132,
    CMD_NAND_FTL_WRITE     = 133,
    CMD_NAND_FTL_FLUSH     = 134,
    CMD_NAND_BOOTAREA_WR   = 135,
    CMD_NAND_STORAGE_WR    = 136,
};


static uint32_t dbgbuf[16] CACHEALIGN_ATTR;


enum dbgstate_t
{
    DBGSTATE_IDLE = 0,
    DBGSTATE_SETUP,
    DBGSTATE_WRITE_MEM,
    DBGSTATE_WRITE_CONSOLE,
    DBGSTATE_ASYNC,
    DBGSTATE_RESPOND,
    DBGSTATE_READ_CONSOLE,
};

static struct scheduler_thread dbgthread_handle IBSS_ATTR;
static uint32_t dbgstack[0x200] STACK_ATTR;
struct wakeup dbgwakeup IBSS_ATTR;
static bool dbgenabled IBSS_ATTR;
static bool dbgbusy IBSS_ATTR;
static const struct usb_instance* dbgusb IBSS_ATTR;
static enum dbgstate_t dbgstate IBSS_ATTR;
static void* dbgmemaddr IBSS_ATTR;
static uint32_t dbgmemlen IBSS_ATTR;
static char dbgconsendbuf[4096];
static char dbgconrecvbuf[1024];
static int dbgconsendreadidx IBSS_ATTR;
static int dbgconsendwriteidx IBSS_ATTR;
static int dbgconrecvreadidx IBSS_ATTR;
static int dbgconrecvwriteidx IBSS_ATTR;
static struct wakeup dbgconsendwakeup IBSS_ATTR;
static struct wakeup dbgconrecvwakeup IBSS_ATTR;
static bool dbgconsoleattached IBSS_ATTR;
static int maxpacket IBSS_ATTR;
static struct bulk_state
{
    void* addr;
    int size;
} bulk_state[2] IBSS_ATTR;
static int bulk_ctrlreq_ep IBSS_ATTR;

/* Monitor-worker liveness heartbeat, bumped once per dbgthread() loop (async cmd 200). */
volatile uint32_t dbg_monitor_heartbeat IBSS_ATTR;

static const char dbgconoverflowstr[] = "\n\n[overflowed]\n\n";

extern int _poolstart;   // These aren't ints at all, but gcc complains about void types being
extern int _poolend;     // used here, and we only need the address, so just make it happy...


void reset() __attribute__((noreturn));

void usbdebug_enable(const struct usb_instance* data, int interface, int altsetting)
{
    dbgusb = data;
    dbgstate = DBGSTATE_IDLE;
    dbgenabled = true;
}

void usbdebug_disable(const struct usb_instance* data, int interface, int altsetting)
{
    dbgenabled = false;
    dbgstate = DBGSTATE_IDLE;
}

void usbdebug_bus_reset(const struct usb_instance* data, int configuration, int interface, int highspeed)
{
    maxpacket = highspeed ? 512 : 64;
}

void usbdebug_bulk_enable(const struct usb_instance* data, int interface, int altsetting)
{
    usbdebug_enable(data, interface, altsetting);
    union usb_endpoint_number outep = { .number = USBDEBUG_ENDPOINT_OUT, .direction = USB_ENDPOINT_DIRECTION_OUT };
    union usb_endpoint_number inep = { .number = USBDEBUG_ENDPOINT_IN, .direction = USB_ENDPOINT_DIRECTION_IN };
    usb_configure_ep(data, outep, USB_ENDPOINT_TYPE_BULK, maxpacket);
    usb_configure_ep(data, inep, USB_ENDPOINT_TYPE_BULK, maxpacket);
}

void usbdebug_bulk_disable(const struct usb_instance* data, int interface, int altsetting)
{
    union usb_endpoint_number outep = { .number = USBDEBUG_ENDPOINT_OUT, .direction = USB_ENDPOINT_DIRECTION_OUT };
    union usb_endpoint_number inep = { .number = USBDEBUG_ENDPOINT_IN, .direction = USB_ENDPOINT_DIRECTION_IN };
    usb_unconfigure_ep(data, outep);
    usb_unconfigure_ep(data, inep);
    usbdebug_disable(data, interface, altsetting);
}

void usbdebug_bulk_xfer_complete(const struct usb_instance* data, int interface, int endpoint, int bytesleft)
{
    struct bulk_state* state = &bulk_state[endpoint];
    if (!bytesleft && state->size)
    {
        int size;
        if (endpoint)
        {
            union usb_endpoint_number ep = { .number = USBDEBUG_ENDPOINT_IN, .direction = USB_ENDPOINT_DIRECTION_IN };
            size = MIN(state->size, maxpacket * usb_get_max_transfer_size(data, ep));
            usb_start_tx(data, ep, state->addr, size);
        }
        else
        {
            union usb_endpoint_number ep = { .number = USBDEBUG_ENDPOINT_OUT, .direction = USB_ENDPOINT_DIRECTION_OUT };
            size = MIN(state->size, maxpacket * usb_get_max_transfer_size(data, ep));
            usb_start_rx(data, ep, state->addr, size);
        }
        state->addr += size;
        state->size -= size;
    }
}

bool usbdebug_bulk_handle_data(const struct usb_instance* data, union usb_endpoint_number epnum, int bytesleft)
{
    uint32_t* buf = (uint32_t*)data->buffer->raw;
    union usb_endpoint_number ep0out = { .number = 0, .direction = USB_ENDPOINT_DIRECTION_OUT };
    union usb_endpoint_number ep0in = { .number = 0, .direction = USB_ENDPOINT_DIRECTION_IN };
    usb_ep0_start_rx(data, false, 0, NULL);
    switch (buf[0])
    {
    case 1:  // START MEMORY TRANSFER
        bulk_state[bulk_ctrlreq_ep].addr = (void*)buf[1];
        bulk_state[bulk_ctrlreq_ep].size = buf[2];
        usb_set_stall(data, ep0out, true);
        usb_ep0_start_tx(data, NULL, 0, NULL);
        usbdebug_bulk_xfer_complete(data, 0, bulk_ctrlreq_ep, 0);  // Convenient way to start a transfer.
        break;
    default:
        usb_set_stall(data, ep0out, true);
        usb_set_stall(data, ep0in, true);
        break;
    }
    return true;
}

int usbdebug_bulk_ctrl_request(const struct usb_instance* data, int interface, int endpoint, union usb_ep0_buffer* request, const void** response)
{
    int size = -1;
    union usb_endpoint_number ep0out = { .number = 0, .direction = USB_ENDPOINT_DIRECTION_OUT };
    switch (request->setup.bmRequestType.type)
    {
    case USB_SETUP_BMREQUESTTYPE_TYPE_VENDOR:
        switch (request->setup.bRequest.raw)
        {
        case 0x00:
            switch (data->buffer->setup.bmRequestType.direction)
            {
            case USB_SETUP_BMREQUESTTYPE_DIRECTION_OUT:
                bulk_ctrlreq_ep = endpoint;
                usb_ep0_start_rx(data, true, 64, usbdebug_bulk_handle_data);
                return -3;
            default: break;
            }
            break;
        default: break;
        }
        break;
        default: break;
    }
    return size;
}

bool usbdebug_handle_data(const struct usb_instance* data, union usb_endpoint_number epnum, int bytesleft)
{
    union usb_endpoint_number ep0in = { .number = 0, .direction = USB_ENDPOINT_DIRECTION_IN };
    union usb_endpoint_number ep0out = { .number = 0, .direction = USB_ENDPOINT_DIRECTION_OUT };
    uint32_t* buf = (uint32_t*)data->buffer->raw;
    usb_ep0_start_rx(dbgusb, false, 0, NULL);
    switch (dbgstate)
    {
    case DBGSTATE_SETUP:
        switch (buf[0])
        {
        case 1:  // GET INFO
            dbgbuf[0] = 1;
            switch (buf[1])
            {
            case 0:  // GET VERSION INFO
                dbgbuf[1] = VERSION_SVN_INT;
                dbgbuf[2] = VERSION_MAJOR | (VERSION_MINOR << 8) | (VERSION_PATCH << 16) | (2 << 24);
                dbgbuf[3] = PLATFORM_ID;
                break;
            case 1:  // GET USER MEMORY INFO
                dbgbuf[1] = (uint32_t)&_poolstart;
                dbgbuf[2] = (uint32_t)&_poolend;
                break;
            default:
                dbgbuf[0] = 2;
            }
            break;
        case 4:  // READ MEMORY
            dbgbuf[0] = 1;
            dbgmemaddr = (void*)buf[1];
            dbgmemlen = buf[2];
            break;
        case 5:  // WRITE MEMORY
        {
            dbgmemaddr = (void*)buf[1];
            dbgmemlen = buf[2];
            int len = MIN(48, dbgmemlen);
            dbgmemlen -= len;
            memcpy(dbgmemaddr, &buf[4], len);
            if (dbgmemlen)
            {
                dbgmemaddr += len;
                dbgstate = DBGSTATE_WRITE_MEM;
                usb_ep0_start_rx(dbgusb, true, MIN(64, dbgmemlen), usbdebug_handle_data);
                return true;
            }
            dbgbuf[0] = 1;
            break;
        }
        case CMD_WEDGE_FIX:  // SET WEDGE-FIX BITMASK (buf[1]=new mask), USB only
            dbgbuf[0] = 1;
            dbgbuf[1] = wedge_fix_enable;   /* return OLD mask */
            wedge_fix_enable = buf[1];
            break;
#ifdef HAVE_NAND5G
        case N5G_MODULE_INFO:
            n5g_module_command(buf, dbgbuf);
            break;
        case N5G_NAND_STATUS:  // nano 5G read-only NAND harness: {1, &n5g_nand_status, size, result buffer}; others queue a job
        case N5G_NAND_STEP:
        case N5G_NAND_READ_ROM:
        case N5G_NAND_READ_PAGE:
        case N5G_NAND_FTL_OPEN:
        case N5G_NAND_FTL_READ:
        case N5G_NAND_GET_STRUCT:
        case N5G_NAND_WRITE_CAPS:
        case N5G_NAND_WRITE_OPEN:
        case N5G_NAND_BLOCK_WRITE:
        case N5G_NAND_FLUSH:
        case N5G_NAND_BLOCK_READ:
            n5g_nand_usb_command(buf, dbgbuf);
            break;
#endif
#ifdef HAVE_NAND_POC
        case CMD_NAND_STATUS:  // nandlib PoC: {1, &nand_poc_status, size, result buffer}; others queue a job
        case CMD_NAND_STEP:
        case CMD_NAND_READ_RESERVED:
        case CMD_NAND_READ_PAGE:
        case CMD_NAND_FTL_OPEN:
        case CMD_NAND_FTL_READ:
        case CMD_NAND_GET_STRUCT:
        case CMD_NAND_WATCHDOG:
        case CMD_NAND_WRITE_INFO:
        case CMD_NAND_SNAPSHOT:
        case CMD_NAND_OPEN_RW:
        case CMD_NAND_ERASE_BLOCK:
        case CMD_NAND_PROGRAM_PAGE:
        case CMD_NAND_FTL_WRITE:
        case CMD_NAND_FTL_FLUSH:
        case CMD_NAND_BOOTAREA_WR:
        case CMD_NAND_STORAGE_WR:
        {
            const void* rep = NULL;
            uint32_t replen = 0;
            nand_poc_usb_command(buf, dbgbuf, &rep, &replen);
            dbgmemaddr = (void*)rep;
            dbgmemlen = replen;
            break;
        }
#endif
        case 10:  // READ CONSOLE
            dbgmemaddr = (void*)buf[1];
            dbgstate = DBGSTATE_READ_CONSOLE;
            usb_set_stall(dbgusb, ep0out, true);
            usb_ep0_start_tx(dbgusb, NULL, 0, NULL);
            return true;
            break;
        case 11:  // WRITE CONSOLE
        {
            int total = 0;
            int bytes = dbgconrecvreadidx - dbgconrecvwriteidx - 1;
            if (bytes < 0) bytes += sizeof(dbgconrecvbuf);
            if (bytes)
            {
                if (bytes > buf[1]) bytes = buf[1];
                total = bytes;
                if (bytes > 48) bytes = 48;
                int writebytes = bytes;
                char* readptr = (char*)&buf[4];
                if (dbgconrecvwriteidx + bytes >= sizeof(dbgconrecvbuf))
                {
                    writebytes = sizeof(dbgconrecvbuf) - dbgconrecvwriteidx;
                    memcpy(&dbgconrecvbuf[dbgconrecvwriteidx], readptr, writebytes);
                    dbgconrecvwriteidx = 0;
                    readptr = &readptr[writebytes];
                    writebytes = bytes - writebytes;
                }
                if (writebytes) memcpy(&dbgconrecvbuf[dbgconrecvwriteidx], readptr, writebytes);
                dbgconrecvwriteidx += writebytes;
                wakeup_signal(&dbgconrecvwakeup);
            }
            dbgbuf[0] = 1;
            dbgbuf[1] = total;
            dbgbuf[2] = sizeof(dbgconrecvbuf);
            if (total > 48)
            {
                dbgmemlen = total - 48;
                dbgstate = DBGSTATE_WRITE_CONSOLE;
                usb_ep0_start_rx(dbgusb, true, MIN(64, dbgmemlen), usbdebug_handle_data);
                return true;
            }
            dbgbuf[3] = dbgconrecvreadidx - dbgconrecvwriteidx - 1;
            break;
        }
        case 15:  // GET PROCESS INFO
            dbgbuf[0] = 1;
            dbgbuf[1] = SCHEDULER_THREAD_INFO_VERSION;
            dbgbuf[2] = (uint32_t)head_thread;
            break;
        case 16:  // FREEZE SCHEDULER
            dbgbuf[1] = scheduler_freeze(buf[1]);
            scheduler_switch(NULL, NULL);
            dbgbuf[0] = 1;
            break;
        case 17:  // SUSPEND THREAD
            if (buf[1])
            {
                if (thread_suspend((struct scheduler_thread*)(buf[2])) == ALREADY_SUSPENDED)
                    dbgbuf[1] = 1;
                else dbgbuf[1] = 0;
            }
            else
            {
                if (thread_resume((struct scheduler_thread*)(buf[2])) == ALREADY_RESUMED)
                    dbgbuf[1] = 0;
                else dbgbuf[1] = 1;
            }
            dbgbuf[0] = 1;
            break;
        case 18:  // KILL THREAD
            thread_terminate((struct scheduler_thread*)(buf[1]));
            dbgbuf[0] = 1;
            break;
        case 19:  // CREATE THREAD
            dbgbuf[0] = 1;
            dbgbuf[1] = (uint32_t)thread_create(NULL, (const char*)(buf[1]), (const void*)(buf[2]),
                                                (char*)(buf[3]), buf[4], (enum thread_type)buf[5],
                                                buf[6], buf[7], (void*)buf[8], (void*)buf[9],
                                                (void*)buf[10], (void*)buf[11]);
            break;
        case 20:  // FLUSH CACHE
            clean_dcache();
            invalidate_icache();
            buf[0] = 1;
            break;
        case CMD_RESET:  // RESET
            if (!buf[1]) reset();
        default:
            if (!dbgbusy)
            {
                memcpy(dbgbuf, buf, 64);
                dbgstate = DBGSTATE_ASYNC;
                dbgbusy = 1;
                wakeup_signal(&dbgwakeup);
                return true;
            }
            buf[0] = 3;
            break;
        }
        break;
    case DBGSTATE_WRITE_MEM:
    {
        int len = MIN(64 - bytesleft, dbgmemlen);
        dbgmemlen -= len;
        memcpy(dbgmemaddr, buf, len);
        if (dbgmemlen && !bytesleft)
        {
            dbgmemaddr += len;
            usb_ep0_start_rx(dbgusb, true, MIN(64, dbgmemlen), usbdebug_handle_data);
            return true;
        }
        dbgbuf[0] = 1;
        break;
    }
    case DBGSTATE_WRITE_CONSOLE:
    {
        int bytes = MIN(64 - bytesleft, dbgmemlen);
        dbgmemlen -= bytes;
        if (bytes)
        {
            int writebytes = bytes;
            char* readptr = (char*)buf;
            if (dbgconrecvwriteidx + bytes >= sizeof(dbgconrecvbuf))
            {
                writebytes = sizeof(dbgconrecvbuf) - dbgconrecvwriteidx;
                memcpy(&dbgconrecvbuf[dbgconrecvwriteidx], readptr, writebytes);
                dbgconrecvwriteidx = 0;
                readptr = &readptr[writebytes];
                writebytes = bytes - writebytes;
            }
            if (writebytes) memcpy(&dbgconrecvbuf[dbgconrecvwriteidx], readptr, writebytes);
            dbgconrecvwriteidx += writebytes;
            wakeup_signal(&dbgconrecvwakeup);
            if (dbgmemlen && !bytesleft)
            {
                usb_ep0_start_rx(dbgusb, true, MIN(64, dbgmemlen), usbdebug_handle_data);
                return true;
            }
        }
        dbgbuf[3] = dbgconrecvreadidx - dbgconrecvwriteidx - 1;
        if (dbgbuf[3] < 0) dbgbuf[3] += sizeof(dbgconrecvbuf);
    }
    default: break;
    }
    dbgstate = DBGSTATE_RESPOND;
    /* M1.1 (bit4): EP0 tolerant reply + NO persistent stall.  The stock path
       asserts a persistent STALL on ep0out at every normal command completion;
       with bit4 we skip that so the OTG pipe does not accumulate stall residue
       across the sustained fire+readback that wedges the device (~450-3343). */
    if (!(wedge_fix_enable & (1u<<4))) usb_set_stall(dbgusb, ep0out, true);
    usb_ep0_start_tx(dbgusb, NULL, 0, NULL);   /* FIX-GLM4: revert DSFLASH2 - restore the NULL callback (bootROM never STALLs EP0 in the normal path; wr/rd idempotent 500k with the NULL callback) */
    return true;
}

bool read_console_callback(const struct usb_instance* data, union usb_endpoint_number epnum, int bytesleft)
{
    if (bytesleft || !dbgmemaddr)
    {
        usb_ep0_start_rx(dbgusb, true, 0, usb_ep0_ack_callback);
        if (!dbgmemaddr) usb_ep0_start_tx(dbgusb, NULL, 0, usb_ep0_short_tx_callback);
        dbgusb->state->ep0_tx_ptr = NULL;
        return false;
    }
    dbgconsoleattached = true;
    int bytes = MIN(64, dbgmemlen);
    if (bytes)
    {
        int readbytes = bytes;
        char* outptr = (char*)dbgbuf;
        if (dbgconsendreadidx + bytes >= sizeof(dbgconsendbuf))
        {
            readbytes = sizeof(dbgconsendbuf) - dbgconsendreadidx;
            memcpy(outptr, &dbgconsendbuf[dbgconsendreadidx], readbytes);
            dbgconsendreadidx = 0;
            outptr = &outptr[readbytes];
            readbytes = bytes - readbytes;
        }
        if (readbytes) memcpy(outptr, &dbgconsendbuf[dbgconsendreadidx], readbytes);
        dbgconsendreadidx += readbytes;
        wakeup_signal(&dbgconsendwakeup);
        dbgmemlen -= bytes;
    }
    bytes = MIN(64, (int)dbgmemaddr);
    dbgmemaddr -= bytes;
    usb_ep0_start_tx(dbgusb, dbgbuf, bytes,
                     bytes < 64 ? usb_ep0_short_tx_callback : read_console_callback);
    return true;
}

int usbdebug_handle_setup(const struct usb_instance* data, int interface, union usb_ep0_buffer* request, const void** response)
{
    int size = -1;
    union usb_endpoint_number ep0out = { .number = 0, .direction = USB_ENDPOINT_DIRECTION_OUT };
    switch (request->setup.bmRequestType.type)
    {
    case USB_SETUP_BMREQUESTTYPE_TYPE_VENDOR:
        switch (request->setup.bRequest.raw)
        {
        case 0x00:
            switch (data->buffer->setup.bmRequestType.direction)
            {
            case USB_SETUP_BMREQUESTTYPE_DIRECTION_OUT:
                /* M1.3 (bit6): do NOT let a new SETUP abandon a pending
                   multi-stage 16KB reply.  If a reply is mid-flight, defer
                   (keep-busy, return -3, leave dbgstate=RESPOND so the host
                   can resume reading the pending reply) and bump a BOUNDED
                   watchdog; past the bound we force-clear and let the fresh
                   command through (no infinite wedge). */
                if ((wedge_fix_enable & (1u<<6)) && dbg_reply_midflight) {
                    if (++dbg_reply_watchdog < WEDGE_REPLY_WATCHDOG_MAX)
                        return -3;
                    dbg_reply_midflight = 0;
                    dbg_reply_watchdog = 0;
                }
                dbgstate = DBGSTATE_SETUP;
                dbgmemlen = 0;
                usb_ep0_start_rx(dbgusb, true, 64, usbdebug_handle_data);
                return -3;
            case USB_SETUP_BMREQUESTTYPE_DIRECTION_IN:
                switch (dbgstate)
                {
                case DBGSTATE_RESPOND:
                {
                    dbgmemlen = MIN(dbgmemlen, data->buffer->setup.wLength - 16);
                    int len = MIN(48, dbgmemlen);
                    if (len) memcpy(&dbgbuf[4], dbgmemaddr, len);
                    dbgmemlen -= len;
                    if (dbgmemlen)
                    {
                        /* M1.3 (bit6): mark a chained multi-stage reply as
                           mid-flight so a new SETUP cannot abandon it. */
                        dbg_reply_midflight = 1;
                        dbg_reply_watchdog = 0;
                        usb_ep0_start_rx(dbgusb, false, 0, NULL);
                        dbgusb->state->ep0_tx_ptr = dbgmemaddr + 48;
                        dbgusb->state->ep0_tx_len = dbgmemlen;
                        usb_ep0_start_tx(dbgusb, dbgbuf, len + 16,
                                         len < 48 ? usb_ep0_short_tx_callback : usb_ep0_tx_callback);
                        return -3;
                    }
                    dbg_reply_midflight = 0;   /* reply fully delivered */
                    dbg_reply_watchdog = 0;
                    dbgstate = DBGSTATE_IDLE;
                    *response = dbgbuf;
                    return len + 16;
                }
                case DBGSTATE_READ_CONSOLE:
                {
                    dbgmemaddr = (void*)MIN((int)dbgmemaddr, data->buffer->setup.wLength - 16);
                    dbgconsoleattached = true;
                    dbgmemlen = dbgconsendwriteidx - dbgconsendreadidx;
                    if (dbgmemlen < 0) dbgmemlen += sizeof(dbgconsendbuf);
                    int used = dbgmemlen;
                    if (dbgmemlen > (int)dbgmemaddr) dbgmemlen = (int)dbgmemaddr;
                    int bytes = MIN(48, dbgmemlen);
                    dbgbuf[0] = 1;
                    dbgbuf[1] = dbgmemlen;
                    dbgbuf[2] = sizeof(dbgconsendbuf);
                    dbgbuf[3] = used - dbgmemlen;
                    if (bytes)
                    {
                        int readbytes = bytes;
                        char* outptr = (char*)&dbgbuf[4];
                        if (dbgconsendreadidx + bytes >= sizeof(dbgconsendbuf))
                        {
                            readbytes = sizeof(dbgconsendbuf) - dbgconsendreadidx;
                            memcpy(outptr, &dbgconsendbuf[dbgconsendreadidx], readbytes);
                            dbgconsendreadidx = 0;
                            outptr = &outptr[readbytes];
                            readbytes = bytes - readbytes;
                        }
                        if (readbytes) memcpy(outptr, &dbgconsendbuf[dbgconsendreadidx], readbytes);
                        dbgconsendreadidx += readbytes;
                        wakeup_signal(&dbgconsendwakeup);
                    }
                    dbgmemlen -= bytes;
                    bytes = MIN(48, (int)dbgmemaddr);
                    dbgmemaddr -= bytes;
                    if (dbgmemaddr)
                    {
                        dbgusb->state->ep0_tx_ptr = (void*)true;
                        usb_ep0_start_rx(dbgusb, false, 0, NULL);
                        usb_ep0_start_tx(dbgusb, dbgbuf, bytes + 16,
                                         bytes < 48 ? usb_ep0_short_tx_callback : read_console_callback);
                        return -3;
                    }
                    dbgstate = DBGSTATE_IDLE;
                    *response = dbgbuf;
                    return bytes + 16;
                }
                default:
                    /* M1.1 (bit4): if a reply is pending but dbgstate is not
                       exactly RESPOND, NAK/keep-busy (return -3 = do nothing)
                       instead of the hard -2 STALL, so the OTG pipe does not
                       accumulate a persistent-stall residue on the wedge path. */
                    if (wedge_fix_enable & (1u<<4)) return -3;
                    return -2;
                }
                break;
            }
            break;
        default: break;
        }
        break;
        default: break;
    }
    return size;
}

void dbgthread(void* arg0, void* arg1, void* arg2, void* arg3)
{
    struct scheduler_thread* t;
    while (1)
    {
        dbg_monitor_heartbeat++;                 /* liveness heartbeat      */
        wakeup_wait(&dbgwakeup, TIMEOUT_BLOCK);
        for (t = head_thread; t; t = t->thread_next)
            if (t->state == THREAD_DEFUNCT)
            {
                if (t->block_type == THREAD_DEFUNCT_STKOV)
                {
                    if (t->name) cprintf(1, "\n*PANIC*\nStack overflow! (%s)\n", t->name);
                    else cprintf(1, "\n*PANIC*\nStack overflow! (%08X)\n", t);
                }
                t->state = THREAD_DEFUNCT_ACK;
            }
        uint32_t mode = enter_critical_section();
        uint32_t buf[16];
        if (dbgstate == DBGSTATE_ASYNC)
        {
            memcpy(buf, dbgbuf, 64);
            leave_critical_section(mode);
            void* addr = &buf[4];
            int len = 0;
            switch (buf[0])
            {
#ifdef HAVE_NAND5G
                case N5G_MODULE_RESERVE:
                case N5G_MODULE_ACTIVATE:
                    n5g_module_command(buf, buf);
                    break;
#endif
                case 2:  // RESET
                    shutdown(false);
                    reset();
                case 3:  // POWER OFF
                    if (buf[1]) shutdown(true);
                    power_off();
                    buf[0] = 1;
                    break;
#ifdef HAVE_I2C
                case 8:  // READ I2C
                    len = buf[1] >> 24;
                    i2c_recv(buf[1] & 0xff, (buf[1] >> 8) & 0xff, (buf[1] >> 16) & 0xff,
                             (uint8_t*)&buf[4], len);
                    buf[0] = 1;
                    break;
                case 9:  // WRITE I2C
                    i2c_send(buf[1] & 0xff, (buf[1] >> 8) & 0xff, (buf[1] >> 16) & 0xff,
                             (uint8_t*)&buf[4], buf[1] >> 24);
                    buf[0] = 1;
                    break;
#endif
                case 12:  // CWRITE
                    cwrite(buf[1], (const char*)&buf[4], buf[2]);
                    buf[0] = 1;
                    break;
                case 13:  // CREAD
                    buf[0] = 1;
                    buf[1] = cread(buf[1], (char*)&buf[4], buf[2], 0);
                    break;
                case 14:  // CFLUSH
                    cflush(buf[1]);
                    buf[0] = 1;
                    break;
#ifndef EMCORE_SLIM
                case 21:  // EXECIMAGE
                {
                    int argc = buf[2] >> 24;
                    if (!buf[3])
                    {
                        buf[3] = (uint32_t)&buf[4];
                        int i;
                        for (i = 0; i < argc; i++) buf[i + 4] += buf[3];
                    }
                    buf[0] = 1;
                    buf[1] = (uint32_t)execimage((void*)buf[1], buf[2] & 0x10000, argc, (const char* const*)buf[3]);
                    break;
                }
#endif
#ifdef HAVE_BOOTFLASH
                case 22:  // READ BOOT FLASH
                    bootflash_readraw((void*)buf[1], buf[2], buf[3]);
                    buf[0] = 1;
                    break;
                case 23:  // WRITE BOOT FLASH
                    bootflash_writeraw((void*)buf[1], buf[2], buf[3]);
                    buf[0] = 1;
                    break;
#endif
                case 24:  // EXECFIRMWARE
                    shutdown(false);
                    execfirmware((void*)buf[1], (void*)buf[2], (size_t)buf[3]);
                    buf[0] = 1;
                    break;
#ifdef HAVE_HWKEYAES
                case 25:  // HWKEYAES
#ifdef HAVE_HWKEYAES_EXT
                    /* nano 5G reports the driver status for both wire formats. */
                    {
                        buf[1] = (uint32_t)hwkeyaes_usb_ext((enum hwkeyaes_direction)((uint8_t*)buf)[4],
                                                            ((uint16_t*)buf)[3], ((uint8_t*)buf)[5], (void*)buf[2], buf[3]);
                        buf[0] = 1;
                        break;
                    }
#else
                    hwkeyaes((enum hwkeyaes_direction)((uint8_t*)buf)[4], ((uint16_t*)buf)[3], (void*)buf[2], buf[3]);
                    buf[0] = 1;
                    break;
#endif
#endif
#ifdef HAVE_HMACSHA1
                case 26:  // HMACSHA1
                    hmacsha1((void*)buf[1], buf[2], (void*)buf[3]);
                    buf[0] = 1;
                    break;
#endif
#ifdef HAVE_STORAGE
                case 27:  // STORAGE_GET_INFO
                    buf[0] = 1;
                    storage_get_info(buf[1], (struct storage_info*)&buf[4]);
                    buf[1] = 1;
                    len = (sizeof(struct storage_info) + 3) / 4 * 4;
                    break;
                case 28:  // STORAGE_READ_SECTORS_MD
                    buf[0] = 1;
                    buf[1] = (uint32_t)storage_read_sectors_md(buf[1], buf[2] | (((uint64_t)(buf[3]) << 32)),
                                                               buf[4], (void*)(buf[5]));
                    break;
                case 29:  // STORAGE_WRITE_SECTORS_MD
                    buf[0] = 1;
                    buf[1] = (uint32_t)storage_write_sectors_md(buf[1], buf[2] | (((uint64_t)(buf[3]) << 32)),
                                                                buf[4], (void*)(buf[5]));
                    break;
#ifndef EMCORE_SLIM   /* nano4g harness: no FAT/file/dir API  */
                case 30:  // FILE_OPEN
                {
                    buf[0] = 1;
                    if (!buf[3]) buf[3] = (uint32_t)&buf[4];
                    int fd = file_open((char*)buf[3], (int)buf[1]);
                    if (fd > 0) reown_file(fd, KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    buf[1] = (uint32_t)fd;
                    break;
                }
                case 31:  // FILESIZE
                    buf[0] = 1;
                    buf[1] = (uint32_t)filesize((int)buf[1]);
                    break;
                case 32:  // READ
                    buf[0] = 1;
                    buf[1] = (uint32_t)read((int)buf[1], (void*)buf[2], (size_t)buf[3]);
                    break;
                case 33:  // WRITE
                    buf[0] = 1;
                    buf[1] = (uint32_t)write((int)buf[1], (void*)buf[2], (size_t)buf[3]);
                    break;
                case 34:  // LSEEK
                    buf[0] = 1;
                    buf[1] = (uint32_t)lseek((int)buf[1], (off_t)buf[2], (int)buf[3]);
                    break;
                case 35:  // FTRUNCATE
                    buf[0] = 1;
                    buf[1] = (uint32_t)ftruncate((int)buf[1], (off_t)buf[2]);
                    break;
                case 36:  // FSYNC
                    buf[0] = 1;
                    buf[1] = (uint32_t)fsync((int)buf[1]);
                    break;
                case 37:  // CLOSE
                    buf[0] = 1;
                    buf[1] = (uint32_t)close((int)buf[1]);
                    break;
                case 38:  // CLOSE_MONITOR_FILES
                    buf[0] = 1;
                    buf[1] = (uint32_t)close_all_of_process(KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    break;
                case 39:  // RELEASE_FILES
                    buf[0] = 1;
                    buf[1] = (uint32_t)release_files((int)buf[1]);
                    break;
                case 40:  // REMOVE
                    buf[0] = 1;
                    if (!buf[3]) buf[3] = (uint32_t)&buf[4];
                    buf[1] = (uint32_t)remove((char*)buf[3]);
                    break;
                case 41:  // RENAME
                    buf[0] = 1;
                    buf[1] = (uint32_t)rename((char*)buf[2], (char*)buf[3]);
                    break;
                case 42:  // OPENDIR
                {
                    buf[0] = 1;
                    if (!buf[3]) buf[3] = (uint32_t)&buf[4];
                    DIR* dir = opendir((char*)buf[3]);
                    if (dir > 0) reown_dir(dir, KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    buf[1] = (uint32_t)dir;
                    break;
                }
                case 43:  // READDIR
                    buf[0] = 1;
                    buf[3] = (uint32_t)readdir((DIR*)buf[1]);
                    buf[1] = 1;
                    buf[2] = MAX_PATH;
                    break;
                case 44:  // CLOSEDIR
                    buf[0] = 1;
                    buf[1] = (uint32_t)closedir((DIR*)buf[1]);
                    break;
                case 45:  // CLOSE_MONITOR_DIRS
                    buf[0] = 1;
                    buf[1] = (uint32_t)closedir_all_of_process(KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    break;
                case 46:  // RELEASE_DIRS
                    buf[0] = 1;
                    buf[1] = (uint32_t)release_dirs((int)buf[1]);
                    break;
                case 47:  // MKDIR
                    buf[0] = 1;
                    if (!buf[3]) buf[3] = (uint32_t)&buf[4];
                    buf[1] = (uint32_t)mkdir((char*)buf[3]);
                    break;
                case 48:  // RMDIR
                    buf[0] = 1;
                    if (!buf[3]) buf[3] = (uint32_t)&buf[4];
                    buf[1] = (uint32_t)rmdir((char*)buf[3]);
                    break;
                case 49:  // ERRNO
                    buf[0] = 1;
                    buf[1] = (uint32_t)errno;
                    break;
#endif
#ifdef HAVE_HOTSWAP
                case 50:  // DISK_MOUNT
                    buf[0] = 1;
                    buf[1] = (uint32_t)disk_mount((int)buf[1]);
                    break;
                case 51:  // DISK_UNMOUNT
                    buf[0] = 1;
                    buf[1] = (uint32_t)disk_unmount((int)buf[1]);
                    break;
#endif
#ifndef EMCORE_SLIM
                case 58:  // FAT_ENABLE_FLUSHING
                    buf[0] = 1;
                    fat_enable_flushing((bool)buf[1]);
                    break;
                case 59:  // FAT_SIZE
                    buf[0] = 1;
                    fat_size_mv(buf[1], &buf[1], &buf[2]);
                    break;
#endif
#endif
                case 52:  // MALLOC
                    buf[0] = 1;
                    buf[1] = (uint32_t)malloc((size_t)buf[1]);
                    if (buf[1]) reownalloc((void*)buf[1], KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    break;
                case 53:  // MEMALIGN
                    buf[0] = 1;
                    buf[1] = (uint32_t)memalign((size_t)buf[1], (size_t)buf[2]);
                    if (buf[1]) reownalloc((void*)buf[1], KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    break;
                case 54:  // REALLOC
                    buf[0] = 1;
                    buf[1] = (uint32_t)realloc((void*)buf[1], (size_t)buf[2]);
                    break;
                case 55:  // REOWNALLOC
                    buf[0] = 1;
                    reownalloc((void*)buf[1], (void*)buf[2]);
                    break;
                case 56:  // FREE
                    buf[0] = 1;
                    free((void*)buf[1]);
                    break;
                case 57:  // FREE MONITOR ALLOCATIONS
                    buf[0] = 1;
                    free_all_of_thread(KERNEL_OWNER(KERNEL_OWNER_USB_MONITOR));
                    buf[1] = 0;
                    break;
#ifdef HAVE_RTC
                case 60:  // RTC READ
                    buf[0] = 1;
                    rtc_read_datetime((struct rtc_datetime*)&buf[1]);
                    break;
                case 61:  // RTC WRITE
                    buf[0] = 1;
                    rtc_write_datetime((const struct rtc_datetime*)&buf[1]);
                    break;
#endif
                case 200:  // GET MONITOR HEARTBEAT (liveness / wedge detection)
                    // buf[1] = heartbeat, buf[2] = NAND state word (bit31 busy | state<<24 |
                    // job kind<<16 | step reached<<8 | jobs done & 0xff), buf[3] = FIL pages read
                    // since boot (live progress counter), see nand_poc.h.
                    buf[0] = 1;
                    buf[1] = dbg_monitor_heartbeat;
#ifdef HAVE_NAND_POC
                    nand_poc_heartbeat(&buf[2], &buf[3]);
#elif defined(HAVE_NAND5G)
                    n5g_nand_heartbeat(&buf[2], &buf[3]);
#else
                    buf[2] = 0;
                    buf[3] = 0;
#endif
                    break;
                default:
#ifdef USB_HAVE_TARGET_SPECIFIC_REQUESTS
                    if (buf[0] >= 0xffff0000)
                        len = usb_target_handle_request(buf, sizeof(buf), &addr);
                    else buf[0] = 2;
#else
                    buf[0] = 2;
#endif
                    break;
            }
            mode = enter_critical_section();
            if (dbgstate == DBGSTATE_ASYNC)
            {
                usb_ep0_start_tx(dbgusb, NULL, 0, NULL);
                dbgstate = DBGSTATE_RESPOND;
                dbgmemaddr = addr;
                dbgmemlen = len;
                memcpy(dbgbuf, buf, 16);
            }
        }
        dbgbusy = false;
        leave_critical_section(mode);
    }
}

void usbdebug_init(void)
{
    wakeup_init(&dbgwakeup);
    dbgconsendreadidx = 0;
    dbgconsendwriteidx = 0;
    dbgconrecvreadidx = 0;
    dbgconrecvwriteidx = 0;
    wakeup_init(&dbgconsendwakeup);
    wakeup_init(&dbgconrecvwakeup);
    dbgenabled = false;
    dbgbusy = false;
    dbgstate = DBGSTATE_IDLE;
    dbgconsoleattached = false;
    thread_create(&dbgthread_handle, "monitor worker", dbgthread, dbgstack,
                  sizeof(dbgstack), CORE_THREAD, 255, true, NULL, NULL, NULL, NULL);
}

int dbgconsole_getfree() ICODE_ATTR;
int dbgconsole_getfree()
{
    int free = dbgconsendreadidx - dbgconsendwriteidx - 1;
    if (free < 0) free += sizeof(dbgconsendbuf);
    return free;
}

int dbgconsole_makespace(int length, bool safe) ICODE_ATTR;
int dbgconsole_makespace(int length, bool safe)
{
    int free = dbgconsole_getfree();
    while (!free && dbgconsoleattached && !safe)
    {
        dbgconsoleattached = false;
        wakeup_wait(&dbgconsendwakeup, 2000000);
        free = dbgconsole_getfree();
    }
    if (free) return free > length ? length : free;
    if (length > sizeof(dbgconsendbuf) - 17) length = sizeof(dbgconsendbuf) - 17;
    uint32_t mode = enter_critical_section();
    dbgconsendreadidx += length;
    if (dbgconsendreadidx >= sizeof(dbgconsendbuf))
        dbgconsendreadidx -= sizeof(dbgconsendbuf);
    int offset = 0;
    int idx = dbgconsendreadidx;
    if (idx + 16 >= sizeof(dbgconsendbuf))
    {
        offset = sizeof(dbgconsendbuf) - dbgconsendreadidx;
        memcpy(&dbgconsendbuf[dbgconsendreadidx], dbgconoverflowstr, offset);
        idx = 0;
    }
    if (offset != 16) memcpy(&dbgconsendbuf[idx], &dbgconoverflowstr[offset], 16 - offset);
    leave_critical_section(mode);
    return length;
}

void dbgconsole_putc_internal(char string, bool safe)
{
    dbgconsole_makespace(1, safe);
    dbgconsendbuf[dbgconsendwriteidx++] = string;
    if (dbgconsendwriteidx >= sizeof(dbgconsendbuf))
        dbgconsendwriteidx -= sizeof(dbgconsendbuf);
}

void dbgconsole_putc(char string)
{
    dbgconsole_putc_internal(string, false);
}

void dbgconsole_sputc(char string)
{
    dbgconsole_putc_internal(string, true);
}

void dbgconsole_write_internal(const char* string, size_t length, bool safe)
{
    while (length)
    {
        int space = dbgconsole_makespace(length, safe);
        if (dbgconsendwriteidx + space >= sizeof(dbgconsendbuf))
        {
            int bytes = sizeof(dbgconsendbuf) - dbgconsendwriteidx;
            memcpy(&dbgconsendbuf[dbgconsendwriteidx], string, bytes);
            dbgconsendwriteidx = 0;
            string = &string[bytes];
            space -= bytes;
            length -= bytes;
        }
        if (space) memcpy(&dbgconsendbuf[dbgconsendwriteidx], string, space);
        dbgconsendwriteidx += space;
        string = &string[space];
        length -= space;
    }
}

void dbgconsole_write(const char* string, size_t length)
{
    dbgconsole_write_internal(string, length, false);
}

void dbgconsole_swrite(const char* string, size_t length)
{
    dbgconsole_write_internal(string, length, true);
}

void dbgconsole_puts(const char* string)
{
    dbgconsole_write(string, strlen(string));
}

void dbgconsole_sputs(const char* string)
{
    dbgconsole_swrite(string, strlen(string));
}

int dbgconsole_getavailable() ICODE_ATTR;
int dbgconsole_getavailable()
{
    int available = dbgconrecvwriteidx - dbgconrecvreadidx;
    if (available < 0) available += sizeof(dbgconrecvbuf);
    return available;
}

int dbgconsole_getc(int timeout)
{
    if (!dbgconsole_getavailable())
    {
        wakeup_wait(&dbgconrecvwakeup, TIMEOUT_NONE);
        if (!dbgconsole_getavailable())
        {
            wakeup_wait(&dbgconrecvwakeup, timeout);
            if (!dbgconsole_getavailable()) return -1;
        }
    }
    int byte = dbgconrecvbuf[dbgconrecvreadidx++];
    if (dbgconrecvreadidx >= sizeof(dbgconrecvbuf))
        dbgconrecvreadidx -= sizeof(dbgconrecvbuf);
    return byte;
}

int dbgconsole_read(char* buffer, size_t length, int timeout)
{
    if (!length) return 0;
    int available = dbgconsole_getavailable();
    if (!available)
    {
        wakeup_wait(&dbgconrecvwakeup, TIMEOUT_NONE);
        int available = dbgconsole_getavailable();
        if (!available)
        {
            wakeup_wait(&dbgconrecvwakeup, timeout);
            int available = dbgconsole_getavailable();
            if (!available) return 0;
        }
    }
    if (available > length) available = length;
    int left = available;
    if (dbgconrecvreadidx + available >= sizeof(dbgconrecvbuf))
    {
        int bytes = sizeof(dbgconrecvbuf) - dbgconrecvreadidx;
        memcpy(buffer, &dbgconrecvbuf[dbgconrecvreadidx], bytes);
        dbgconrecvreadidx = 0;
        buffer = &buffer[bytes];
        left -= bytes;
    }
    if (left) memcpy(buffer, &dbgconrecvbuf[dbgconrecvreadidx], left);
    dbgconrecvreadidx += left;
    return available;
}

