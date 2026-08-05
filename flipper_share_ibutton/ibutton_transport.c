#include "ibutton_transport.h"
#include "share.h" // FSH_* sizes, share_config.h (IBTN_TP_*), fsh_transport_send/receive_callback

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_rfid.h>

#include <one_wire/one_wire_host.h>
#include <one_wire/one_wire_slave.h>
#include <power/power_service/power.h>

#define TAG "IbtnTransport"

// One length byte prefixes every payload on the wire, so a whole flipper-share
// packet must fit in it.
_Static_assert(FSH_PACKET_MAX <= 255, "flipper-share packet must fit a single length byte");

// One queued packet: [len][packet bytes]. len is the on-wire length prefix.
typedef struct {
    uint8_t len;
    uint8_t data[FSH_PACKET_MAX];
} IbtnTpPacket;

typedef struct {
    IbtnTransportMode mode;

    // Slave (sender) side: the emulation answers host transactions in the 1-Wire
    // interrupt; PUSH'd frames are handed to the RX worker for thread-context
    // delivery to the engine.
    OneWireSlave* slave;
    FuriMessageQueue* rx_queue; // ISR (command callback) -> IbtnRxWorker
    FuriThread* rx_worker;

    // Host (receiver) side: this worker owns the bus exclusively and drives the
    // POLL/PUSH loop.
    OneWireHost* host;
    FuriThread* host_worker;

    // Outbound DATA mailbox (slave only). fsh_transport_send() blocks up to
    // IBTN_TP_SEND_TIMEOUT_MS when full — the backpressure that paces the
    // sender's block stream.
    FuriMessageQueue* tx_queue;

    // Latest-wins control slot (ANNOUNCE on the slave, REQUEST on the host).
    // Kept out of tx_queue and always sent first, so a DATA stream can never
    // starve control traffic. Written by fsh_transport_send() under
    // FURI_CRITICAL; read by the slave command callback (ISR) or the host
    // worker. A newer control packet supersedes an older unsent one.
    IbtnTpPacket ctrl_pkt;
    volatile bool ctrl_valid;

    volatile bool worker_stop;
    volatile bool dormant; // host: pause polling after the transfer finishes
} IbtnTransport;

// Owned by the scene lifecycle: init in on_enter, deinit in on_exit, and no
// thread calls fsh_transport_send() during deinit (workers joined first).
static IbtnTransport* ibtn_tp = NULL;

// ===== Engine -> transport (TX) ==============================================

void fsh_transport_send(const uint8_t* buf, size_t len) {
    IbtnTransport* tp = ibtn_tp;
    if(!tp) return; // transport not running — drop, ARQ recovers
    if(len <= FSH_HEADER_LENGTH || len > FSH_PACKET_MAX) {
        FURI_LOG_E(TAG, "bad packet length %zu", len);
        return;
    }

    // ANNOUNCE / REQUEST go to the priority control slot (latest-wins, never
    // dropped by a full DATA queue); DATA goes to the paced FIFO. packet_type is
    // the 3rd header byte. The slave command callback reads the control slot from
    // the 1-Wire interrupt, so guard the write with FURI_CRITICAL (no mutex is
    // usable from that context).
    if(buf[FSH_HEADER_LENGTH - 1] != FSH_PKT_DATA) {
        FURI_CRITICAL_ENTER();
        tp->ctrl_pkt.len = (uint8_t)len;
        memcpy(tp->ctrl_pkt.data, buf, len);
        tp->ctrl_valid = true;
        FURI_CRITICAL_EXIT();
        return;
    }

    IbtnTpPacket pkt;
    pkt.len = (uint8_t)len;
    memcpy(pkt.data, buf, len);
    if(furi_message_queue_put(tp->tx_queue, &pkt, furi_ms_to_ticks(IBTN_TP_SEND_TIMEOUT_MS)) !=
       FuriStatusOk) {
        FURI_LOG_W(TAG, "TX mailbox full, DATA packet dropped");
    }
}

