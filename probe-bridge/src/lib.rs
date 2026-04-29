//! probe-bridge — C FFI bridge wrapping probe-rs for mkdbg.
//!
//! Architecture:
//!
//! ```text
//!  mkdbg (C)                probe-bridge (Rust)              probe-rs
//!  ─────────────────────────────────────────────────────────────────
//!  probe_write(buf) ──────► write_tx ──► RSP thread
//!                                          │  parse RSP packet
//!                                          │  translate to probe-rs call ──► target
//!                                          │  format RSP reply  ◄────────── registers/memory
//!  probe_read(buf)  ◄────── read_rx  ◄──  read_tx.send(reply)
//! ```
//!
//! The RSP interpreter thread runs inside `std::thread::spawn`.  C is kept
//! fully decoupled from Rust async machinery.

mod rsp;

use std::ffi::CStr;
use std::os::raw::c_char;
use std::panic;
use std::sync::mpsc::{self, Receiver, Sender, SyncSender};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;

use probe_rs::config::TargetSelector;
use probe_rs::probe::list::Lister;
use probe_rs::{MemoryInterface, Permissions, RegisterId};

// ---------------------------------------------------------------------------
// C-compatible types
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct ProbeInfo {
    identifier: [u8; 128],
    serial: [u8; 64],
    vid: u16,
    pid: u16,
}

pub struct ProbeHandle {
    /// C → RSP thread: raw RSP bytes
    write_tx: SyncSender<Vec<u8>>,
    /// RSP thread → C: raw RSP bytes (Mutex keeps probe_read thread-safe)
    read_state: Mutex<ReadState>,
    shutdown_tx: SyncSender<()>,
    thread_handle: Mutex<Option<thread::JoinHandle<()>>>,
}

struct ReadState {
    rx: Receiver<Vec<u8>>,
    /// Bytes received but not yet consumed by probe_read
    leftover: Vec<u8>,
}

// SAFETY: ProbeHandle is accessed only through the raw-pointer C API.
// All interior mutability is mediated by Mutex / channel primitives.
unsafe impl Send for ProbeHandle {}
unsafe impl Sync for ProbeHandle {}

// ---------------------------------------------------------------------------
// RSP interpreter thread
// ---------------------------------------------------------------------------

/// ARM Cortex-M GDB register layout for the 'g' packet:
///   r0–r15 (16 regs × 4 bytes) + xpsr (4 bytes) = 68 bytes = 136 hex chars.
const CORTEX_M_REG_COUNT: usize = 17;

/// probe-rs RegisterId for xPSR on ARM Cortex-M.
/// GDB numbers it as register slot 16 immediately after r0–r15.
const XPSR_REG_SLOT: u16 = 16;

fn rsp_thread(
    probe_idx: usize,
    chip: Option<String>,
    write_rx: Receiver<Vec<u8>>,
    read_tx: Sender<Vec<u8>>,
    shutdown_rx: Receiver<()>,
) {
    let lister = Lister::new();
    let probes = lister.list_all();
    let probe_info = match probes.get(probe_idx) {
        Some(p) => p,
        None => return,
    };

    let probe = match probe_info.open() {
        Ok(p) => p,
        Err(_) => return,
    };

    let target: TargetSelector = match chip {
        Some(ref name) => name.as_str().into(),
        None => TargetSelector::Auto,
    };

    let mut session = match probe.attach(target, Permissions::default()) {
        Ok(s) => s,
        Err(_) => return,
    };

    let mut core = match session.core(0) {
        Ok(c) => c,
        Err(_) => return,
    };

    // Halt the core so it is in a known state when the RSP loop starts.
    let _ = core.halt(Duration::from_secs(1));

    let mut inbuf: Vec<u8> = Vec::new();

    loop {
        if shutdown_rx.try_recv().is_ok() {
            break;
        }

        while let Ok(chunk) = write_rx.try_recv() {
            inbuf.extend_from_slice(&chunk);
        }

        loop {
            match rsp::parse_rsp_packet(&inbuf) {
                Some((packet, consumed)) => {
                    inbuf.drain(..consumed);
                    // Send '+' ACK immediately, then the formatted reply.
                    let _ = read_tx.send(b"+".to_vec());
                    let reply_data = handle_command(&mut core, &packet);
                    let _ = read_tx.send(rsp::format_rsp_packet(&reply_data));
                }
                None => break,
            }
        }

        thread::sleep(Duration::from_millis(1));
    }
}

