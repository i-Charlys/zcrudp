# zcrudp

A zero-malloc, 32-bit fixed-frame Reliable UDP library in pure C.

## Description

`zcrudp` is a lightweight, high-performance Reliable UDP (RUDP) implementation designed for embedded systems and performance-critical applications. It operates with zero dynamic memory allocation, using a fixed-frame 32-bit structure for both headers and data.

## Key Features

- **Zero-malloc**: All memory is managed through static or stack-allocated contexts.
- **Fixed-frame RUDP**: Optimized for 32-bit architectures with a total frame size of 8 bytes (4-byte header + 4-byte payload).
- **Cumulative ACKs**: Implements a sliding window (default 64 slots) with cumulative acknowledgment logic.
- **Retransmission**: Built-in timeout handling and retransmission tracking.
- **TLV Integration**: Uses a Type-Length-Value (TLV) structure for the 32-bit payload.
- **Sequence Rollover**: Robust handling of 16-bit sequence number wraparound.

## Project Structure

```text
.
├── include/
│   ├── protocol_rudp.h    # Core RUDP definitions and context
│   └── protocol_tlv.h     # 32-bit TLV packet structure
├── src/
│   └── rudp.c             # RUDP implementation logic
└── tests/
    ├── test_rudp.c        # RUDP engine and extreme case tests
    └── test_tlv.c         # TLV structure validation
```

## Quick Start / Usage

### Basic Initialization and Sending

```c
#include "protocol_rudp.h"

rudp_context_s ctx;
rudp_init(&ctx);

tlv_packet_u packet;
packet.type = 1;
packet.flags = 0;
packet.value = 123;

// Send a packet at current timestamp (1000ms)
rudp_send(&ctx, packet, 1000);
```

### Handling ACKs and Retransmissions

```c
// Receive an ACK for sequence number 0
rudp_recv_ack(&ctx, 0);

// Check for timed-out packets (100ms timeout)
uint16_t expired_slots[64];
int count = rudp_tick(&ctx, 1200, 100, expired_slots, 64);

for (int i = 0; i < count; i++) {
    // Resend packet at expired_slots[i]
}
```

## Building & Testing

The project includes a `Makefile` for easy compilation and testing.

### Run all tests
```bash
make test
```

### Build individual tests
```bash
make test_rudp
make test_tlv
```

### Manual compilation (alternative)
If you don't have `make`, you can compile manually (ensure the `build/` directory exists):
```bash
mkdir -p build
gcc -Iinclude src/rudp.c tests/test_rudp.c -o build/test_rudp
./build/test_rudp
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
