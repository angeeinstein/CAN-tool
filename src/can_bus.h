// TWAI (CAN) driver wrapper: modes, RX task, guarded transmit, error handling / bus-off recovery.
//
// One RX task owns the driver: it receives frames, handles alerts and performs every
// (re)configuration, so the driver is never uninstalled while another task is inside it.
// Transmission is only possible in ACTIVE mode; this is checked on every single frame.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "can_frame.h"

enum class CanMode : uint8_t { Closed, Listen, Active };

const char* can_mode_name(CanMode m);

void can_init();  // installs the driver in LISTEN mode and starts the RX task

// Synchronous reconfiguration (main task only). Waits until the RX task has applied it.
// Returns false on driver error (the bus is then CLOSED).
bool can_set_mode(CanMode m);
bool can_set_bitrate(uint32_t bps);  // keeps the current mode; false if unsupported
bool can_bitrate_supported(uint32_t bps);

// CTX/CRX pins. Only pins safe for this chip are accepted (see TX_PINS/RX_PINS in can_bus.cpp).
// Saved in NVS and applied immediately (bus re-opens in LISTEN). Main task only.
bool can_set_pins(int tx, int rx, char* err, size_t errcap);
void can_get_pins(int& tx, int& rx);
void can_pins_allowed(char* tx_out, size_t tx_cap, char* rx_out, size_t rx_cap);

CanMode can_mode();
uint32_t can_bitrate();
bool can_bus_off();

enum class TxResult : uint8_t { Ok, NotActive, RateLimited, Busy, Error };
const char* tx_result_name(TxResult r);

// Thread-safe. Applies the TX gate and the rate limiter; echoes to the host if enabled.
TxResult can_transmit(const CanFrame& f, const char* reason);

// Callable from any task, non-blocking: TX gate off, all rules disabled, pending actions and
// TX queue cleared, switch to LISTEN, "#stop <reason>" to the host.
// latch=true (default): sets the STOP latch (kept in NVS). While latched, ACTIVE, sending and
// adding/enabling rules are refused until can_resume() ("#resume"). latch=false for a script's own cleanup.
void can_emergency_stop(const char* reason, bool latch = true);
bool can_stop_latched();
uint32_t can_stop_count();   // latched stops since the last #resume
void can_resume();

// ---- ACTIVE watchdog: revert to LISTEN if the client that enabled ACTIVE disappears ----
void can_active_set_owner(uint8_t sink, uint32_t web_client);  // call on a successful switch to ACTIVE
uint8_t can_active_owner();          // sink (SINK_*) that owns ACTIVE, 0 = none
uint32_t can_active_owner_client();  // web client id, if the owner is SINK_WEB
void can_watchdog_tick(bool owner_alive);   // call ~1/s from the main task; reverts after the timeout
uint32_t can_watchdog_secs();        // 0 = off
void can_set_watchdog(uint32_t secs);

void can_set_rate_limit(uint16_t per_sec);
uint16_t can_rate_limit();

struct CanStats {
    uint32_t rx;
    uint32_t tx;
    uint32_t tx_errors;
    uint32_t bus_off_count;
    uint32_t tec;
    uint32_t rec;
    uint32_t rx_missed;       // driver RX queue full
    uint32_t rx_overrun;      // hardware FIFO overrun
    uint32_t bus_errors;
    uint32_t arb_lost;
    uint32_t tx_failed;       // driver statistic
    const char* state;
};
void can_get_stats(CanStats& s);
void can_reset_stats();

// SLCAN 'F' status flags (bit0 RX full, 2 err warning, 3 overrun, 5 err passive, 6 arb lost, 7 bus err).
// Reading clears the latched flags.
uint8_t can_take_slcan_flags();

uint32_t can_last_rx_ms();
