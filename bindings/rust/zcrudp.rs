//! Rust FFI binding and safe wrappers for the zcrudp C11 library.
//!
//! Link against `libzcrudp` (e.g. `println!("cargo:rustc-link-lib=zcrudp");`).
//! Zero dynamic allocation is performed by the C library; storage lives on the
//! stack or caller-owned static buffers.

use std::mem::MaybeUninit;
use std::os::raw::c_int;

pub const RUDP_OK: c_int = 0;
pub const RUDP_ERR_INVALID_ARG: c_int = -1;
pub const RUDP_ERR_DISCONNECTED: c_int = -2;
pub const RUDP_ERR_BUFFER_FULL: c_int = -3;
pub const RUDP_ERR_OUT_OF_WINDOW: c_int = -4;
pub const RUDP_ERR_AUTH: c_int = -5;

pub const RUDP_CHANNEL_FLAG_UNRELIABLE: u8 = 0;
pub const RUDP_CHANNEL_FLAG_RELIABLE: u8 = 1 << 0;
pub const RUDP_CHANNEL_FLAG_ORDERED: u8 = 1 << 1;
pub const RUDP_CHANNEL_FLAG_ENCRYPTED: u8 = 1 << 2;
pub const RUDP_CHANNEL_FLAG_FEC_K2: u8 = 1 << 3;

pub const RUDP_MAX_DATAGRAM_SIZE: usize = 1404;
pub const RUDP_CRYPTO_KEY_BYTES: usize = 32;
pub const RUDP_CRYPTO_OVERHEAD_BYTES: usize = 24;
pub const RUDP_SESSION_ABI_BYTES: usize = 5364;

