# Vendored third-party code

## zstddeclib.c — zstd decompressor, single file

- **Upstream**: <https://github.com/facebook/zstd>
- **Vendored version**: **v1.5.7** (`ZSTD_VERSION_*` defines inside the file)
- **What it is**: the official single-file *decompression-only* amalgamation
  (no compressor, no legacy formats) — used by `fseq_player` to inflate
  FSEQ v2 compressed frame blocks.
- **License**: BSD-3-Clause / GPLv2 dual (see the file header). BSD applies
  here.

### How to update

Regenerate from a zstd release checkout — never hand-edit this file:

```bash
git clone --depth 1 --branch v1.5.x https://github.com/facebook/zstd
cd zstd/build/single_file_libs
python3 combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
cp zstddeclib.c <pixfrog>/components/fseq_player/third_party/
```

Then update the version in this README, run the fseq host suite
(`components/fseq_player/test`) and a full `./tools/ci-local.sh`.

The file is listed in `.clang-format-ignore` — keep it that way.
