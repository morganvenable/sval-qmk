"""Render guided characterization summaries into a standalone HTML report."""
import argparse
import json
from pathlib import Path


def render(directory):
    phases=sorted((json.loads(p.read_text()) for p in directory.glob('*.json')),key=lambda r:(r['expected_contacts'],r['configuration']['period_ms'],r['configuration']['maximum']))
    if not phases:raise ValueError('No phase summaries found')
    rows=[]
    for r in phases:
        c=r['configuration'];m=r['metrics'];cycle=r['mean_sensor_cycle_ms']
        rows.append(f"""<tr><td>{r['expected_contacts']}</td><td>{c['maximum']}</td><td>{c['period_ms']} ms</td>
        <td><span class="bar" style="width:{cycle*6:.1f}px"></span> {cycle:.2f} ms</td>
        <td>{r['mean_acquisition_ms']:.2f} ms</td><td>{r['p95_interval_ms']:g} ms</td>
        <td>{r['matching_fraction']:.1%}</td><td>{m['rate_missed']}/{m['sensor_samples']}</td>
        <td>{sum(r['errors'].values())} / {r['sequence_gaps']}</td></tr>""")
    return '''<!doctype html><meta charset="utf-8"><title>Azoteq timing characterization</title>
    <style>body{background:#101b27;color:#e1edf5;font:16px system-ui;max-width:1180px;margin:40px auto;padding:0 24px}h1{font-size:34px}p{line-height:1.6;max-width:980px}.muted{color:#a5bbc9}table{border-collapse:collapse;width:100%;margin:30px 0;font-variant-numeric:tabular-nums}th,td{text-align:left;padding:12px 8px;border-bottom:1px solid #314556}th{color:#91d9d1;font-size:13px}.bar{display:inline-block;height:9px;background:#58cebf;border-radius:5px}strong{color:#80e4d6}.card{padding:20px;background:#1a2b3a;border-radius:12px;margin:20px 0}li{margin:12px 0;line-height:1.5}</style>
    <p class="muted">TEST BOARD · IQS572 / TPS43 · 400 kHz I²C · 64-byte HID</p>
    <h1>Fewer allowed fingers did not make this pad faster</h1>
    <p>One finger stayed near <strong>10.3 ms</strong> whether the configured maximum was 1, 2, 3 or 5.
    With three fingers, limits of 3 and 5 both yielded roughly <strong>12 ms</strong> cycles.
    Requesting 5 ms did not achieve 200 Hz; requesting 20 ms slowed the observed cycle to about 21 ms.</p>
    <table><thead><tr><th>Held fingers</th><th>Allowed</th><th>Requested period</th><th>Mean sensor cycle</th><th>Mean blocking read</th><th>P95 frame interval</th><th>Matching contacts</th><th>RR_MISSED / samples</th><th>I²C errors / sequence gaps</th></tr></thead><tbody>'''+''.join(rows)+'''</tbody></table>
    <div class="card"><strong>Contact dropout ≠ missing report</strong><p>The user observed temporary loss of a finger while holding three.
    The 5 ms phase reported three contacts in 87% of frames; the rest reported two.
    All eight phases had zero I²C errors and zero frame-sequence gaps. These are different failure modes.</p></div>
    <ul><li>Each measurement is approximately four seconds after one second of settling. One run per condition; no randomized order or statistical confidence interval.</li>
    <li>The sensor's Previous Cycle Time includes its communication window. These measurements characterize this firmware and sensor together, not a pure sensor processing limit.</li>
    <li>RR_MISSED means the requested sensor reporting interval was missed. It does not mean a USB frame was lost. At short periods, synchronous I²C reads can occupy most of the main loop.</li>
    <li>Mixed contact counts confound the 5 ms comparison. Lower maximum-touch settings are also unsafe as a transparent optimization: exceeding the configured limit can suppress contact coordinates.</li>
    <li>The next scheduling improvement is readiness-driven or asynchronous acquisition. A second half should timestamp and acquire its own contacts independently.</li></ul>
    <p class="muted">Firmware remains installed. Characterization restores five allowed contacts and a 10 ms requested period.</p>'''


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();args.output.write_text(render(args.directory),encoding='utf-8')


if __name__=='__main__':main()
