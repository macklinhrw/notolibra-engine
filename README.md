# notolibra-engine

Notolibra's own Chromium build (option D). This repository is checked out at
`src/notolibra` inside a Chromium checkout, the way Brave's `brave-core` sits at
`src/brave`. The plan is `docs/research/browser-engine/plan.md` in the Notolibra
app repository; section numbers below refer to it. Private; proprietary.

## Layout

| Path | What |
| --- | --- |
| `CHROMIUM_VERSION` | The Chromium tag we build (§5.5: Extended Stable). |
| `api/ne_engine.h` | The C embedding API (§2.2). |
| `engine/` | NotolibraEngine.framework (on `//content`): the API's implementation (`api.mm`), a main delegate for every process, browser client and profile (`browser_client`, `browser_context`: permissions, session SSL state, downloads, persistent storage, cookies encrypted in our own keychain item), `Tab` (`tab.mm`, `tab_ui.mm`: frames as IOSurfaces from Viz's capturer, input, dialogs, file choosers, context menus, new windows, favicons as UI requests), `view_bridge` (RenderWidgetHostViewMac's NSView calls as data: cursors, `<select>`, tooltips, IME), `request_filter` (the app's content filter in front of every page's URLLoaderFactory, on the IO thread), `frame_agent.mojom` and `renderer/` (document-start scripts run as each frame's main-world context is created), `download_delegate`, the sandboxed helper app, and the framework's own .pak. |
| `proto/` | The Phase 0 gpui host: a standalone cargo project that shows a tab in a GPUI Fast composition surface and runs the Phase 0 checks hidden (`proto/run_checks`). |
| `test/engine/` | `run_smoke`: the engine from a plain AppKit host, hidden. |
| `build/args/` | GN args: `common.gn` (§3.2, §5.1), `dev.gn`, `release_test.gn`, `official.gn`, and `clt.gn` for building without Xcode. |
| `build/config/BUILD.gn` | The `chromium_src` override config (§4.3). |
| `chromium_src/` | Overrides, mirroring Chromium's paths (none yet). |
| `patches/series` | Quilt-style series of the upstream patches we can't avoid (§4.4). |
| App integration | The Notolibra app's `native/crates/engine_sys` (bindings, loaded with dlopen) and `native/crates/app/src/engine_notolibra.rs` (the browser pane's backend). The app's only browser engine since October 18, 2026, when it replaced CEF. Unsigned app builds pass an empty keychain service (mock keychain); builds signed with a Team ID name the engine's item. |
| `scripts/` | `sync`, `build`, `patches.py`, `clt-xcode`, `redirect_cc`. |
| `test/media/` | The Phase 0 media check (§6, check 6), run hidden. |
| `docs/xcode.md` | What the macOS build needs from Xcode, and how it builds without it. |

## Day to day

```sh
cd ~/chromium/src/notolibra
scripts/sync                      # gclient sync to CHROMIUM_VERSION, apply patches, check them
scripts/build                     # dev build of content_shell at -j 6 (daytime rule)
scripts/build --jobs 10 --min-free-mem 25 content_shell chrome   # overnight
scripts/build notolibra/engine    # NotolibraEngine.framework (+ helper app)
test/media/run_media_check        # hidden codec check against out/Dev
test/engine/run_smoke             # the engine from a plain AppKit host, hidden
proto/run_checks                  # the gpui host's Phase 0 checks, hidden
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
| `media-audiotoolbox-aac` | `audio_toolbox_audio_decoder.cc`, `gpu_mojo_media_client_mac.cc`, `supported_types.cc`, `ffmpeg_common.cc`, `audio_decoder_unittest.cc` | AudioToolbox decodes all AAC, not only xHE-AAC (§3.3); FFmpeg demuxes AAC it can't decode. |
| `macos-attach-mode` | 7 files in `base/` | The browser's UI thread attaches to a host's running `-[NSApplication run]` (iOS's attach mode, ported; §2.4). |
| `embedder-run-loop` | `content_main.h`, `content_main.cc` | `ContentMainParams::embedder_owns_run_loop`: content starts without running or shutting down the loop. |
| `chromium-src` (disabled) | `build/config/BUILDCONFIG.gn` | Turns on `chromium_src` overrides. |
| `embedder-ns-view` | `render_widget_host_view_mac.{h,mm}`, `content/browser/BUILD.gn` | `RenderWidgetHostViewMac::SetEmbedderNSView`: the engine's view bridge takes the NSView's calls (cursors, `<select>`, IME state as data); `for_content_tests` visible to `//notolibra/*`. |
| `embedder-keychain` | `components/os_crypt/common/BUILD.gn` | `ALLOW_RUNTIME_CONFIGURABLE_KEY_STORAGE`, so the engine names its own Safe Storage keychain item. |
| `embedder-sandbox-path` | `content/browser/sandbox_parameters_mac.mm` | When the framework is outside the host app's bundle, the Seatbelt profile's bundle path is the framework's folder. |
| `nonbundled-ffmpeg-rpath` | `build/config/mac/BUILD.gn` | FFmpeg is a shared library in every build, so non-component builds (`release_test`) give binaries the component build's rpaths; build tools such as `v8_context_snapshot_generator` load it. |

## Licences

Closed source. Only permissive code goes in here: our own, Chromium's
(BSD-3), CEF's (BSD-3, with notices). GPL code (Helium's patches) and LGPL code
(Vivaldi's FFmpeg-derived files) are reference only. Patches that modify
FFmpeg's LGPL files say so in their headers; the modified FFmpeg ships as its
own `libffmpeg.dylib` and is offered in source form under the LGPL.