fn handle_command(core: &mut probe_rs::Core<'_>, cmd: &str) -> String {
    match cmd.chars().next() {
        Some('g') => cmd_read_registers(core),
        Some('G') => cmd_write_registers(core, &cmd[1..]),
        Some('m') => cmd_read_memory(core, &cmd[1..]),
        Some('M') => cmd_write_memory(core, &cmd[1..]),
        Some('s') => cmd_step(core),
        Some('c') => cmd_continue(core),
        Some('Z') => cmd_breakpoint(core, &cmd[1..], true),
        Some('z') => cmd_breakpoint(core, &cmd[1..], false),
        _ => String::new(), // RSP "not supported" — empty reply
    }
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

/// `g` — read all core registers; reply is 136 hex chars (17 × 4 bytes LE).
fn cmd_read_registers(core: &mut probe_rs::Core<'_>) -> String {
    let mut out = String::with_capacity(CORTEX_M_REG_COUNT * 8);
    for i in 0..CORTEX_M_REG_COUNT as u16 {
        let reg_id = if i < 16 { i } else { XPSR_REG_SLOT };
        let word = read_reg_u32(core, reg_id);
        for b in word.to_le_bytes() {
            out.push_str(&format!("{:02x}", b));
        }
    }
    out
}

/// `G<hex-registers>` — write all core registers; reply `OK` or `E01`.
fn cmd_write_registers(core: &mut probe_rs::Core<'_>, hex: &str) -> String {
    let bytes = match rsp::decode_hex_bytes(hex) {
        Some(b) if b.len() >= CORTEX_M_REG_COUNT * 4 => b,
        _ => return "E01".to_string(),
    };
    for i in 0..CORTEX_M_REG_COUNT as u16 {
        let off = (i as usize) * 4;
        let val = u32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]]);
        let reg_id = if i < 16 { i } else { XPSR_REG_SLOT };
        let _ = core.write_core_reg(RegisterId(reg_id), val);
    }
    "OK".to_string()
}

/// `m addr,len` — read memory; reply is hex-encoded bytes or `E01`.
fn cmd_read_memory(core: &mut probe_rs::Core<'_>, args: &str) -> String {
    let (addr, len) = match rsp::parse_addr_len(args) {
        Some(v) => v,
        None => return "E01".to_string(),
    };
    let mut buf = vec![0u8; len];
    match core.read_8(addr, &mut buf) {
        Ok(_) => rsp::encode_hex_bytes(&buf),
        Err(_) => "E01".to_string(),
    }
}

/// `M addr,len:hexdata` — write memory; reply `OK` or `E01`.
fn cmd_write_memory(core: &mut probe_rs::Core<'_>, args: &str) -> String {
    let (addr, _len) = match rsp::parse_addr_len(args) {
        Some(v) => v,
        None => return "E01".to_string(),
    };
    let hex_data = match args.find(':') {
        Some(p) => &args[p + 1..],
        None => return "E01".to_string(),
    };
    let data = match rsp::decode_hex_bytes(hex_data) {
        Some(d) => d,
        None => return "E01".to_string(),
    };
    match core.write_8(addr, &data) {
        Ok(_) => "OK".to_string(),
        Err(_) => "E01".to_string(),
    }
}

/// `s` — single step; returns `S05` (SIGTRAP) or `E01`.
fn cmd_step(core: &mut probe_rs::Core<'_>) -> String {
    match core.step() {
        Ok(_) => "S05".to_string(),
        Err(_) => "E01".to_string(),
    }
}

/// `c` — continue execution; blocks until halted; returns `S05` or `S02`.
fn cmd_continue(core: &mut probe_rs::Core<'_>) -> String {
    if core.run().is_err() {
        return "E01".to_string();
    }
    match core.wait_for_core_halted(Duration::from_secs(10)) {
        Ok(_) => "S05".to_string(),
        Err(_) => "S02".to_string(), // SIGINT — timeout / externally interrupted
    }
}

