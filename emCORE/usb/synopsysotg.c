#include "global.h"
#include "synopsysotg.h"
#include "usb.h"
#include "timer.h"
#include "util.h"
#include "mmu.h"   /* WEDGE-FIX-2026-09-20 M0.4: proper decls for the D-cache ops
                       (the legacy clean_dcache/invalidate_dcache calls below were
                       previously IMPLICIT declarations - not in any header. mmu.h
                       now also declares the new range-based ops). */

/* =============================================================
 * WEDGE-FIX-2026-09-20: top-down USB-wedge elimination (Agent A).
 * Runtime-gated fixes, one at a time, via `wedge_fix_enable` (u32 bitmask;
 * DEFINED in usbdebug.c - Agent B; set/read by host via USB cmd 68).
 *   bit0 = M0.4 range-based D-cache ops (instead of full-cache per transfer)
 *   bit1 = M0.1 conditional TX-FIFO flush (only when FIFO non-empty)
 *   bit2 = M0.2 conditional RX-FIFO residual drain
 *   bit3 = M0.3 GINTSTS W1C only the handled sources (no whole-snapshot write-back)
 * ALWAYS-ON INSTRUMENTATION (not gated; host reads these via normal cmd-4):
 *   wedge_snap[0] IRQ-hold MAX (us), [1] COUNT, [2] LAST (us),
 *   [4] TX-FIFO free words (at next arm), [5] RX-FIFO resident
 * New symbols in this file: wedge_irq_hold_sample / wedge_txfifo_free /
 *   wedge_rx_resident (static inline helpers) + the WEDGE_SNAP_* address macros.
 * All waits BOUNDED (no unbounded OTG spins). No device touched (code-only).
 * ============================================================= */
extern uint32_t wedge_fix_enable;

/* Snapshot words (formerly at the absolute address 0x08827300, which lies inside the
   malloc pool; moved into .bss so heap users such as the NAND FTL cannot collide). The
   host finds them via the symbol wedge_snap (layout unchanged: +0 MAX, +4 COUNT,
   +8 LAST, +0x10 TXFREE, +0x14 RXRES). */
volatile uint32_t wedge_snap[14];
#define WEDGE_SNAP_IRQMAX   (wedge_snap[0])
#define WEDGE_SNAP_IRQCNT   (wedge_snap[1])
#define WEDGE_SNAP_IRQLAST  (wedge_snap[2])
#define WEDGE_SNAP_TXFREE   (wedge_snap[4])
#define WEDGE_SNAP_RXRES    (wedge_snap[5])

/* record one IRQ-held-time sample (us). u32 microsecond timer wraps; a delta
   of < 2^31 is a valid interval (all IRQ holds are far below that). */
static inline void wedge_irq_hold_sample(uint32_t t0, uint32_t t1)
{
    uint32_t d = t1 - t0;
    if ((int32_t)d >= 0)
    {
        WEDGE_SNAP_IRQLAST = d;
        WEDGE_SNAP_IRQCNT++;
        if (d > WEDGE_SNAP_IRQMAX) WEDGE_SNAP_IRQMAX = d;
    }
}

/* TX-FIFO free words for an IN endpoint as seen at transfer-arm time.
   ep0 uses the non-periodic FIFO (HNPTXSTS); ep>0 uses its dedicated FIFO
   (DTXFSTS, txfnum=ep, depth = txfifosize[ep], 0x100 for EP1/EP3 here). */
static inline uint32_t wedge_txfifo_free(const struct synopsysotg_config* data, int ep)
{
    if (!ep) return data->core->gregs.hnptxsts.b.nptxfspcavail;
    return data->core->inep_regs[ep].dtxfsts.b.txfspcavail;
}

/* resident RX-status entries, read NON-destructively (GRXSTSR, no pop).
   pktsts != 0 means a valid status entry is queued = residue in DMA mode. */
static inline uint32_t wedge_rx_resident(const struct synopsysotg_config* data)
{
    return (data->core->gregs.grxstsr.b.pktsts != 0u) ? 1u : 0u;
}


#ifndef SYNOPSYSOTG_AHB_BURST_LEN
#define SYNOPSYSOTG_AHB_BURST_LEN 5
#endif
#ifndef SYNOPSYSOTG_AHB_THRESHOLD
#define SYNOPSYSOTG_AHB_THRESHOLD 8
#endif
#ifndef SYNOPSYSOTG_TURNAROUND
#define SYNOPSYSOTG_TURNAROUND 3
#endif

static void synopsysotg_flush_out_endpoint(const struct usb_instance* instance, int ep)
{
    if (!ep) return;
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    if (data->core->outep_regs[ep].doepctl.b.epena)
    {
        // We are waiting for an OUT packet on this endpoint, which might arrive any moment.
        // Assert a global output NAK to avoid race conditions while shutting down the endpoint.
        synopsysotg_target_disable_irq(instance);
        data->core->dregs.dctl.b.sgoutnak = 1;
        while (!(data->core->gregs.gintsts.b.goutnakeff));
        data->core->outep_regs[ep].doepctl.b.snak = 1;
        data->core->outep_regs[ep].doepctl.b.epdis = 1;
        while (!(data->core->outep_regs[ep].doepint.b.epdisabled));
        data->core->dregs.dctl.b.cgoutnak = 1;
        synopsysotg_target_enable_irq(instance);
    }
    data->core->outep_regs[ep].doepctl.b.usbactep = 0;
    // Reset the transfer size register. Not strictly necessary, but can't hurt.
    data->core->outep_regs[ep].doeptsiz.d32 = 0;
}

