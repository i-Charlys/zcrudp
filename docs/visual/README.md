# Packet replay

Open [index.html](index.html) locally in a browser. It is self-contained: no web
server, CDN, framework, network request or package installation is required to
replay it. GitHub shows the [GIF](recovery.gif); the HTML must be downloaded/opened
locally or served as static files. [Static overview](poster.png).

The renderer replays `trace.json`, produced by compiling `bench/trace_rudp.c`
against the actual `src/rudp.c`. Source hashes, compiler and build command are
embedded in the trace. No alternative implementation of RUDP is used to create
events, receive-buffer contents, delivery counters, retries or RTO values.

## Scripted scenario, not a benchmark

- Two session instances; reliable channel 0 and unreliable channel 1.
- 8 reliable messages, every 20 ms; 12 unreliable messages, every 15 ms.
- Default link delay 15 ms each way. First R2 transmission and U9 are dropped.
- First R3 transmission takes 70 ms; U4 takes 80 ms and arrives stale.
- Adaptive recovery starts at 100 ms, with base-RTO bounds 10..2,000 ms.
- 360 ms of simulated time, replayed approximately 30 times slower, with an
  end hold. This intentionally differs from the comparative benchmark workload.
- Packet icons interpolate between actual simulated send/arrival times. A red
  cross in the middle is a graphical convention, not a measured loss location.
- 8 reliable events are delivered in order and acknowledged; 10 fresh updates
  are delivered, one stale update is ignored. The trace asserts that fresh
  delivery continues while the reliable RX window holds ahead-of-order events.
- Displayed bytes/session are `sizeof(rudp_session_s)` from the compiled host
  ABI, excluding the application, simulator, socket buffers and visualization.

Amber packet icons denote retransmissions; amber RX cells denote retained
out-of-order messages. Green RX cells denote delivered reliable messages. ACK
labels indicate the next expected reliable sequence, not the previous one.

## Reproduce

```bash
make visual-trace  # C compiler + Python standard library: JSON and offline HTML

# Only GIF generation needs browser/imaging tools, never the protocol core:
python3 -m venv /tmp/zcrudp-visual-env
/tmp/zcrudp-visual-env/bin/pip install playwright==1.62.0 pillow==12.3.0
PLAYWRIGHT_BROWSERS_PATH=/tmp/zcrudp-playwright /tmp/zcrudp-visual-env/bin/python -m playwright install chromium
PLAYWRIGHT_BROWSERS_PATH=/tmp/zcrudp-playwright make visual-report VISUAL_PYTHON=/tmp/zcrudp-visual-env/bin/python
```

Browser rendering may vary slightly with fonts/tool versions; the trace is
deterministic and generated twice for comparison. GIF capture verifies every
sampled state against the trace, checks seek/replay controls and JavaScript errors.
Reduced-motion preferences pause the interactive replay initially. All output
files are regenerable; no competitor performance claim is made by this animation.
