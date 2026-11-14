/**
 * Node.js ESM FFI binding for libzcrudp (via koffi when installed).
 *
 * Build the shared library (`build/libzcrudp.so`, `libzcrudp.dylib`, or `zcrudp.dll`)
 * and pass its path (or set ZCRUDP_LIB) when instantiating `loadZcrudp()`.
 */
import path from "node:path";
import { fileURLToPath } from "node:url";

export const RUDP_OK = 0;
export const RUDP_ERR_INVALID_ARG = -1;
export const RUDP_ERR_DISCONNECTED = -2;
export const RUDP_ERR_BUFFER_FULL = -3;
export const RUDP_ERR_OUT_OF_WINDOW = -4;
export const RUDP_ERR_AUTH = -5;

export const RUDP_CHANNEL_FLAG_UNRELIABLE = 0;
export const RUDP_CHANNEL_FLAG_RELIABLE = 1 << 0;
export const RUDP_CHANNEL_FLAG_ORDERED = 1 << 1;
export const RUDP_CHANNEL_FLAG_ENCRYPTED = 1 << 2;
export const RUDP_CHANNEL_FLAG_FEC_K2 = 1 << 3;

export const RUDP_MAX_DATAGRAM_SIZE = 1404;
export const RUDP_CRYPTO_KEY_BYTES = 32;
export const RUDP_CRYPTO_OVERHEAD_BYTES = 24;
export const RUDP_SESSION_ABI_BYTES = 5364;
export const RUDP_CRYPTO_CTX_ABI_BYTES = 160;

export function resolveLibraryPath(explicitPath) {
  if (explicitPath) return explicitPath;
  if (process.env.ZCRUDP_LIB) return process.env.ZCRUDP_LIB;
  const __dirname = path.dirname(fileURLToPath(import.meta.url));
  const ext =
    process.platform === "win32"
      ? "zcrudp.dll"
      : process.platform === "darwin"
        ? "libzcrudp.dylib"
        : "libzcrudp.so";
  return path.resolve(__dirname, "../../build", ext);
}

/**
 * Loads libzcrudp using `koffi` (`npm install koffi`) and returns bound Session / Crypto classes.
 */