static void synopsysotg_flush_in_endpoint(const struct usb_instance* instance, int ep)
{
    if (!ep) return;
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    if (data->core->inep_regs[ep].diepctl.b.epena)
    {
        // We are shutting down an endpoint that might still have IN packets in the FIFO.
        // Disable the endpoint, wait for things to settle, and flush the relevant FIFO.
        synopsysotg_target_disable_irq(instance);
        data->core->inep_regs[ep].diepctl.b.snak = 1;
        while (!(data->core->inep_regs[ep].diepint.b.inepnakeff));
        data->core->inep_regs[ep].diepctl.b.epdis = 1;
        while (!(data->core->inep_regs[ep].diepint.b.epdisabled));
        data->core->inep_regs[ep].diepctl.b.usbactep = 0;
        synopsysotg_target_enable_irq(instance);
        // Wait for any DMA activity to stop, to make sure nobody will touch the FIFO.
        while (!data->core->gregs.grstctl.b.ahbidle);
        // Flush it all the way down!
        union synopsysotg_grstctl grstctl = { .b = { .txfnum = data->core->inep_regs[ep].diepctl.b.txfnum, .txfflsh = 1 } };
        data->core->gregs.grstctl = grstctl;
        while (data->core->gregs.grstctl.b.txfflsh);
    }
    else if (ep) data->core->inep_regs[ep].diepctl.b.usbactep = 0;
    // Reset the transfer size register. Not strictly necessary, but can't hurt.
    data->core->inep_regs[ep].dieptsiz.d32 = 0;
}

/* Flush the non-periodic TX FIFO used by endpoint `ep` (EP0 uses txfnum 0).
   This is the S5L8720 OTG "TXFIFO residue" fix: the DW_otg-style UDC never
drains the TX FIFO between EP0 transfers by itself, so a data stage leaves
stale FIFO words behind that get prepended to the NEXT transfer and, over
thousands of sustained page reads, accumulate until I/O errors -> overflow
(Errno 75) -> the device drops off USB. Flushing an empty FIFO (the normal
case between completed packets) is a harmless no-op; flushing before arming a
fresh transfer removes any residual words. All waits are bounded (project
rule: no unbounded OTG/NAND spins - see the bootROM BOUNDED-wait pattern). */
static void synopsysotg_flush_txfifo(const struct usb_instance* instance, int ep)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    int i;
    for (i = 0; i < 1000 && !data->core->gregs.grstctl.b.ahbidle; i++) ;
    union synopsysotg_grstctl grstctl = { .b = { .txfnum = ep, .txfflsh = 1 } };
    data->core->gregs.grstctl = grstctl;
    for (i = 0; i < 1000 && data->core->gregs.grstctl.b.txfflsh; i++) ;
    for (i = 0; i < 1000 && !data->core->gregs.grstctl.b.ahbidle; i++) ;
}


/* ==========================================================================
 * EP0-WEDGE FIX (2026-09-28): kill a still-armed EP0 IN transfer on SETUP.
 *
 * THE WEDGE (device-observed, 2026-09-20 echo probes + every dump tool since):
 * a host-aborted or timed-out EP0 control-IN (the xHCI gives up on a data
 * stage that stalls/NAKs too long, or the host tool times out and re-issues)
 * leaves the EP0 IN pipe ARMED: DIEPCTL0.epena=1, DIEPTSIZ nonzero, possibly
 * residual words in the non-periodic TX FIFO. The stock driver NEVER kills it:
 * synopsysotg_flush_in_endpoint() early-returns for ep 0, and
 * usb_handle_setup_received() -> usb_handle_ep0_setup() arms the new data
 * stage on top of the old one (DIEPDMA/DIEPTSIZ writes while epena=1 are
 * largely IGNORED by the core). The stale armed transfer then desyncs the
 * next control-IN: the core pushes stale/short data (host EOVERFLOW Errno 75
 * / EIO Errno 5) and the reply-pipeline state machine never completes its
 * chain (device-side signature: dbgstate stuck DBGSTATE_RESPOND, ep0_tx_ptr
 * dangling, callbacks NULL) -> the host xHCI HALTS the control endpoint ->
 * "wedge": every subsequent transfer fails EIO until dev.reset().
 *
 * THE FIX (what a real DW_otg stack does on SETUP): disable + flush the EP0 IN
 * pipe when (and ONLY when) it is still enabled when a SETUP arrives:
 * SNAK -> EPDIS -> bounded wait for epdisabled -> W1C DIEPINT0 (so a stale
 * xfercompl cannot run the dead reply chain in this IRQ pass) -> bounded
 * non-periodic TX-FIFO flush (drop the stale FIFO words).
 *
 * NO-OP IN THE NORMAL CASE: on a completed control transfer epena has
 * auto-cleared long before the next SETUP, so this adds zero register writes
 * to the healthy path (the wr-only/rd-only idempotency baseline is
 * unaffected). It acts exactly on the abnormal case that previously corrupted
 * the pipe. ALL WAITS BOUNDED (project rule: no unbounded OTG spins).
 * ========================================================================== */