// ===== Slave (sender) side ===================================================

// Pick the next outbound packet for a POLL: control slot first, then the DATA
// queue. Runs in the 1-Wire interrupt/critical context.
static bool ibtn_tp_pop_outbound_isr(IbtnTransport* tp, IbtnTpPacket* out) {
    // We are already inside the 1-Wire critical section (IRQ), so the
    // FURI_CRITICAL writer in fsh_transport_send cannot be mid-update here.
    if(tp->ctrl_valid) {
        *out = tp->ctrl_pkt;
        tp->ctrl_valid = false;
        return true;
    }
    // 0 timeout in IRQ context -> xQueueReceiveFromISR (ISR-safe).
    return furi_message_queue_get(tp->tx_queue, out, 0) == FuriStatusOk;
}

// Participate in normal-speed resets only (no overdrive between two Flippers).
static bool ibtn_tp_slave_reset_callback(bool is_short, void* context) {
    UNUSED(context);
    return !is_short;
}

// 1-Wire interrupt/critical context: no blocking furi calls, no mutexes, no
// storage I/O here. One command per reset (the host issues a fresh reset for the
// next transaction), so always return false.
static bool ibtn_tp_slave_command_callback(uint8_t command, void* context) {
    IbtnTransport* tp = context;
    OneWireSlave* bus = tp->slave;

    if(command == IBTN_TP_CMD_POLL) {
        IbtnTpPacket pkt;
        if(ibtn_tp_pop_outbound_isr(tp, &pkt)) {
            // len byte then the packet bytes. On a bus error the packet is
            // already dequeued and is lost — the engine's ARQ re-requests it.
            if(onewire_slave_send(bus, &pkt.len, 1)) {
                onewire_slave_send(bus, pkt.data, pkt.len);
            }
        } else {
            const uint8_t nothing = 0x00; // len = 0: nothing queued
            onewire_slave_send(bus, &nothing, 1);
        }
    } else if(command == IBTN_TP_CMD_PUSH) {
        uint8_t len = 0;
        // Invalid length aborts the transaction (leave the bus idle; the next
        // reset resynchronizes).
        if(onewire_slave_receive(bus, &len, 1) && len >= 1 && len <= FSH_PACKET_MAX) {
            IbtnTpPacket frame;
            frame.len = len;
            if(onewire_slave_receive(bus, frame.data, len)) {
                // 0 timeout in IRQ context -> xQueueSendToBackFromISR; drop on a
                // full queue (ARQ recovers).
                furi_message_queue_put(tp->rx_queue, &frame, 0);
            }
        }
    }
    // Any other command (incl. standard 1-Wire ROM commands): ignore.

    return false;
}

// Drains PUSH'd frames and delivers them to the engine in thread context.
static int32_t ibtn_tp_rx_worker_thread(void* context) {
    IbtnTransport* tp = context;
    IbtnTpPacket frame;

    while(!tp->worker_stop) {
        // Timed get so the stop flag is checked promptly on deinit.
        if(furi_message_queue_get(tp->rx_queue, &frame, furi_ms_to_ticks(50)) == FuriStatusOk) {
            fsh_receive_callback(frame.data, frame.len);
        }
    }
    return 0;
}

// ===== Host (receiver) side ==================================================
// Owns the OneWireHost exclusively. Each iteration is one reset+presence
// transaction: PUSH a pending REQUEST, else POLL the slave for a packet.
//
// TIMING: the firmware's 1-Wire host bit-bangs with plain busy-wait delays and
// NO critical section, so a FreeRTOS preemption or a long interrupt lands in the
// middle of a bit slot and corrupts it. Under USB load (CDC/RPC/qFlipper) that
// happened on virtually every transaction — the host missed the presence pulse
// window entirely and the link looked dead. The slave side has no such problem
// (the firmware runs its whole transaction inside the EXTI critical section).
// So the host wraps the reset and each byte in a SHORT critical section
// (~0.6-1.2 ms of interrupts-off), leaving gaps between bytes for the system to
// breathe; the slave tolerates inter-byte gaps up to its 15 ms slot timeout.