export async function loadZcrudp(explicitPath) {
  const { default: koffi } = await import("koffi");
  const lib = koffi.load(resolveLibraryPath(explicitPath));

  const rudp_session_init = lib.func("int rudp_session_init(void *)");
  const rudp_session_config_channel = lib.func(
    "int rudp_session_config_channel(void *, uint8_t, uint8_t)",
  );
  const rudp_session_config_recovery = lib.func(
    "int rudp_session_config_recovery(void *, uint8_t, uint32_t, uint32_t, uint32_t)",
  );
  const rudp_session_send_reliable = lib.func(
    "int rudp_session_send_reliable(void *, uint8_t, uint32_t, uint32_t)",
  );
  const rudp_session_queue_unreliable = lib.func(
    "int rudp_session_queue_unreliable(void *, uint8_t, uint32_t)",
  );
  const rudp_session_build_datagram = lib.func(
    "int rudp_session_build_datagram(void *, uint8_t, uint8_t *, size_t, uint32_t, uint32_t)",
  );
  const rudp_session_has_pending_work = lib.func(
    "bool rudp_session_has_pending_work(const void *, uint32_t, uint32_t)",
  );
  const rudp_session_process_datagram_at = lib.func(
    "int rudp_session_process_datagram_at(void *, const uint8_t *, size_t, void *, size_t, uint32_t)",
  );
  const rudp_crypto_init = lib.func(
    "int rudp_crypto_init(void *, const uint8_t *, uint64_t, uint8_t)",
  );
  const rudp_crypto_seal = lib.func(
    "int rudp_crypto_seal(void *, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *, size_t)",
  );
  const rudp_crypto_open = lib.func(
    "int rudp_crypto_open(void *, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *, size_t)",
  );
  const rudp_crypto_wipe = lib.func("void rudp_crypto_wipe(void *)");

  function check(rc, op) {
    if (rc < 0) {
      throw new Error(`zcrudp: ${op} failed with error ${rc}`);
    }
    return rc;
  }

  class Session {
    constructor() {
      this._buf = Buffer.alloc(8192);
      check(rudp_session_init(this._buf), "rudp_session_init");
    }

    configChannel(channelId, flags) {
      check(
        rudp_session_config_channel(this._buf, channelId, flags),
        "rudp_session_config_channel",
      );
    }

    configRecovery(channelId, initialMs, minMs, maxMs) {
      check(
        rudp_session_config_recovery(this._buf, channelId, initialMs, minMs, maxMs),
        "rudp_session_config_recovery",
      );
    }

    sendReliable(channelId, rawTfv, nowMs = 0) {
      check(
        rudp_session_send_reliable(this._buf, channelId, rawTfv >>> 0, nowMs >>> 0),
        "rudp_session_send_reliable",
      );
    }

    queueUnreliable(channelId, rawTfv) {
      check(
        rudp_session_queue_unreliable(this._buf, channelId, rawTfv >>> 0),
        "rudp_session_queue_unreliable",
      );
    }

    hasPendingWork(nowMs, fallbackTimeoutMs = 100) {
      return Boolean(
        rudp_session_has_pending_work(this._buf, nowMs >>> 0, fallbackTimeoutMs >>> 0),
      );
    }

    buildDatagram(
      primaryAckChannel = 0,
      nowMs = 0,
      fallbackTimeoutMs = 100,
      maxLen = RUDP_MAX_DATAGRAM_SIZE,
    ) {
      const out = Buffer.alloc(maxLen);
      const n = check(
        rudp_session_build_datagram(
          this._buf,
          primaryAckChannel,
          out,
          out.length,
          nowMs >>> 0,
          fallbackTimeoutMs >>> 0,
        ),
        "rudp_session_build_datagram",
      );
      return out.subarray(0, n);
    }

    processDatagramAt(inputBuf, nowMs = 0, maxRecords = 175) {
      const recBuf = Buffer.alloc(maxRecords * 8);
      const n = check(
        rudp_session_process_datagram_at(
          this._buf,
          inputBuf,
          inputBuf.length,
          recBuf,
          maxRecords,
          nowMs >>> 0,
        ),
        "rudp_session_process_datagram_at",
      );
      const out = [];
      for (let i = 0; i < n; i++) {
        const off = i * 8;
        out.push({
          channelId: recBuf.readUInt8(off),
          flags: recBuf.readUInt8(off + 1),
          seqNum: recBuf.readUInt16LE(off + 2),
          rawPayload: recBuf.readUInt32LE(off + 4),
        });
      }
      return out;
    }
  }

  class CryptoCtx {
    constructor(sharedKey, sessionIdBigInt, initiator) {
      if (sharedKey.length !== RUDP_CRYPTO_KEY_BYTES) {
        throw new RangeError("sharedKey must be 32 bytes");
      }
      this._buf = Buffer.alloc(256);
      check(
        rudp_crypto_init(
          this._buf,
          sharedKey,
          sessionIdBigInt,
          initiator ? 1 : 0,
        ),
        "rudp_crypto_init",
      );
    }

    seal(plaintext, aad = null) {
      const out = Buffer.alloc(plaintext.length + RUDP_CRYPTO_OVERHEAD_BYTES);
      const n = check(
        rudp_crypto_seal(
          this._buf,
          aad,
          aad ? aad.length : 0,
          plaintext,
          plaintext.length,
          out,
          out.length,
        ),
        "rudp_crypto_seal",
      );
      return out.subarray(0, n);
    }

    open(wire, aad = null) {
      const out = Buffer.alloc(Math.max(1, wire.length - RUDP_CRYPTO_OVERHEAD_BYTES));
      const n = check(
        rudp_crypto_open(
          this._buf,
          aad,
          aad ? aad.length : 0,
          wire,
          wire.length,
          out,
          out.length,
        ),
        "rudp_crypto_open",
      );
      return out.subarray(0, n);
    }

    wipe() {
      rudp_crypto_wipe(this._buf);
    }
  }

  return { Session, CryptoCtx };
}