static void synopsysotg_ep0_kill_stale_in(const struct usb_instance* instance)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    volatile struct synopsysotg_inepregs* regs = &data->core->inep_regs[0];
    int i;

    if (!regs->diepctl.b.epena) return;   /* healthy path: pipe idle, no-op */

    /* disable the armed IN transfer (SNAK first so the core stops populating
       the FIFO, then EPDIS); bounded wait for the disable to latch */
    regs->diepctl.b.snak = 1;
    regs->diepctl.b.epdis = 1;
    for (i = 0; i < 1000 && !regs->diepint.b.epdisabled; i++) ;
    /* W1C the endpoint interrupt latch so a stale xfercompl/timeout from the
       killed transfer cannot fire the dead reply chain in this IRQ pass */
    regs->diepint = regs->diepint;
    /* bounded non-periodic TX-FIFO flush: drop the stale FIFO words */
    synopsysotg_flush_txfifo(instance, 0);
}

static void synopsysotg_flush_ints(const struct usb_instance* instance)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    int i;
    for (i = 0; i < 16; i++)
    {
        data->core->outep_regs[i].doepint = data->core->outep_regs[i].doepint;
        data->core->inep_regs[i].diepint = data->core->inep_regs[i].diepint;
    }
    data->core->gregs.gintsts = data->core->gregs.gintsts;
}

static void synopsysotg_try_push(const struct usb_instance* instance, int ep)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    struct synopsysotg_state* state = (struct synopsysotg_state*)instance->driver_state;
    union synopsysotg_depctl depctl = data->core->inep_regs[ep].diepctl;
    if (!depctl.b.epena || !depctl.b.usbactep || depctl.b.stall || depctl.b.naksts) return;
    int bytesleft = data->core->inep_regs[ep].dieptsiz.b.xfersize;
    if (!bytesleft) return;
    int maxpacket = ep ? data->core->inep_regs[ep].diepctl.b.mps : 64;
    union synopsysotg_hnptxsts fifospace = data->core->gregs.hnptxsts;
    int words = (MIN(maxpacket, bytesleft) + 3) >> 2;
    if (fifospace.b.nptxqspcavail && fifospace.b.nptxfspcavail << 2 >= words)
        while (words--) data->core->dfifo[ep][0] = *state->endpoints[ep].txaddr++;
    if (!words && bytesleft <= maxpacket) return;
    data->core->gregs.gintmsk.b.nptxfempty = true;
}

void synopsysotg_start_rx(const struct usb_instance* instance, union usb_endpoint_number ep, void* buf, int size)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    struct synopsysotg_state* state = (struct synopsysotg_state*)instance->driver_state;

    // Find the appropriate set of endpoint registers
    volatile struct synopsysotg_outepregs* regs = &data->core->outep_regs[ep.number];
    // Calculate number of packets (if size == 0 an empty packet will be sent)
    int maxpacket = regs->doepctl.b.mps;
    int packets = (size + maxpacket - 1) / maxpacket;
    if (!packets) packets = 1;

    // Set up data destination
    if (data->use_dma) regs->doepdma = buf;
    else state->endpoints[ep.number].rxaddr = (uint32_t*)buf;
    union synopsysotg_depxfrsiz deptsiz = { .b = { .pktcnt = packets, .xfersize = size } };
    regs->doeptsiz = deptsiz;

    /* WEDGE-FIX-2026-09-20 M0.2 (bit2): drain a residual RX-status entry before
       re-arming an OUT endpoint in DMA mode. In DMA mode software never pops the
       RX status queue, so an unclaimed entry (artefact of a previous transfer)
       sits in the FIFO and desyncs the RX state machine over ~300 reads. Flush
       ONLY when GRXSTSR shows a resident entry (never pop GRXSTSP here) and
       bounded. NOTE: legit OUT data reaches memory via doepdma, not this queue. */
    if (data->use_dma && (wedge_fix_enable & (1u << 2)) && wedge_rx_resident(data))
    {
        union synopsysotg_grstctl rxf = { .b = { .rxfflsh = 1 } };
        int i;
        data->core->gregs.grstctl = rxf;
        for (i = 0; i < 1000 && data->core->gregs.grstctl.b.rxfflsh; i++) ;
    }
    WEDGE_SNAP_RXRES = wedge_rx_resident(data);

    // Flush CPU cache if necessary
    if (data->use_dma)
    {
        if (wedge_fix_enable & (1u << 0)) invalidate_dcache_range((uint32_t)buf, size);
        else invalidate_dcache(buf, size);
    }

    // Enable the endpoint
    union synopsysotg_depctl depctl = regs->doepctl;
    depctl.b.epena = 1;
    depctl.b.cnak = 1;
    regs->doepctl = depctl;
}