#[repr(C)]
#[derive(Copy, Clone)]
pub union TfvPacket {
    pub raw: u32,
    pub bytes: [u8; 4],
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct RudpRecord {
    pub channel_id: u8,
    pub flags: u8,
    pub seq_num: u16,
    pub payload: TfvPacket,
}

#[repr(C, align(8))]
pub struct RudpSessionStorage {
    _opaque: [u8; 8192], // Covers default 5364B ABI & larger configured windows
}

#[repr(C, align(8))]
pub struct RudpCryptoCtxStorage {
    _opaque: [u8; 256], // Covers rudp_crypto_ctx_s
}

extern "C" {
    pub fn rudp_session_init(session: *mut RudpSessionStorage) -> c_int;
    pub fn rudp_session_config_channel(
        session: *mut RudpSessionStorage,
        channel_id: u8,
        flags: u8,
    ) -> c_int;
    pub fn rudp_session_config_recovery(
        session: *mut RudpSessionStorage,
        channel_id: u8,
        initial_ms: u32,
        min_ms: u32,
        max_ms: u32,
    ) -> c_int;
    pub fn rudp_session_send_reliable(
        session: *mut RudpSessionStorage,
        channel_id: u8,
        payload: TfvPacket,
        now_ms: u32,
    ) -> c_int;
    pub fn rudp_session_queue_unreliable(
        session: *mut RudpSessionStorage,
        channel_id: u8,
        payload: TfvPacket,
    ) -> c_int;
    pub fn rudp_session_build_datagram(
        session: *mut RudpSessionStorage,
        primary_ack_channel: u8,
        out_buf: *mut u8,
        max_len: usize,
        now_ms: u32,
        timeout_ms: u32,
    ) -> c_int;
    pub fn rudp_session_has_pending_work(
        session: *const RudpSessionStorage,
        now_ms: u32,
        fallback_timeout_ms: u32,
    ) -> bool;
    pub fn rudp_session_next_wakeup_ms(
        session: *const RudpSessionStorage,
        now_ms: u32,
        fallback_timeout_ms: u32,
    ) -> u32;
    pub fn rudp_session_process_datagram_at(
        session: *mut RudpSessionStorage,
        in_buf: *const u8,
        in_len: usize,
        out_delivered: *mut RudpRecord,
        max_delivered: usize,
        now_ms: u32,
    ) -> c_int;
    pub fn rudp_session_poll(
        session: *mut RudpSessionStorage,
        out_delivered: *mut RudpRecord,
        capacity: usize,
    ) -> c_int;

    pub fn rudp_pack_payload(payload: TfvPacket, out_buf: *mut u8, max_len: usize) -> c_int;
    pub fn rudp_unpack_payload(
        in_buf: *const u8,
        len: usize,
        out_payload: *mut TfvPacket,
    ) -> c_int;

    pub fn rudp_crypto_init(
        ctx: *mut RudpCryptoCtxStorage,
        shared_key: *const u8,
        session_id: u64,
        local_is_initiator: u8,
    ) -> c_int;
    pub fn rudp_crypto_seal(
        ctx: *mut RudpCryptoCtxStorage,
        aad: *const u8,
        aad_len: usize,
        plaintext: *const u8,
        pt_len: usize,
        out_wire: *mut u8,
        max_out_len: usize,
    ) -> c_int;
    pub fn rudp_crypto_open(
        ctx: *mut RudpCryptoCtxStorage,
        aad: *const u8,
        aad_len: usize,
        in_wire: *const u8,
        in_wire_len: usize,
        out_plaintext: *mut u8,
        max_pt_len: usize,
    ) -> c_int;
    pub fn rudp_crypto_wipe(ctx: *mut RudpCryptoCtxStorage);
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct Error(pub c_int);

#[inline]
fn map_res(code: c_int) -> Result<usize, Error> {
    if code < 0 {
        Err(Error(code))
    } else {
        Ok(code as usize)
    }
}

pub struct Session {
    raw: RudpSessionStorage,
}

impl Session {
    pub fn new() -> Result<Self, Error> {
        let mut raw = MaybeUninit::<RudpSessionStorage>::uninit();
        let rc = unsafe { rudp_session_init(raw.as_mut_ptr()) };
        map_res(rc)?;
        Ok(Self {
            raw: unsafe { raw.assume_init() },
        })
    }

    pub fn config_channel(&mut self, channel_id: u8, flags: u8) -> Result<(), Error> {
        map_res(unsafe { rudp_session_config_channel(&mut self.raw, channel_id, flags) })?;
        Ok(())
    }

    pub fn config_recovery(
        &mut self,
        channel_id: u8,
        initial_ms: u32,
        min_ms: u32,
        max_ms: u32,
    ) -> Result<(), Error> {
        map_res(unsafe {
            rudp_session_config_recovery(&mut self.raw, channel_id, initial_ms, min_ms, max_ms)
        })?;
        Ok(())
    }

    pub fn send_reliable(&mut self, channel_id: u8, pkt: TfvPacket, now_ms: u32) -> Result<(), Error> {
        map_res(unsafe { rudp_session_send_reliable(&mut self.raw, channel_id, pkt, now_ms) })?;
        Ok(())
    }

    pub fn queue_unreliable(&mut self, channel_id: u8, pkt: TfvPacket) -> Result<(), Error> {
        map_res(unsafe { rudp_session_queue_unreliable(&mut self.raw, channel_id, pkt) })?;
        Ok(())
    }

    pub fn has_pending_work(&self, now_ms: u32, fallback_timeout_ms: u32) -> bool {
        unsafe { rudp_session_has_pending_work(&self.raw, now_ms, fallback_timeout_ms) }
    }

    pub fn build_datagram(
        &mut self,
        primary_ack_channel: u8,
        out: &mut [u8],
        now_ms: u32,
        timeout_ms: u32,
    ) -> Result<usize, Error> {
        map_res(unsafe {
            rudp_session_build_datagram(
                &mut self.raw,
                primary_ack_channel,
                out.as_mut_ptr(),
                out.len(),
                now_ms,
                timeout_ms,
            )
        })
    }

    pub fn process_datagram_at(
        &mut self,
        input: &[u8],
        out: &mut [RudpRecord],
        now_ms: u32,
    ) -> Result<usize, Error> {
        map_res(unsafe {
            rudp_session_process_datagram_at(
                &mut self.raw,
                input.as_ptr(),
                input.len(),
                out.as_mut_ptr(),
                out.len(),
                now_ms,
            )
        })
    }

    pub fn poll(&mut self, out: &mut [RudpRecord]) -> Result<usize, Error> {
        map_res(unsafe { rudp_session_poll(&mut self.raw, out.as_mut_ptr(), out.len()) })
    }
}

pub struct CryptoCtx {
    raw: RudpCryptoCtxStorage,
}

impl CryptoCtx {
    pub fn new(
        shared_key: &[u8; RUDP_CRYPTO_KEY_BYTES],
        session_id: u64,
        initiator: bool,
    ) -> Result<Self, Error> {
        let mut raw = MaybeUninit::<RudpCryptoCtxStorage>::uninit();
        let rc = unsafe {
            rudp_crypto_init(
                raw.as_mut_ptr(),
                shared_key.as_ptr(),
                session_id,
                if initiator { 1 } else { 0 },
            )
        };
        map_res(rc)?;
        Ok(Self {
            raw: unsafe { raw.assume_init() },
        })
    }

    pub fn seal(&mut self, aad: &[u8], pt: &[u8], out: &mut [u8]) -> Result<usize, Error> {
        map_res(unsafe {
            rudp_crypto_seal(
                &mut self.raw,
                if aad.is_empty() { std::ptr::null() } else { aad.as_ptr() },
                aad.len(),
                pt.as_ptr(),
                pt.len(),
                out.as_mut_ptr(),
                out.len(),
            )
        })
    }

    pub fn open(&mut self, aad: &[u8], wire: &[u8], out: &mut [u8]) -> Result<usize, Error> {
        map_res(unsafe {
            rudp_crypto_open(
                &mut self.raw,
                if aad.is_empty() { std::ptr::null() } else { aad.as_ptr() },
                aad.len(),
                wire.as_ptr(),
                wire.len(),
                out.as_mut_ptr(),
                out.len(),
            )
        })
    }
}

impl Drop for CryptoCtx {
    fn drop(&mut self) {
        unsafe { rudp_crypto_wipe(&mut self.raw) };
    }
}