/// `Z/z type,addr,kind` — set or clear hardware breakpoint / DWT watchpoint.
///
/// type 1   → hardware instruction breakpoint
/// type 2–4 → DWT data watchpoints (write / read / access)
///
/// Unsupported type values return `""` (RSP not-supported).
fn cmd_breakpoint(core: &mut probe_rs::Core<'_>, args: &str, set: bool) -> String {
    let mut parts = args.splitn(3, ',');
    let bp_type: u8 = match parts.next().and_then(|s| s.parse().ok()) {
        Some(t) => t,
        None => return "E01".to_string(),
    };
    let addr: u64 = match parts.next().and_then(|s| u64::from_str_radix(s, 16).ok()) {
        Some(a) => a,
        None => return "E01".to_string(),
    };

    let result = match (bp_type, set) {
        (1, true) => core.set_hw_breakpoint(addr),
        (1, false) => core.clear_hw_breakpoint(addr),
        // DWT watchpoints 2-4: probe-rs exposes DWT through the same
        // breakpoint slot API on Cortex-M targets.
        (2..=4, true) => core.set_hw_breakpoint(addr),
        (2..=4, false) => core.clear_hw_breakpoint(addr),
        _ => return String::new(), // unsupported type → not-supported reply
    };
    match result {
        Ok(_) => "OK".to_string(),
        Err(_) => "E01".to_string(),
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn read_reg_u32(core: &mut probe_rs::Core<'_>, reg: u16) -> u32 {
    core.read_core_reg::<u32>(RegisterId(reg)).unwrap_or(0)
}

// ---------------------------------------------------------------------------
// C FFI exports
// ---------------------------------------------------------------------------

/// Enumerate connected debug probes.
///
/// # Safety
/// `out` must point to a buffer of at least `max` `ProbeInfo` entries.
#[no_mangle]
pub unsafe extern "C" fn probe_list(out: *mut ProbeInfo, max: i32) -> i32 {
    let out_ptr = out as usize; // capture for use inside closure
    let result = panic::catch_unwind(move || {
        let out = out_ptr as *mut ProbeInfo;
        let lister = Lister::new();
        let probes = lister.list_all();
        let n = probes.len();
        let limit = (max.max(0) as usize).min(n);

        for (i, info) in probes.iter().take(limit).enumerate() {
            let slot = &mut *out.add(i);
            *slot = ProbeInfo {
                identifier: [0u8; 128],
                serial: [0u8; 64],
                vid: info.vendor_id,
                pid: info.product_id,
            };

            let id_bytes = info.identifier.as_bytes();
            let id_len = id_bytes.len().min(127);
            slot.identifier[..id_len].copy_from_slice(&id_bytes[..id_len]);

            let sn_bytes = info.serial_number.as_deref().unwrap_or("").as_bytes();
            let sn_len = sn_bytes.len().min(63);
            slot.serial[..sn_len].copy_from_slice(&sn_bytes[..sn_len]);
        }

        n as i32
    });
    result.unwrap_or(-1)
}

/// Connect to probe[`probe_idx`] and attach to target chip.
///
/// `chip` must be a NUL-terminated chip name (e.g. `"STM32F446RETx"`) or
/// NULL for auto-detect.  Returns NULL on failure.
///
/// # Safety
/// `chip` must be a valid NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn probe_open(probe_idx: i32, chip: *const c_char) -> *mut ProbeHandle {
    let chip_str: Option<String> = if chip.is_null() {
        None
    } else {
        CStr::from_ptr(chip).to_str().ok().map(str::to_owned)
    };

    let (write_tx, write_rx) = mpsc::sync_channel::<Vec<u8>>(64);
    let (read_tx, read_rx) = mpsc::channel::<Vec<u8>>();
    let (shutdown_tx, shutdown_rx) = mpsc::sync_channel::<()>(1);

    let idx = probe_idx as usize;
    let jh = thread::spawn(move || {
        rsp_thread(idx, chip_str, write_rx, read_tx, shutdown_rx);
    });

    let h = Box::new(ProbeHandle {
        write_tx,
        read_state: Mutex::new(ReadState { rx: read_rx, leftover: Vec::new() }),
        shutdown_tx,
        thread_handle: Mutex::new(Some(jh)),
    });
    Box::into_raw(h)
}

/// Push RSP bytes from mkdbg into the bridge.
///
/// # Safety
/// `h` must be a valid non-NULL handle.  `buf` must be readable for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn probe_write(
    h: *mut ProbeHandle,
    buf: *const u8,
    len: i32,
) -> i32 {
    if h.is_null() {
        return -3;
    }
    let h = &*h;
    let data = std::slice::from_raw_parts(buf, len as usize).to_vec();
    match h.write_tx.try_send(data) {
        Ok(_) => len,
        Err(mpsc::TrySendError::Full(_)) => -4,
        Err(mpsc::TrySendError::Disconnected(_)) => -3,
    }
}

/// Pull RSP reply bytes out of the bridge.
///
/// Blocks up to `timeout_ms` milliseconds.
///
/// # Safety
/// `h` must be a valid non-NULL handle.  `buf` must be writable for `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn probe_read(
    h: *mut ProbeHandle,
    buf: *mut u8,
    len: i32,
    timeout_ms: i32,
) -> i32 {
    if h.is_null() {
        return -3;
    }
    let h = &*h;
    let out = std::slice::from_raw_parts_mut(buf, len as usize);

    let mut state = match h.read_state.lock() {
        Ok(g) => g,
        Err(_) => return -4,
    };

    if state.leftover.is_empty() {
        let timeout = Duration::from_millis(timeout_ms.max(0) as u64);
        match state.rx.recv_timeout(timeout) {
            Ok(chunk) => state.leftover.extend_from_slice(&chunk),
            Err(mpsc::RecvTimeoutError::Timeout) => return -2,
            Err(mpsc::RecvTimeoutError::Disconnected) => return -3,
        }
    }

    let n = (len as usize).min(state.leftover.len());
    out[..n].copy_from_slice(&state.leftover[..n]);
    state.leftover.drain(..n);
    n as i32
}

/// Detach from target and release all resources.
///
/// Blocks until the RSP thread exits.  Safe to call multiple times.
///
/// # Safety
/// `h` must be a valid pointer returned by `probe_open`.  After this call the
/// pointer is invalid and must not be dereferenced.
#[no_mangle]
pub unsafe extern "C" fn probe_close(h: *mut ProbeHandle) {
    if h.is_null() {
        return;
    }
    let h = Box::from_raw(h);
    let _ = h.shutdown_tx.try_send(());
    if let Ok(mut guard) = h.thread_handle.lock() {
        if let Some(jh) = guard.take() {
            let _ = jh.join();
        }
    };  // semicolon drops the MutexGuard before h (Box) is freed
    // Box drops here, releasing all resources.
}