void synopsysotg_start_tx(const struct usb_instance* instance, union usb_endpoint_number ep, const void* buf, int size)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    struct synopsysotg_state* state = (struct synopsysotg_state*)instance->driver_state;

    // Find the appropriate set of endpoint registers
    volatile struct synopsysotg_inepregs* regs = &data->core->inep_regs[ep.number];

    // WEDGE-FIX-2026-09-20: observe the FIFO state at arm time (residue detector).
    WEDGE_SNAP_TXFREE = wedge_txfifo_free(data, ep.number);

    // WEDGE-FIX-2026-09-20 M0.1 (bit1): CONDITIONAL TX-FIFO flush - ONLY when the
    // endpoint's TX FIFO still holds residual words from a previous transfer
    // (DTXFSTS free < its configured depth). This is the fix for the residue the
    // reverted every-transfer flush was meant to clear, WITHOUT the destabilizing
    // blanket GRSTCTL+ahbidle dance on empty FIFOs that caused the ~5-64-read
    // early-drop regression. Flush is the existing bounded synopsysotg_flush_txfifo.
    if (data->use_dma && (wedge_fix_enable & (1u << 1)))
    {
        int depth = data->txfifosize[ep.number];
        uint32_t free = wedge_txfifo_free(data, ep.number);
        if (depth && free < (uint32_t)depth) synopsysotg_flush_txfifo(instance, ep.number);
    }

    // NOTE (2026-09-18 REVERT): the bulk-IN TX-FIFO flush was REMOVED.
    // On-device evidence: with this flush, the L2P dump drops the device after
    // only ~5-64 reads (Errno 5/75 + unresponsive), whereas the no-flush
    // baseline survived 533 reads with the device alive. GRSTCTL.TXFFLSH +
    // ahbidle wait on every bulk transfer destabilizes the OTG during DMA bulk
    // IN reads. Residual OTG residue is instead handled host-side by the dump's
    // periodic reconnect/re-claim.

    // Calculate number of packets (if size == 0 an empty packet will be sent)
    int maxpacket = regs->diepctl.b.mps;
    int packets = (size + maxpacket - 1) / maxpacket;
    if (!packets) packets = 1;

    // Set up data source
    if (data->use_dma) regs->diepdma = buf;
    else state->endpoints[ep.number].txaddr = (uint32_t*)buf;
    union synopsysotg_depxfrsiz deptsiz = { .b = { .pktcnt = packets, .xfersize = size } };
    regs->dieptsiz = deptsiz;

    // Flush CPU cache if necessary
    if (data->use_dma)
    {
        if (wedge_fix_enable & (1u << 0)) clean_dcache_range((uint32_t)buf, size);
        else clean_dcache(buf, size);
    }

    // Enable the endpoint
    union synopsysotg_depctl depctl = regs->diepctl;
    depctl.b.epena = 1;
    depctl.b.cnak = 1;
    regs->diepctl = depctl;

    // Start pushing data into the FIFO (must be done after enabling the endpoint)
    if (size && !data->use_dma)
    {
        if (data->shared_txfifo) synopsysotg_try_push(instance, ep.number);
        else data->core->dregs.diepempmsk.ep.in |= (1 << ep.number);
    }
}

int synopsysotg_get_stall(const struct usb_instance* instance, union usb_endpoint_number ep)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    if (ep.direction == USB_ENDPOINT_DIRECTION_IN)
        return !!data->core->inep_regs[ep.number].diepctl.b.stall;
    return !!data->core->outep_regs[ep.number].doepctl.b.stall;
}

void synopsysotg_set_stall(const struct usb_instance* instance, union usb_endpoint_number ep, bool stall)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    if (ep.direction == USB_ENDPOINT_DIRECTION_IN)
    {
        data->core->inep_regs[ep.number].diepctl.b.stall = !!stall;
        if (!stall) data->core->inep_regs[ep.number].diepctl.b.setd0pid = true;
    }
    else
    {
        data->core->outep_regs[ep.number].doepctl.b.stall = !!stall;
        if (!stall) data->core->outep_regs[ep.number].doepctl.b.setd0pid = true;
    }

}

void synopsysotg_set_address(const struct usb_instance* instance, uint8_t address)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    data->core->dregs.dcfg.b.devaddr = address;
}

void synopsysotg_unconfigure_ep(const struct usb_instance* instance, union usb_endpoint_number ep)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    if (ep.direction == USB_ENDPOINT_DIRECTION_IN)
    {
        // Kill any outstanding IN transfers
        synopsysotg_flush_in_endpoint(instance, ep.number);
        // Mask interrupts for this endpoint
        data->core->dregs.daintmsk.ep.in &= ~(1 << ep.number);
    }
    else
    {
        // Kill any outstanding OUT transfers
        synopsysotg_flush_out_endpoint(instance, ep.number);
        // Mask interrupts for this endpoint
        data->core->dregs.daintmsk.ep.out &= ~(1 << ep.number);
    }
}

