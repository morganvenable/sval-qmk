# Single-Sensor Full-Rate Streaming: Firmware + Host Logger

## Goal

Stream one TMAG3001 sensor (3 axes, X/Y/Z as int16) at the maximum rate the
hardware can deliver, from the RP2040 to a host-side Python logger. The host
captures a sequential binary log with drop detection so data can be
reconstructed offline. This is a dedicated test firmware — no keyboard
functionality required.

---

## Existing Infrastructure to Reuse

All of this lives in **hall-qmk** (`C:\Users\morga\OneDrive\Documents\GitHub\hall-qmk`), branch `hall2`.

### TMAG3001 Driver (`keyboards/svalboard/tmag3001/`)
- **tmag3001.c** (835 lines) — async DMA-driven PIO I2C, non-blocking `tmag_scan()`
- **tmag3001.h** — public API: `tmag_init()`, `tmag_scan()`, `tmag_get()`, `tmag_get_bus()`
- **pio_i2c.c/h** + **i2c.pio.h** — PIO state machine I2C implementation
- **tmag_config.h** — pin assignments, bus config, `TMAG_SINGLE_CLUSTER` option

### Data Structures (already defined)
```c
typedef struct {
    int16_t x, y, z;   // 16-bit signed (12-bit ADC, sign-extended)
    bool valid;
} tmag_reading_t;
```

### Build System (`keyboards/svalboard/rules.mk`)
- `USE_HALL_MATRIX=yes` enables the TMAG driver, links tmag3001.c, pio_i2c.c, matrix_tmag3001.c
- `TMAG_CLUSTER=N` selects a single cluster (0-4) — only that bus is initialized
- Automatically forces `SERIAL_DRIVER=usart` (PIO occupied by I2C)

### Matrix Integration (`matrix_tmag3001.c`)
- `matrix_init_custom()` — deferred init (100ms delay for console readiness)
- `matrix_scan_custom()` — calls `tmag_scan()` non-blocking each cycle
- Current console report: ASCII at 5 Hz (200ms interval) — too slow for our purpose

### Console Reader (`qmk-console-viewer/`)
- **console_reader.py** — connects to QMK console HID (Usage Page 0xFF31), reads ASCII frames
- **app.py** — Flask + SocketIO, WebSocket streaming to browser
- Parses the `=== TMAG3001 Sensor Report ===` format via regex

---

## Transport Decision: Raw HID

**Why Raw HID instead of console:**
- Console HID is text-based, requires formatting overhead (sprintf per value)
- Raw HID sends binary directly — 6 bytes per sample instead of ~40 bytes ASCII
- 32 bytes/packet at 1ms polling = 32KB/s; for 6 bytes/sample that's ~5000 samples/s theoretical
- We can batch 4 samples per packet (24 bytes + 5 byte header = 29 bytes fits in 32)
- **4kHz effective sample rate** — far exceeds sensor read rate

**Why not CDC/virtser:**
- Raw HID is simpler (no serial port enumeration, baud rate config)
- 4kHz is well above what one sensor can produce (~250Hz per bus in current config, up to ~2.5kHz with single-sensor optimization)
- Raw HID is already used by Viable protocol in viable-qmk — familiar territory

**Actual bottleneck:** The TMAG3001 I2C read time at 400kHz. One sensor, 6 data bytes + addressing overhead ≈ ~200µs per read → ~5kHz max. With the existing DMA pipeline (which has some overhead for start/stop/address), realistic rate is probably ~1-3kHz for a single sensor. Raw HID at 4kHz has headroom.

---

## Packet Format (Firmware → Host)

```
Byte   Field           Type      Description
──────────────────────────────────────────────────
0      0xAA            uint8     Sync marker (distinguishes stream from other HID traffic)
1      seq             uint8     Rolling sequence number 0-255 (drop detection)
2-3    timestamp       uint16    Milliseconds since stream start (wraps at 65536)
4      count           uint8     Number of samples in this packet (1-4)
5-6    sample[0].x     int16     First sample, X axis (little-endian, raw Q12)
7-8    sample[0].y     int16     First sample, Y axis
9-10   sample[0].z     int16     First sample, Z axis
11-16  sample[1]       int16×3   Second sample (if count >= 2)
17-22  sample[2]       int16×3   Third sample (if count >= 3)
23-28  sample[3]       int16×3   Fourth sample (if count >= 4)
29     flags           uint8     Bit 0: sensor valid, Bit 1: error since last packet
30-31  reserved        uint8×2   Zero-filled (pad to 32 bytes)
```