static bool ibtn_tp_host_reset_atomic(OneWireHost* host) {
    FURI_CRITICAL_ENTER();
    bool present = onewire_host_reset(host);
    FURI_CRITICAL_EXIT();
    return present;
}

static void ibtn_tp_host_write_atomic(OneWireHost* host, uint8_t byte) {
    FURI_CRITICAL_ENTER();
    onewire_host_write(host, byte);
    FURI_CRITICAL_EXIT();
}

static uint8_t ibtn_tp_host_read_atomic(OneWireHost* host) {
    FURI_CRITICAL_ENTER();
    uint8_t value = onewire_host_read(host);
    FURI_CRITICAL_EXIT();
    return value;
}

static void ibtn_tp_host_write_bytes_atomic(OneWireHost* host, const uint8_t* buf, size_t len) {
    for(size_t i = 0; i < len; i++) ibtn_tp_host_write_atomic(host, buf[i]);
}

static void ibtn_tp_host_read_bytes_atomic(OneWireHost* host, uint8_t* buf, size_t len) {
    for(size_t i = 0; i < len; i++) buf[i] = ibtn_tp_host_read_atomic(host);
}

static int32_t ibtn_tp_host_worker_thread(void* context) {
    IbtnTransport* tp = context;
    uint8_t buf[FSH_PACKET_MAX];

    while(!tp->worker_stop) {
        if(tp->dormant) {
            furi_delay_ms(50);
            continue;
        }

        // Peek whether a control packet (REQUEST) is pending; only consume it
        // once presence is confirmed, so a missed touch keeps it queued.
        FURI_CRITICAL_ENTER();
        bool have_ctrl = tp->ctrl_valid;
        FURI_CRITICAL_EXIT();

        bool present = ibtn_tp_host_reset_atomic(tp->host);

        if(present && have_ctrl) {
            IbtnTpPacket ctrl;
            bool got = false;
            FURI_CRITICAL_ENTER();
            if(tp->ctrl_valid) {
                ctrl = tp->ctrl_pkt;
                tp->ctrl_valid = false;
                got = true;
            }
            FURI_CRITICAL_EXIT();
            if(got) {
                ibtn_tp_host_write_atomic(tp->host, IBTN_TP_CMD_PUSH);
                ibtn_tp_host_write_atomic(tp->host, ctrl.len);
                ibtn_tp_host_write_bytes_atomic(tp->host, ctrl.data, ctrl.len);
            }
        } else if(present) {
            ibtn_tp_host_write_atomic(tp->host, IBTN_TP_CMD_POLL);
            uint8_t len = ibtn_tp_host_read_atomic(tp->host);
            // len == 0: nothing queued. Out of range: glitch -> abort, the next
            // reset resynchronizes.
            if(len >= 1 && len <= FSH_PACKET_MAX) {
                ibtn_tp_host_read_bytes_atomic(tp->host, buf, len);
                fsh_receive_callback(buf, len);
            }
        }

        furi_delay_ms(present ? IBTN_TP_POLL_INTERVAL_MS : IBTN_TP_RECONNECT_MS);
    }
    return 0;
}

// ===== Public API ============================================================