void synopsysotg_configure_ep(const struct usb_instance* instance, union usb_endpoint_number ep,
                              enum usb_endpoint_type type, int maxpacket)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;

    // Write the new configuration and unmask interrupts for the endpoint.
    // Reset data toggle to DATA0, as required by the USB specification.
    int txfifo = data->shared_txfifo ? 0 : ep.number;
    union synopsysotg_depctl depctl = { .b = { .usbactep = 1, .eptype = type, .mps = maxpacket,
                                               .txfnum = txfifo, .setd0pid = 1, .nextep = (ep.number + 1) & 0xf } };
    if (ep.direction == USB_ENDPOINT_DIRECTION_IN)
    {
        data->core->inep_regs[ep.number].diepctl = depctl;
        data->core->dregs.daintmsk.ep.in |= (1 << ep.number);
    }
    else
    {
        data->core->outep_regs[ep.number].doepctl = depctl;
        data->core->dregs.daintmsk.ep.out |= (1 << ep.number);
    }
}

void synopsysotg_ep0_start_rx(const struct usb_instance* instance, bool non_setup, int len)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    struct synopsysotg_state* state = (struct synopsysotg_state*)instance->driver_state;

    // Set up data destination
    if (data->use_dma) data->core->outep_regs[0].doepdma = instance->buffer;
    else state->endpoints[0].rxaddr = (uint32_t*)instance->buffer;
    union synopsysotg_dep0xfrsiz deptsiz = { .b = { .supcnt = 3, .pktcnt = !!non_setup, .xfersize = len } };
    data->core->outep_regs[0].doeptsiz.d32 = deptsiz.d32;

    // Flush CPU cache if necessary
    if (data->use_dma)
    {
        if (wedge_fix_enable & (1u << 0)) invalidate_dcache_range((uint32_t)instance->buffer, len);
        else invalidate_dcache(instance->buffer, len);
    }

    // Enable the endpoint
    union synopsysotg_depctl depctl = data->core->outep_regs[0].doepctl;
    depctl.b.epena = 1;
    depctl.b.cnak = !!non_setup;
    data->core->outep_regs[0].doepctl = depctl;
}

void synopsysotg_ep0_start_tx(const struct usb_instance* instance, const void* buf, int len)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    struct synopsysotg_state* state = (struct synopsysotg_state*)instance->driver_state;

    // WEDGE-FIX-2026-09-20: observe the non-periodic TX FIFO state at arm time
    // (residue detector for the ~340 EP0 stages of each 16KB cmd-4 reply).
    WEDGE_SNAP_TXFREE = wedge_txfifo_free(data, 0);

    if (len)
    {
        // NOTE (2026-09-18 REVERT): the EP0 TX-FIFO flush was REMOVED too - it
        // was also added after the proven 533-read-stable baseline and the same
        // on-device evidence (drop after ~5-64 reads with flushes present vs
        // 533 alive without) applies. Residual OTG residue is handled host-side
        // by the dump's periodic reconnect/re-claim. synopsysotg_flush_txfifo
        // remains defined as a documented utility.
        //
        // WEDGE-FIX-2026-09-20 M0.1 (bit1): CONDITIONAL flush - only when the
        // EP0/non-periodic TX FIFO holds residual words (HNPTXSTS free < the
        // EP0 FIFO depth). Never on an empty FIFO (that was the regression).
        if (data->use_dma && (wedge_fix_enable & (1u << 1)))
        {
            uint32_t depth = data->txfifosize[0];
            uint32_t free = wedge_txfifo_free(data, 0);
            if (depth && free < depth) synopsysotg_flush_txfifo(instance, 0);
        }
        // Set up data source
        if (data->use_dma) data->core->inep_regs[0].diepdma = buf;
        else state->endpoints[0].txaddr = buf;
        union synopsysotg_dep0xfrsiz deptsiz = { .b = { .pktcnt = (len + 63) >> 6, .xfersize = len } };
        data->core->inep_regs[0].dieptsiz.d32 = deptsiz.d32;
    }
    else
    {
        // Set up the IN pipe for a zero-length packet
        union synopsysotg_dep0xfrsiz deptsiz = { .b = { .pktcnt = 1 } };
        data->core->inep_regs[0].dieptsiz.d32 = deptsiz.d32;
    }

    // Flush CPU cache if necessary
    if (data->use_dma)
    {
        if (wedge_fix_enable & (1u << 0)) clean_dcache_range((uint32_t)buf, len);
        else clean_dcache(buf, len);
    }

    // Enable the endpoint
    union synopsysotg_depctl depctl = data->core->inep_regs[0].diepctl;
    depctl.b.epena = 1;
    depctl.b.cnak = 1;
    data->core->inep_regs[0].diepctl = depctl;

    // Start pushing data into the FIFO (must be done after enabling the endpoint)
    if (len && !data->use_dma)
    {
        if (data->shared_txfifo) synopsysotg_try_push(instance, 0);
        else data->core->dregs.diepempmsk.ep.in |= 1;
    }
}

static void synopsysotg_ep0_init(const struct usb_instance* instance)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;

    // Make sure both EP0 pipes are active.
    // (The hardware should take care of that, but who knows...)
    union synopsysotg_depctl depctl = { .b = { .usbactep = 1, .nextep = data->core->inep_regs[0].diepctl.b.nextep } };
    data->core->outep_regs[0].doepctl = depctl;
    data->core->inep_regs[0].diepctl = depctl;
}