Little-endian matches RP2040 native byte order — no conversion needed on firmware side.

---

## Firmware Changes

### Repository Setup

1. Create a new worktree + feature branch from hall-qmk:
   ```
   cd C:\Users\morga\OneDrive\Documents\GitHub\hall-qmk
   git worktree add ../hall-qmk-stream -b sensor-stream hall2
   ```
   All firmware work happens in `../hall-qmk-stream/`.

### New File: `keyboards/svalboard/sensor_stream.h`

Packet format defines and streaming API:
```c
#define STREAM_SYNC         0xAA
#define STREAM_SAMPLES_MAX  4
#define STREAM_PACKET_SIZE  32  // RAW_EPSIZE

typedef struct __attribute__((packed)) {
    uint8_t  sync;          // 0xAA
    uint8_t  seq;           // rolling counter
    uint16_t timestamp_ms;  // ms since start
    uint8_t  count;         // samples in packet (1-4)
    struct {
        int16_t x, y, z;
    } samples[STREAM_SAMPLES_MAX];
    uint8_t  flags;
    uint8_t  reserved[2];
} stream_packet_t;
_Static_assert(sizeof(stream_packet_t) == 32, "packet must be 32 bytes");

void sensor_stream_init(void);
void sensor_stream_task(void);  // call from housekeeping or matrix scan
```

### New File: `keyboards/svalboard/sensor_stream.c`

Core streaming logic:
```c
#include "sensor_stream.h"
#include "tmag3001.h"
#include "raw_hid.h"
#include "timer.h"

static stream_packet_t pkt;
static uint8_t seq = 0;
static uint16_t start_time = 0;
static bool started = false;

void sensor_stream_init(void) {
    start_time = timer_read();
    started = true;
    memset(&pkt, 0, sizeof(pkt));
}

void sensor_stream_task(void) {
    if (!started || !tmag_healthy()) return;

    // Run the sensor scan (non-blocking, returns quickly if DMA not done)
    bool new_data = tmag_scan();
    if (!new_data) return;  // No new reading yet

    // Get the single sensor reading
    tmag_reading_t *r = tmag_get(0);  // sensor index 0

    // Build packet header
    pkt.sync = STREAM_SYNC;
    pkt.seq = seq++;
    pkt.timestamp_ms = timer_elapsed(start_time);
    pkt.count = 1;  // one sample per packet for simplicity
    pkt.samples[0].x = r->x;
    pkt.samples[0].y = r->y;
    pkt.samples[0].z = r->z;
    pkt.flags = r->valid ? 0x01 : 0x00;

    raw_hid_send((uint8_t *)&pkt, sizeof(pkt));
}
```

Note: We start with 1 sample per packet. If the sensor read rate is lower than
the USB frame rate (likely), there's no benefit to batching. If we later find
the sensor outpaces USB, we accumulate into `samples[0..3]` and send when full
or when a USB frame boundary hits.

### Modifications to `matrix_tmag3001.c`