void ibutton_transport_init(IbtnTransportMode mode) {
    furi_assert(ibtn_tp == NULL);

    IbtnTransport* tp = malloc(sizeof(IbtnTransport));
    memset(tp, 0, sizeof(*tp));
    tp->mode = mode;
    tp->ctrl_valid = false;
    tp->worker_stop = false;
    tp->dormant = false;

    // Outbound DATA mailbox exists in both roles (the host never puts DATA into
    // it, but keeping the mailbox structure symmetric means fsh_transport_send()
    // needs no per-role branch). The RX queue is slave-only (the host receives
    // inline on its worker thread, not via an interrupt).
    tp->tx_queue = furi_message_queue_alloc(IBTN_TP_QUEUE_DEPTH, sizeof(IbtnTpPacket));

    if(mode == IbtnTransportModeSlave) {
        tp->rx_queue = furi_message_queue_alloc(IBTN_TP_QUEUE_DEPTH, sizeof(IbtnTpPacket));

        tp->slave = onewire_slave_alloc(IBTN_TP_GPIO);
        onewire_slave_set_reset_callback(tp->slave, ibtn_tp_slave_reset_callback, tp);
        onewire_slave_set_command_callback(tp->slave, ibtn_tp_slave_command_callback, tp);

        tp->rx_worker = furi_thread_alloc_ex("IbtnRxWorker", 2048, ibtn_tp_rx_worker_thread, tp);
        furi_thread_start(tp->rx_worker);

        // The iButton pad shares its analog path with the LF RFID front-end; the
        // stock key emulator grounds the RFID pull before emulating so the RFID
        // circuitry cannot load the 1-Wire line. Same here.
        furi_hal_rfid_pins_reset();
        furi_hal_rfid_pin_pull_pulldown();

        onewire_slave_start(tp->slave);
    } else {
        // The iButton line's pull-up is fed from the 5V rail, which is only
        // powered from USB or the OTG boost. Without it the bus floats and the
        // host sees phantom presence pulses (and reads only zeros). The stock
        // iButton reader enables OTG for exactly this reason — do the same.
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, true);
        furi_record_close(RECORD_POWER);

        tp->host = onewire_host_alloc(IBTN_TP_GPIO);
        onewire_host_start(tp->host);

        tp->host_worker =
            furi_thread_alloc_ex("IbtnHostWorker", 2048, ibtn_tp_host_worker_thread, tp);
        // High priority so the worker is not preempted for long BETWEEN the
        // per-byte critical sections — a >15 ms gap mid-transaction would trip
        // the slave's slot timeout and drop the frame.
        furi_thread_set_priority(tp->host_worker, FuriThreadPriorityHigh);
        furi_thread_start(tp->host_worker);
    }

    ibtn_tp = tp; // publish only when fully started
    FURI_LOG_I(TAG, "started as %s", mode == IbtnTransportModeSlave ? "slave" : "host");
}

void ibutton_transport_deinit(void) {
    IbtnTransport* tp = ibtn_tp;
    if(!tp) return;
    ibtn_tp = NULL; // sends become no-ops first

    tp->worker_stop = true;

    if(tp->mode == IbtnTransportModeSlave) {
        // Stop the emulation first so the interrupt can no longer touch rx_queue,
        // then join the worker and free the queues.
        if(tp->slave) {
            onewire_slave_stop(tp->slave);
            onewire_slave_free(tp->slave);
        }
        if(tp->rx_worker) {
            furi_thread_join(tp->rx_worker);
            furi_thread_free(tp->rx_worker);
        }
        if(tp->rx_queue) furi_message_queue_free(tp->rx_queue);
        // Release the RFID pull grounded at init (mirrors the stock emulator).
        furi_hal_rfid_pins_reset();
    } else {
        if(tp->host_worker) {
            furi_thread_join(tp->host_worker);
            furi_thread_free(tp->host_worker);
        }
        if(tp->host) {
            onewire_host_stop(tp->host);
            onewire_host_free(tp->host);
        }
        // Drop the OTG 5V enabled at init (powers the pad pull-up, stock-reader style).
        Power* power = furi_record_open(RECORD_POWER);
        power_enable_otg(power, false);
        furi_record_close(RECORD_POWER);
    }

    if(tp->tx_queue) furi_message_queue_free(tp->tx_queue);
    free(tp);
    FURI_LOG_I(TAG, "stopped");
}

void ibutton_transport_stop_field(void) {
    // Only sets a flag observed by the host worker, so it is safe to call from
    // any thread. No-op for the slave (its emulation stops at deinit).
    IbtnTransport* tp = ibtn_tp;
    if(tp) tp->dormant = true;
}