void synopsysotg_irq(const struct usb_instance* instance)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;
    struct synopsysotg_state* state = (struct synopsysotg_state*)instance->driver_state;

    /* WEDGE-FIX-2026-09-20: always-on IRQ-held-time instrumentation.
       case-64 runs the multi-ms M2 NAND read inside this handler -> the IRQ is
       held for ms and the host's in-flight transfer goes un-answered (Errno
       110) -> OTG desync. This measures the hold time per IRQ pass so the host
       can prove/disprove the IRQ-latency hypothesis (M2.1) and the cache-op
       cost (M0.4) by comparing a gated build against the baseline. */
    uint32_t wedge_t0 = USEC_TIMER;

    union synopsysotg_gintsts gintsts = data->core->gregs.gintsts;

    /* FIX-GLM3 (bootROM pattern, dumps/bootrom.S 0xa0f4/a0f8): W1C the entry
       snapshot IMMEDIATELY, before any handling. The legacy end-of-handler
       write-back cleared any interrupt that RE-ASSERTED during the handler body
       - at high speed the next EP0 packet/SETUP completes 1-5us after the SW
       re-arm while this handler (full-cache ops, 10-30us) is still running, so a
       same-source event (WRITE pkt1->pkt2, reply stage1->2) was W1C'd away and
       its callback never ran -> the pipe stayed armed forever (no recovery path)
       -> permanent NAK -> host EIO + xHCI control-EP halt. W1C at entry means a
       re-asserted event re-latches after the clear and re-fires the IRQ: nothing
       is lost. (Exit write-back removed below.) */
    data->core->gregs.gintsts = gintsts;

    if (gintsts.b.usbreset)
    {
        data->core->dregs.dcfg.b.devaddr = 0;
        usb_handle_bus_reset(instance, 0);
        if (wedge_fix_enable & (1u << 3))
        { union synopsysotg_gintsts c = { .b = { .usbreset = 1 } }; data->core->gregs.gintsts = c; }
    }

    if (gintsts.b.enumdone)
    {
        usb_handle_bus_reset(instance, data->core->dregs.dsts.b.enumspd == 0);
        synopsysotg_ep0_init(instance);
        if (wedge_fix_enable & (1u << 3))
        { union synopsysotg_gintsts c = { .b = { .enumdone = 1 } }; data->core->gregs.gintsts = c; }
    }

    if (gintsts.b.rxstsqlvl && !data->use_dma)
    {
        // Device to memory part of the "software DMA" implementation, used to receive data if use_dma == 0.
        // Handle one packet at a time, the IRQ will re-trigger if there's something left.
        union synopsysotg_grxfsts rxsts = data->core->gregs.grxstsp;
        int ep = rxsts.b.chnum;
        int words = (rxsts.b.bcnt + 3) >> 2;
        while (words--) *state->endpoints[ep].rxaddr++ = data->core->dfifo[0][0];
        if (wedge_fix_enable & (1u << 3))
        { union synopsysotg_gintsts c = { .b = { .rxstsqlvl = 1 } }; data->core->gregs.gintsts = c; }
    }

    if (gintsts.b.nptxfempty && data->core->gregs.gintmsk.b.nptxfempty)
    {
        // Old style, "shared TX FIFO" memory to device part of the "software DMA" implementation,
        // used to send data if use_dma == 0 and the device doesn't support one non-periodic TX FIFO per endpoint.

        // First disable the IRQ, it will be re-enabled later if there is anything left to be done.
        data->core->gregs.gintmsk.b.nptxfempty = false;

        // Check all endpoints for anything to be transmitted
        int ep;
        for (ep = 0; ep < 16; ep++) synopsysotg_try_push(instance, ep);
        if (wedge_fix_enable & (1u << 3))
        { union synopsysotg_gintsts c = { .b = { .nptxfempty = 1 } }; data->core->gregs.gintsts = c; }
    }

    if (gintsts.b.inepintr)
    {
        union synopsysotg_daint daint = data->core->dregs.daint;
        int ep;
        for (ep = 0; ep < 16; ep++)
            if (daint.ep.in & (1 << ep))
            {
                union synopsysotg_diepintn epints = data->core->inep_regs[ep].diepint;
                if (epints.b.emptyintr)
                {
                    // Memory to device part of the "software DMA" implementation, used to transmit data if use_dma == 0.
                    union synopsysotg_depxfrsiz deptsiz = data->core->inep_regs[ep].dieptsiz;
                    if (!deptsiz.b.xfersize) data->core->dregs.diepempmsk.ep.in &= ~(1 << ep);
                    else
                    {
                        // Push data into the TX FIFO until we don't have anything left or the FIFO would overflow.
                        int left = (deptsiz.b.xfersize + 3) >> 2;
                        while (left)
                        {
                            int words = data->core->inep_regs[ep].dtxfsts.b.txfspcavail;
                            if (words > left) words = left;
                            if (!words) break;
                            left -= words;
                            while (words--) data->core->dfifo[ep][0] = *state->endpoints[ep].txaddr++;
                        }
                    }
                }
                union usb_endpoint_number epnum = { .direction = USB_ENDPOINT_DIRECTION_IN, .number = ep };
                int bytesleft = data->core->inep_regs[ep].dieptsiz.b.xfersize;
                data->core->inep_regs[ep].diepint = epints;
                if (epints.b.timeout) usb_handle_timeout(instance, epnum, bytesleft);
                if (epints.b.xfercompl) usb_handle_xfer_complete(instance, epnum, bytesleft);
            }
        if (wedge_fix_enable & (1u << 3))
        { union synopsysotg_gintsts c = { .b = { .inepintr = 1 } }; data->core->gregs.gintsts = c; }
    }

    if (gintsts.b.outepintr)
    {
        union synopsysotg_daint daint = data->core->dregs.daint;
        int ep;
        for (ep = 0; ep < 16; ep++)
            if (daint.ep.out & (1 << ep))
            {
                union synopsysotg_doepintn epints = data->core->outep_regs[ep].doepint;
                data->core->outep_regs[ep].doepint = epints;
                union usb_endpoint_number epnum = { .direction = USB_ENDPOINT_DIRECTION_OUT, .number = ep };
                if (epints.b.setup)
                {
                    if (data->use_dma)
                    {
                        if (wedge_fix_enable & (1u << 0))
                            invalidate_dcache_range((uint32_t)data->core->outep_regs[ep].doepdma, 8);
                        else invalidate_dcache((const void*)data->core->outep_regs[ep].doepdma, 8);
                    }
                    synopsysotg_flush_in_endpoint(instance, ep);
                    /* EP0-WEDGE FIX (2026-09-28): the stock flush above
                       early-returns for ep 0, so a host-aborted control-IN
                       data stage stays ARMED across SETUPs and desyncs the
                       pipe (-> host xHCI control-EP halt, the recurring
                       dev.reset wedge).  Kill a still-enabled EP0 IN on SETUP
                       (a no-op when the pipe is idle -- the normal case). */
                    if (!ep) synopsysotg_ep0_kill_stale_in(instance);
                    /* FIX-GLM4: revert the per-SETUP EP0 toggle-reset poke (Change B). The
                       bootROM (dumps/bootrom.S) never writes SETD0PID/SNAK on EP0 - the
                       S5L8720 OTG core auto-manages the EP0 data PID per SETUP. wr-only
                       (2-packet OUT + status-IN-ZLP + 16B control-IN reply) is idempotent
                       to 500k without any poke. The SNak/naksts-wait/setd0pid poke adds
                       jitter in the race window and is built on a refuted theory. */
                    usb_handle_setup_received(instance, epnum);
                }
                else if (epints.b.xfercompl)
                {
                    int bytesleft = data->core->outep_regs[ep].doeptsiz.b.xfersize;
                    usb_handle_xfer_complete(instance, epnum, bytesleft);
                }
            }
        if (wedge_fix_enable & (1u << 3))
        { union synopsysotg_gintsts c = { .b = { .outepintr = 1 } }; data->core->gregs.gintsts = c; }
    }

    /* FIX-GLM3: the legacy whole-snapshot exit write-back is REMOVED (it W1C'd a
       source that re-asserted during the handler body). The entry-W1C above now
       clears the serviced bits; a re-asserted source stays set and re-fires the
       IRQ. (bit3 block-end W1Cs remain only for the bit3-gated A/B path.) */

    /* always-on IRQ-held-time sample (entry t0 -> now) */
    wedge_irq_hold_sample(wedge_t0, USEC_TIMER);
}

