> Historical design model: its single-slot output scheduler predates the flick correction. The current scheduler is verified against production C by tests/sval_pointing/test_flick.c.

# Discrete pointing reference model

An executable companion to the [architecture draft](../../docs/design/discrete-pointing.md).
This directory is not part of any QMK build. It is newly written specification
support, not ported BKB code.

Run from the repository root, using only the Python standard library:

```sh
python3 -m unittest discover -s experiments/discrete_pointing -v
python3 experiments/discrete_pointing/model.py
```

The model covers one-dimensional normalized distance, deliberate reversal,
idle reset, one-slot fresh-intent limiting and per-pointer hold/toggle
precedence. Its input is one-dimensional motion already admitted by the
existing shared free/axis-lock scroll policy. Reversal tolerance operates
within an eligible axis; it is not a competing axis lock. Integration must
clear affected step progress/pending intent when eligibility changes and must
never feed suppressed motion into these accumulators.

It uses exact fractions to make expected results independent of
fixed-point rounding. Firmware must separately test bounded arithmetic,
overflow and timer wrap.

The important reversal example uses 100 units per step:

| Motion | Ordinary signed bucket | Proposed qualified-turn rule |
|---|---|---|
| +80 | No output; remainder +80 | No output; progress +80 |
| Then −100 | No output; remainder −20 | One negative step; remainder 0 |

The signed bucket requires 180 negative units for the first negative output
after that +80 partial movement. The proposed rule recognizes a deliberate
change of direction, abandons the unfinished positive stroke and credits all
the reverse travel. A small −5/+5 wobble does neither: it restores the previous
progress without manufacturing new travel.

This is a gesture policy, not an absolute-position dial. Repeated +90/−90
strokes produce no output at a 100-unit step distance, even though the total
path length is large. The [generated traces](traces.csv) also show retained
overshoot and idle reset. The comparison bucket intentionally has no idle
timeout, so that final trace compares stale retention rather than reversal.

The limiter admits at most one fresh pending intent. Admission means a
hypothetical output resource is ready; it does not model USB delivery or host
acknowledgement. Coalescing deliberately loses excess steps above the output
rate. A test that preserves exact spatial distance under a rate cap would be
testing the wrong contract.

Not implemented here: shared scroll-policy integration, multi-axis arbitration,
the task-session state machine, QMK action/report integration, modifier
ownership, consumer arbitration, split
transport, persistence, Keybard or operating-system behavior. The tests do not
claim any of those are validated. No test board was used for this draft.