Minimal changes to existing file:
- In `matrix_scan_custom()`: after calling `tmag_scan()`, also call `sensor_stream_task()`
- Optionally suppress the existing 200ms ASCII console report (or leave it — it won't interfere at 5Hz)
- In `matrix_init_custom()`: after `tmag_init()` succeeds, call `sensor_stream_init()`

### New File: `keyboards/svalboard/keymaps/stream_test/rules.mk`

```makefile
RAW_ENABLE = yes
CONSOLE_ENABLE = yes       # keep for debug output
MOUSEKEY_ENABLE = no
RGBLIGHT_ENABLE = no       # reduce firmware size, free PIO if needed
VIA_ENABLE = no
VIAL_ENABLE = no
```

### New File: `keyboards/svalboard/keymaps/stream_test/keymap.c`

Minimal keymap — just enough for QMK to compile. Single layer, all KC_NO.

### New File: `keyboards/svalboard/keymaps/stream_test/config.h`

Any overrides needed (e.g., force single cluster).

### Build Command

```bash
qmk compile -kb svalboard/trackball/pmw3389/right -km stream_test \
  -e USE_HALL_MATRIX=yes -e TMAG_CLUSTER=0
```

This compiles with only bus 0 (4 sensors), and we only read sensor index 0.

---

## Host Side (qmk-console-viewer)

All host-side work happens in `C:\Users\morga\OneDrive\Documents\GitHub\qmk-console-viewer\`.

### Existing Infrastructure to Reuse

The qmk-console-viewer project already has everything we need except Raw HID support:

**`console_reader.py`** — HID device management pattern to follow:
- `ConsoleReader` class: connects via `hidapi`, Usage Page 0xFF31, auto-reconnect with exponential backoff
- `_enumerate_devices()` → `_find_console_device()` → `_open_device()` → read loop
- Eventlet-compatible: wraps blocking HID calls in `tpool.execute()`
- PID filtering: only connects to `PREFERRED_PID = 0x4044` (test firmware)
- Connection status callbacks for UI notification
- `MockConsoleReader` for testing without hardware

**`app.py`** — Flask + SocketIO server:
- Background task via `socketio.start_background_task()`
- WebSocket events for sensor data, connection status, logging control
- Calibration save/load (JSON files)
- C header export with optional LinearSolver pseudoinverse

**`calibrate_keys.py`** — has its own `HIDReader` class for direct HID access outside Flask

**`requirements.txt`** — currently: flask, flask-socketio, eventlet (needs `hidapi`, `numpy`)

**`linear_solver.py`** — NumPy-based weighted least squares, SVD pseudoinverse

### New File: `raw_hid_reader.py`

A `RawHIDReader` class that mirrors `ConsoleReader`'s connection management but targets
the Raw HID endpoint instead of the Console HID endpoint. This keeps concerns separate —
console_reader.py handles text, raw_hid_reader.py handles binary stream packets.

```python
class RawHIDReader:
    """Reads binary sensor stream from Raw HID endpoint."""

    RAW_USAGE_PAGE = 0xFF60   # QMK Raw HID usage page
    RAW_USAGE_ID = 0x61       # Raw HID usage ID
    PREFERRED_PID = 0x4044    # Same test firmware PID
    STREAM_SYNC = 0xAA        # Sync byte for stream packets
    PACKET_SIZE = 32           # RAW_EPSIZE

    def __init__(self, recordings_dir="recordings"):
        self.recordings_dir = Path(recordings_dir)
        self.recordings_dir.mkdir(exist_ok=True)
        self.device = None
        self.running = False
        self._connected = False
        # Callbacks
        self._on_connection_change = None
        self._on_packet = None

    # Connection management — same pattern as ConsoleReader:
    # _enumerate_devices(), _find_raw_hid_device(), _open_device(),
    # _connect_device(), _disconnect_device()
    # Only difference: filter by RAW_USAGE_PAGE/RAW_USAGE_ID instead of
    # QMK_USAGE_PAGE/CONSOLE_USAGE

    def _find_raw_hid_device(self):
        """Find Raw HID interface (Usage Page 0xFF60, Usage 0x61)."""
        devices = self._enumerate_devices()
        raw_devices = [
            d for d in devices
            if d.get('usage_page') == self.RAW_USAGE_PAGE
            and d.get('usage') == self.RAW_USAGE_ID
            and d['product_id'] == self.PREFERRED_PID
        ]
        return raw_devices[0] if raw_devices else None

    def start(self, on_packet):
        """Start reading with auto-reconnect. on_packet receives raw 32-byte packets."""
        # Same reconnect loop pattern as ConsoleReader.start()
        # Reads 32-byte packets, filters for STREAM_SYNC, calls on_packet(data)
```

**Key design decision**: `RawHIDReader` is a *connection manager + raw packet source*.
It does NOT parse the stream format — that's the logger's job. This keeps the reader
reusable for other Raw HID protocols (e.g., Viable commands) later.

### New File: `stream_logger.py`

Standalone CLI script for recording sensor streams. Uses `RawHIDReader` for connection,
adds packet parsing, sequence validation, and file output.

```
Usage: python stream_logger.py [--duration SECONDS] [--output DIR] [--vid VID] [--pid PID]

Defaults:
  --duration 120        (2 minutes, 0 = unlimited until ctrl-C)
  --output recordings/  (auto-named with timestamp)
  --vid 0xFEED          (QMK default VID)
  --pid 0x4044          (test firmware PID, matching console_reader.py)
```

**NOTE on VID/PID**: The existing console_reader.py uses PID 0x4044 but doesn't
specify a VID — it enumerates all devices by Usage Page. The CLAUDE.md for
qmk-console-viewer says VID is 0x303A (Espressif). For the test firmware in
hall-qmk the VID may be 0xFEED (QMK default) or different. The logger should
accept `--vid` and `--pid` overrides, but default to filtering by Usage Page
alone (like console_reader.py does) plus PID 0x4044.

#### Packet Parsing

```python
def parse_stream_packet(data: bytes) -> Optional[StreamPacket]:
    """Parse a 32-byte raw HID packet into structured data.

    Returns None if not a valid stream packet (wrong sync byte, etc.)
    """
    if len(data) < 32 or data[0] != 0xAA:
        return None

    seq = data[1]
    timestamp_ms = int.from_bytes(data[2:4], 'little')
    count = data[4]
    flags = data[29]

    if count < 1 or count > 4:
        return None  # invalid sample count

    samples = []
    for i in range(count):
        offset = 5 + i * 6
        x = int.from_bytes(data[offset:offset+2], 'little', signed=True)
        y = int.from_bytes(data[offset+2:offset+4], 'little', signed=True)
        z = int.from_bytes(data[offset+4:offset+6], 'little', signed=True)
        samples.append((x, y, z))

    return StreamPacket(
        seq=seq,
        timestamp_ms=timestamp_ms,
        samples=samples,
        valid=(flags & 0x01) != 0,
        error=(flags & 0x02) != 0,
    )
```

#### Sequence Tracking and Gap Handling

```python
class SequenceTracker:
    """Tracks rolling 8-bit sequence numbers, detects gaps."""

    def __init__(self):
        self.last_seq = None
        self.total_packets = 0
        self.dropped_packets = 0
        self.drop_events = []  # [(packet_index, gap_size, timestamp_ms)]

    def check(self, seq: int, timestamp_ms: int) -> int:
        """Returns number of dropped packets (0 if none)."""
        if self.last_seq is None:
            self.last_seq = seq
            self.total_packets += 1
            return 0

        expected = (self.last_seq + 1) & 0xFF
        gap = (seq - expected) & 0xFF

        if gap > 0:
            self.dropped_packets += gap
            self.drop_events.append((self.total_packets, gap, timestamp_ms))

        self.last_seq = seq
        self.total_packets += 1
        return gap
```

#### Recording State Machine

```python
class StreamRecording:
    """Accumulates samples during a recording session."""

    def __init__(self):
        self.samples = []       # list of (x, y, z) tuples
        self.timestamps = []    # per-sample timestamp_ms
        self.drop_flags = []    # True = gap placeholder
        self.raw_packets = bytearray()  # raw .bin backup
        self.seq_tracker = SequenceTracker()
        self.start_wall_time = None

    def add_packet(self, raw_data: bytes, parsed: StreamPacket):
        """Process one packet. Inserts gap placeholders if drops detected."""
        if self.start_wall_time is None:
            self.start_wall_time = datetime.now()

        # Always save raw packet for .bin backup
        self.raw_packets.extend(raw_data)

        # Check sequence
        gap = self.seq_tracker.check(parsed.seq, parsed.timestamp_ms)

        # Insert NaN placeholders for any dropped packets
        # (We don't know how many samples each dropped packet had,
        #  assume 1 per packet since that's our current firmware config)
        for _ in range(gap):
            self.samples.append((0, 0, 0))  # sentinel, flagged below
            self.timestamps.append(parsed.timestamp_ms)  # approximate
            self.drop_flags.append(True)

        # Add actual samples
        for sample in parsed.samples:
            self.samples.append(sample)
            self.timestamps.append(parsed.timestamp_ms)
            self.drop_flags.append(False)

    def save(self, output_dir: Path, prefix: str = "sensor_stream"):
        """Write .npz, .bin, and .meta.json files."""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        base = output_dir / f"{prefix}_{timestamp}"

        # 1. NumPy archive
        import numpy as np
        np.savez(
            f"{base}.npz",
            samples=np.array(self.samples, dtype=np.int16),       # (N, 3)
            timestamps=np.array(self.timestamps, dtype=np.uint16), # (N,)
            drop_flags=np.array(self.drop_flags, dtype=bool),      # (N,)
        )

        # 2. Raw binary backup (every packet byte-for-byte)
        with open(f"{base}.bin", 'wb') as f:
            f.write(self.raw_packets)

        # 3. Human-readable metadata
        duration = (datetime.now() - self.start_wall_time).total_seconds()
        n = len(self.samples)
        meta = {
            "start_time": self.start_wall_time.isoformat(),
            "duration_s": round(duration, 1),
            "total_samples": n,
            "total_packets": self.seq_tracker.total_packets,
            "dropped_packets": self.seq_tracker.dropped_packets,
            "drop_events": self.seq_tracker.drop_events,
            "effective_rate_hz": round(n / duration, 1) if duration > 0 else 0,
            "sensor_config": "single sensor, cluster 0, index 0",
        }
        with open(f"{base}.meta.json", 'w') as f:
            json.dump(meta, f, indent=2)

        return base
```

#### Main Loop and Console Output

```python
def main():
    reader = RawHIDReader(recordings_dir=args.output)
    recording = StreamRecording()

    def on_packet(raw_data):
        parsed = parse_stream_packet(raw_data)
        if parsed is None:
            return
        recording.add_packet(raw_data, parsed)

    # Print status line every second
    # [  12.0s] 14520 samples | 1210 Hz | 0 drops | buffer: 870 KB

    # On ctrl-C or duration elapsed:
    recording.save(Path(args.output))
```

The main loop uses `RawHIDReader.start(on_packet)` which handles reconnection.
On ctrl-C (KeyboardInterrupt), it calls `recording.save()` and prints a summary.

### Output Files

Each recording produces three files in `recordings/`:

1. **`sensor_stream_20260131_143022.npz`** — NumPy archive
   - `samples`: (N, 3) int16 array [X, Y, Z]
   - `timestamps`: (N,) uint16 array (firmware ms since stream start, wraps at 65536)
   - `drop_flags`: (N,) bool array (True = gap placeholder, data is sentinel)

2. **`sensor_stream_20260131_143022.bin`** — raw packet backup
   - Every 32-byte packet concatenated in received order
   - Can always reconstruct from scratch if .npz parsing has bugs
   - File size = `total_packets × 32` bytes

3. **`sensor_stream_20260131_143022.meta.json`** — human-readable summary
   ```json
   {
     "start_time": "2026-01-31T14:30:22.456789",
     "duration_s": 120.3,
     "total_samples": 145200,
     "total_packets": 145200,
     "dropped_packets": 3,
     "drop_events": [[48201, 2, 40123], [99450, 1, 82876]],
     "effective_rate_hz": 1207.5,
     "sensor_config": "single sensor, cluster 0, index 0"
   }
   ```

### Integration with Existing qmk-console-viewer

The stream logger is a **standalone CLI tool**, not integrated into the Flask app.
Rationale:
- The Flask app uses eventlet which complicates Raw HID access (tpool needed)
- A standalone script is simpler to run, debug, and kill with ctrl-C
- Console reader (text at 5Hz) and stream logger (binary at ~1kHz) serve different purposes
- They can run simultaneously since they use different HID interfaces (0xFF31 vs 0xFF60)

However, `RawHIDReader` is designed as a reusable module so it *could* be integrated
into app.py later (as a second background task alongside ConsoleReader) if we want
live streaming in the web UI.

### Changes to Existing Files

**`requirements.txt`** — add:
```
hidapi>=0.14.0
numpy>=1.24.0
```
(hidapi is already a dependency mentioned in CLAUDE.md but not in requirements.txt;
numpy is needed for .npz output)

**No changes to `console_reader.py`, `app.py`, or `index.html`** — the stream logger
is additive, not modifying existing code.

### Sequential Guarantee

The combination of:
- **seq number** (rolling 0-255) in every firmware packet detects any gap
- **Gap placeholder insertion** for drops keeps array indices aligned to wall-clock time
- **`drop_events` list** in metadata records exact position and size of every gap
- **Raw .bin backup** preserves every byte received in order, can always reconstruct
- **No on-device buffering** — data is never stored on the RP2040, nothing to pull off

means the host log is the single source of truth, always sequential, always reconstructable.

### Future: Web UI Integration (Not in Scope)

If we later want live streaming in the web UI:
1. Add `RawHIDReader` as second background task in `app.py`
2. New SocketIO event `stream_packet` emits parsed samples to browser
3. Frontend chart (e.g., Chart.js or D3) plots X/Y/Z over time
4. Recording start/stop via WebSocket (like existing log controls)

This is straightforward because `RawHIDReader` follows the same pattern as
`ConsoleReader` — but it's out of scope for the initial streaming test.

---

## Implementation Order

### Step 1: Create worktree and branch (hall-qmk)
```
cd C:\Users\morga\OneDrive\Documents\GitHub\hall-qmk
git worktree add ../hall-qmk-stream -b sensor-stream hall2
```

### Step 2: Firmware — `sensor_stream.h` + `sensor_stream.c`
- Packet struct with static assert
- `sensor_stream_init()` and `sensor_stream_task()`
- Uses existing `tmag_get(0)` and `raw_hid_send()`

### Step 3: Firmware — `keymaps/stream_test/`
- rules.mk: enable RAW_ENABLE, USE_HALL_MATRIX, disable VIA/VIAL/RGB
- keymap.c: minimal single-layer KC_NO keymap
- config.h: any overrides

### Step 4: Firmware — wire into `matrix_tmag3001.c`
- Call `sensor_stream_init()` after tmag_init succeeds
- Call `sensor_stream_task()` each scan cycle
- Conditionally compile (only when RAW_ENABLE is defined)

### Step 5: Compile and verify firmware builds
```
qmk compile -kb svalboard/trackball/pmw3389/right -km stream_test \
  -e USE_HALL_MATRIX=yes -e TMAG_CLUSTER=0
```

### Step 6: Host — `raw_hid_reader.py`
- RawHIDReader class following ConsoleReader connection management pattern
- Filter by Usage Page 0xFF60, Usage 0x61, PID 0x4044
- Auto-reconnect with exponential backoff
- Eventlet-compatible (tpool wrapping for blocking HID calls)

### Step 7: Host — `stream_logger.py`
- StreamPacket parsing
- SequenceTracker for drop detection
- StreamRecording for accumulation + save
- CLI with --duration, --output, --vid, --pid
- Live status line every second
- .npz + .bin + .meta.json output on exit

### Step 8: Host — update `requirements.txt`
- Add hidapi, numpy

### Step 9: Integration test
- Flash firmware, run stream_logger.py
- Verify sample rate, drop rate, data integrity
- Record 2-minute baseline with no keys pressed
- Record samples with key presses
- Inspect .npz in notebook/matplotlib

---

## Open Questions

1. **I2C frequency**: Currently 400kHz (conservative for long FFC cables). For a bench test with short wires, could bump to 1MHz for higher sample rate. Worth trying?

2. **Oversampling**: TMAG3001 Conv_AVG is currently 0 (no oversampling). This gives maximum rate but noisiest signal. The whole point is to characterize this noise, so 0 is correct — but worth confirming.

3. **Which physical sensor**: Cluster 0, sensor 0 — is that the right one for your test setup, or do you need a different bus/address?