void synopsysotg_init(const struct usb_instance* instance)
{
    int i;

    /* WEDGE-DEBUG (2026-09-20, D0.3): zero the always-on instrumentation snapshot
       region so an uninitialized read returns 0 (deterministic baseline) instead of
       stale DRAM garbage that made IRQ-hold/FIFO snapshots untrustworthy. */
    {
        for (i = 0; i < 14; i++) wedge_snap[i] = 0u;
        __asm volatile("mcr p15,0,%0,c7,c10,4" :: "r"(0));   /* DSB: make it visible */
    }

    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;

    // Disable IRQ during setup
    synopsysotg_target_disable_irq(instance);

    // Enable OTG clocks
    synopsysotg_target_enable_clocks(instance);

    // Enable PHY clocks
    union synopsysotg_pcgcctl pcgcctl = { .b = {} };
    data->core->pcgcctl = pcgcctl;

    // Configure PHY type (must be done before reset)
    union synopsysotg_gccfg gccfg = { .b = { .disablevbussensing = 1, .pwdn = 0 } };
    data->core->gregs.gccfg = gccfg;
    union synopsysotg_gusbcfg gusbcfg = { .b = { .force_dev = 1, .usbtrdtim = SYNOPSYSOTG_TURNAROUND } };
    if (data->phy_16bit) gusbcfg.b.phyif = 1;
    else if (data->phy_ulpi) gusbcfg.b.ulpi_utmi_sel = 1;
    else gusbcfg.b.physel  = 1;
    data->core->gregs.gusbcfg = gusbcfg;

    // Reset the whole USB core
    union synopsysotg_grstctl grstctl = { .b = { .csftrst = 1 } };
    udelay(100);
    while (!data->core->gregs.grstctl.b.ahbidle);
    data->core->gregs.grstctl = grstctl;
    while (data->core->gregs.grstctl.b.csftrst);
    while (!data->core->gregs.grstctl.b.ahbidle);

    // Soft disconnect
    union synopsysotg_dctl dctl = { .b = { .sftdiscon = 1 } };
    data->core->dregs.dctl = dctl;

    // Configure the core
    union synopsysotg_gahbcfg gahbcfg = { .b = { .dmaenable = data->use_dma, .hburstlen = SYNOPSYSOTG_AHB_BURST_LEN, .glblintrmsk = 1 } };
    if (data->disable_double_buffering)
    {
        gahbcfg.b.nptxfemplvl_txfemplvl = 1;
        gahbcfg.b.ptxfemplvl = 1;
    }
    data->core->gregs.gahbcfg = gahbcfg;
    data->core->gregs.gusbcfg = gusbcfg;
    gccfg.b.pwdn = 1;
    data->core->gregs.gccfg = gccfg;
    union synopsysotg_dcfg dcfg = { .b = { .nzstsouthshk = 1 } };
    data->core->dregs.dcfg = dcfg;

    // Configure the FIFOs
    if (data->use_dma)
    {
        union synopsysotg_dthrctl dthrctl = { .b = { .arb_park_en = 1, .rx_thr_en = 1, .iso_thr_en = 0, .non_iso_thr_en = 0,
                                                     .rx_thr_len = SYNOPSYSOTG_AHB_THRESHOLD } };
#ifndef TARGET_ipodnano5g   /* nano 5G: Apple never writes DTHRCTL  */
        data->core->dregs.dthrctl = dthrctl;
#endif
    }
    int addr = data->fifosize;
    for (i = 0; i < 16; i++)
    {
        data->core->inep_regs[i].diepctl.b.nextep = (i + 1) & 0xf;
        int size = data->txfifosize[i];
        addr -= size;
        if (size)
        {
            union synopsysotg_txfsiz fsiz = { .b = { .startaddr = addr, .depth = size } };
            if (!i) data->core->gregs.dieptxf0_hnptxfsiz = fsiz;
            else data->core->gregs.dieptxf[i - 1] = fsiz;
        }
    }
    union synopsysotg_rxfsiz fsiz = { .b = { .depth = addr } };
    data->core->gregs.grxfsiz = fsiz;

    // Set up interrupts
    union synopsysotg_doepintn doepmsk =  { .b = { .xfercompl = 1, .setup = 1 } };
    data->core->dregs.doepmsk = doepmsk;
    union synopsysotg_diepintn diepmsk =  { .b = { .xfercompl = 1, .timeout = 1 } };
    data->core->dregs.diepmsk = diepmsk;
    data->core->dregs.diepempmsk.d32 = 0;
    union synopsysotg_daint daintmsk = { .ep = { .in = 0b0000000000000001, .out = 0b0000000000000001 } };
    data->core->dregs.daintmsk = daintmsk;
    union synopsysotg_gintmsk gintmsk =  { .b = { .usbreset = 1, .enumdone = 1, .outepintr = 1, .inepintr = 1 } };
    if (!data->use_dma) gintmsk.b.rxstsqlvl = 1;
    data->core->gregs.gintmsk = gintmsk;
    synopsysotg_flush_ints(instance);
    synopsysotg_target_clear_irq(instance);
    synopsysotg_target_enable_irq(instance);

    // Soft reconnect
#ifdef TARGET_ipodnano5g
    synopsysotg_target_run_mode(instance);   /* nano 5G: PHY run mode before the soft reconnect  */
#endif
    dctl.b.sftdiscon = 0;
    data->core->dregs.dctl = dctl;
}

