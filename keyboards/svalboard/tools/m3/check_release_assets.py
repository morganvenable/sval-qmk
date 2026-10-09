#!/usr/bin/env python3
"""Check a downloaded release's assets (M3): check_release.sh runs this.

    check_release_assets.py DIR TAG

DIR must hold exactly NAME_TAG.uf2 and NAME_TAG.svup for each of the 12
release keymap builds. For each pair: the .svup's image is the .uf2's image;
its manifest says TAG, the build's side, pointing device and keymap, and a
release key slot; the image's build-info record agrees; and make_update.py
verify --build release accepts it (signature with the committed release keys,
manifest rules, hash, structure). Exit 1 on any failure.
"""
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import check_release_artifacts as cra  # noqa: E402
import make_update as mu  # noqa: E402


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    d, tag = Path(argv[0]), argv[1]
    names = {cra.build_name(kb, km): (kb, km) for kb, km in cra.RELEASE_BUILDS}
    want = {f"{n}_{tag}{ext}" for n in names for ext in (".uf2", ".svup")}
    have = {p.name for p in d.iterdir()}
    problems = [f"unexpected file {f}" for f in sorted(have - want)] + [f"missing {f}" for f in sorted(want - have)]
    if not problems:
        for n, (kb, km) in sorted(names.items()):
            uf2, svup = d / f"{n}_{tag}.uf2", d / f"{n}_{tag}.svup"
            try:
                manifest, _sig, image = mu.parse_svup(svup.read_bytes())
                m = mu.parse_manifest(manifest)
                hand, pointing_id = mu.kb_fields(kb)
                ident = dict(hand=hand, pointing_id=pointing_id, keymap_id=mu.KEYMAP[km])
                info = mu.build_info(image)
                if mu.uf2_to_image(uf2.read_bytes()) != image:
                    problems.append(f"{n}: the .svup's image is not the .uf2's")
                if mu.manifest_version(m) != tag or m["key_id"] not in mu.RELEASE_KEY_IDS:
                    problems.append(f"{n}: version {mu.manifest_version(m)!r}, key_id {m['key_id']}")
                for k, v in ident.items():
                    if m[k] != v or info is None or info[k] != v:
                        problems.append(f"{n}: {k} is {m[k]} in the manifest, "
                                        f"{None if info is None else info[k]} in the image, {v} expected")
            except (mu.Refused, OSError) as e:
                problems.append(f"{n}: {e}")
                continue
            p = subprocess.run([sys.executable, "-I", str(TOOLS / "make_update.py"), "verify", str(svup),
                                "--build", "release"], capture_output=True, text=True)
            print((p.stdout + p.stderr).strip())
            if p.returncode:
                problems.append(f"{n}: a release build refuses its .svup")
    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    print(f"ok: {tag}: 12 .uf2 and 12 .svup; each .svup holds its .uf2's image and a release build accepts it")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
