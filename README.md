# notolibra-engine

Notolibra's own Chromium build (option D). This repository is checked out at
`src/notolibra` inside a Chromium checkout, the way Brave's `brave-core` sits at
`src/brave`. The plan is `docs/research/browser-engine/plan.md` in the Notolibra
app repository; section numbers below refer to it. Private; proprietary.

## Layout

| Path | What |
| --- | --- |
| `CHROMIUM_VERSION` | The Chromium tag we build (§5.5: Extended Stable). |
| `api/ne_engine.h` | The C embedding API (§2.2). Phase 0 stub: declarations only. |
| `build/args/` | GN args: `common.gn` (§3.2, §5.1), `dev.gn`, `release_test.gn`, `official.gn`, and `clt.gn` for building without Xcode. |
| `build/config/BUILD.gn` | The `chromium_src` override config (§4.3). |
| `chromium_src/` | Overrides, mirroring Chromium's paths (none yet). |
| `patches/series` | Quilt-style series of the upstream patches we can't avoid (§4.4). |
| `scripts/` | `sync`, `build`, `patches.py`, `clt-xcode`, `redirect_cc`. |
| `test/media/` | The Phase 0 media check (§6, check 6), run hidden. |
| `docs/xcode.md` | What the macOS build needs from Xcode, and how it builds without it. |

## Day to day

```sh
cd ~/chromium/src/notolibra
scripts/sync                      # gclient sync to CHROMIUM_VERSION, apply patches, check them
scripts/build                     # dev build of content_shell at -j 6 (daytime rule)
scripts/build --jobs 10 --min-free-mem 25 content_shell chrome   # overnight
test/media/run_media_check        # hidden codec check against out/Dev
```

`scripts/build` refuses to start under 60 GiB free (AGENTS.md's floor is
50 GiB), samples `memory_pressure` every 20 s and stops its own build if free
memory drops below `--min-free-mem`, and compresses large logs with zstd. Logs
and memory samples go to `out/<Config>/logs/`. A stopped build resumes
incrementally when re-run.

## Patches

`scripts/patches.py {check,status,apply,unapply,refresh,new}`. Rules:

- One upstream file per patch, named after the file (`/` becomes `-`, with the
  nested repository's path in front for patches in `third_party/*` repos).
  Patches for one concern share a directory.
- Each patch starts with a header: the reason, the upstream bug if any, and
  `Repo: <path>` for nested repositories.
- `# disabled: <patch>` keeps a patch in the series without applying it.
- `check` runs ungoogled-chromium's checks: every listed patch exists, every
  patch file is listed, the file it changes exists, and the series applies
  cleanly or is already applied.

Current series:

| Concern | Patches | Why |
| --- | --- | --- |
| `clt-build` | ANGLE `metal_backend.gni` | Build with only the Command Line Tools: ANGLE compiles its internal Metal shaders at run time (`docs/xcode.md`). |
| `ffmpeg-no-h264-aac-decoders` | FFmpeg `BUILD.gn`, Chrome mac arm64 `codec_list.c` and `config_components.h` | Chrome-branded FFmpeg without its H.264 and AAC decoders (§3.2; Chromium asserts `proprietary_codecs` needs Chrome branding). The two config files are LGPL. |
| `media-audiotoolbox-aac` | `audio_toolbox_audio_decoder.cc`, `gpu_mojo_media_client_mac.cc`, `supported_types.cc` | AudioToolbox decodes all AAC, not only xHE-AAC (§3.3). |
| `chromium-src` (disabled) | `build/config/BUILDCONFIG.gn` | Turns on `chromium_src` overrides. |

## Licences

Closed source. Only permissive code goes in here: our own, Chromium's
(BSD-3), CEF's (BSD-3, with notices). GPL code (Helium's patches) and LGPL code
(Vivaldi's FFmpeg-derived files) are reference only. Patches that modify
FFmpeg's LGPL files say so in their headers; the modified FFmpeg ships as its
own `libffmpeg.dylib` and is offered in source form under the LGPL.