void synopsysotg_exit(const struct usb_instance* instance)
{
    const struct synopsysotg_config* data = (const struct synopsysotg_config*)instance->driver_config;

    // Soft disconnect
    union synopsysotg_dctl dctl = { .b = { .sftdiscon = 1 } };
    data->core->dregs.dctl = dctl;

    // Disable IRQs
    synopsysotg_target_disable_irq(instance);

    // Disable clocks
    synopsysotg_target_disable_clocks(instance);
}

int synopsysotg_get_max_transfer_size(const struct usb_instance* data, union usb_endpoint_number ep)
{
    return 1023;
}

const struct usb_driver synopsysotg_driver =
{
    .init = synopsysotg_init,
    .exit = synopsysotg_exit,
    .ep0_start_rx = synopsysotg_ep0_start_rx,
    .ep0_start_tx = synopsysotg_ep0_start_tx,
    .start_rx = synopsysotg_start_rx,
    .start_tx = synopsysotg_start_tx,
    .get_stall = synopsysotg_get_stall,
    .set_stall = synopsysotg_set_stall,
    .set_address = synopsysotg_set_address,
    .configure_ep = synopsysotg_configure_ep,
    .unconfigure_ep = synopsysotg_unconfigure_ep,
    .get_max_transfer_size = synopsysotg_get_max_transfer_size,
};

