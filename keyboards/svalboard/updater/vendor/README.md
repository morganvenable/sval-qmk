# Vendored: Monocypher 4.0.2

Ed25519 signature checks for the in-firmware updater (decision D7). Compiled only
when the build sets `SVAL_UPDATER=yes` (`keyboards/svalboard/rules.mk`).

- **Upstream:** https://github.com/LoupVaillant/Monocypher
- **Release tarball:** https://github.com/LoupVaillant/Monocypher/releases/download/4.0.2/monocypher-4.0.2.tar.gz
- **Tarball SHA-256:** `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`
- **Licence:** dual BSD-2-Clause / CC0-1.0, your choice (`LICENCE.md`, copied from the tarball). Not GPL.

Files, copied byte for byte from the tarball's `src/` (do not edit them; update
by replacing them from a new release and recording it here):

| File | From | SHA-256 |
|---|---|---|
| `monocypher.c` | `src/monocypher.c` | `afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59` |
| `monocypher.h` | `src/monocypher.h` | `f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374` |
| `optional/monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | `7c9b16056cbd27521919e8a6f56a228808b9e718afc42e3d33f28c08e5abdee2` |
| `optional/monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | `bd546edcd468d64e28caa3dbf4b1d6bfad7435c0ce994723fd81aae26405121b` |
| `LICENCE.md` | `LICENCE.md` | `5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3` |

## Use

- Verify with **`crypto_ed25519_check`** and hash with `crypto_sha512*` from
  `optional/monocypher-ed25519.h`. These are standard Ed25519 (RFC 8032) and SHA-512.
- **Never `crypto_eddsa_check`**: the core one hashes with BLAKE2b, so it does
  not verify standard Ed25519 signatures.
- `optional/monocypher-ed25519.h` includes `"monocypher.h"`, so this directory
  is on the include path (`EXTRAINCDIRS` in `rules.mk`).
- The host tests (`util/updater_test/run.sh`) check RFC 8032 vectors and
  signatures made by `keyboards/svalboard/tools/make_update.py`.
