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
| `engine/` | NotolibraEngine.framework (on `//content`): the API's implementation (`api.mm`), a main delegate for every process, browser client and profile (`browser_client`, `browser_context`: permissions, session SSL state, downloads, persistent storage, cookies encrypted in our own keychain item), `Tab` (`tab.mm`, `tab_ui.mm`: frames as IOSurfaces, the compositor's own output (below), input, dialogs, file choosers, context menus, new windows, favicons as UI requests), `view_bridge` (RenderWidgetHostViewMac's NSView calls as data: cursors, `<select>`, tooltips, IME), `request_filter` (the app's content filter in front of every page's URLLoaderFactory, on the IO thread), `system_drag` (page drags become system drags only from a window someone can see, below), `pdf` (PDF responses commit an engine-made document and their bytes are saved for the app, below), `frame_agent.mojom` and `renderer/` (document-start scripts run as each frame's main-world context is created; error pages the browser sends with the commit), paint holding across navigations and the app's surface colour (below), `profile_import` (another browser's cookies and local storage written through the profile's own stores, below), `download_delegate`, the sandboxed helper app, and the framework's own .pak. |
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
| `embedder-ns-view` | `render_widget_host_view_mac.{h,mm}`, `content/browser/BUILD.gn` | `RenderWidgetHostViewMac::SetEmbedderNSView`: the engine's view bridge takes the NSView's calls (cursors, `<select>`, IME state as data, and the compositor's frames, its last one handed over when the bridge is attached); `for_content_tests` visible to `//notolibra/*`. |
| `embedder-keychain` | `components/os_crypt/common/BUILD.gn` | `ALLOW_RUNTIME_CONFIGURABLE_KEY_STORAGE`, so the engine names its own Safe Storage keychain item. |
| `embedder-sandbox-path` | `content/browser/sandbox_parameters_mac.mm` | When the framework is outside the host app's bundle, the Seatbelt profile's bundle path is the framework's folder. |
| `nonbundled-ffmpeg-rpath` | `build/config/mac/BUILD.gn` | FFmpeg is a shared library in every build, so non-component builds (`release_test`) give binaries the component build's rpaths; build tools such as `v8_context_snapshot_generator` load it. |
| `embedder-compositor-frames` | `skia_output_device_buffer_queue.cc` | Without remote Core Animation, macOS gets five root buffers instead of three (pending swaps stay at two): the app shows each buffer itself, so it can fall three frames behind before one it shows is drawn into again. |

## Frames

Pages are shown from their compositor's own output (October 18, 2026, for
hover latency; native/README.md, Browser engine). The engine turns off remote
Core Animation (`RemoteCoreAnimationAPI`) and renders in sRGB
(`--force-color-profile=srgb`), so the GPU process hands each display frame
to the browser as one IOSurface through `SetCALayerParams`, which the view
bridge passes to `Tab::OnCompositorFrame` and the app shows with no copy.
Viz's capturer, used until then, saw a frame only as the display drew it and
copied it in the display's next pass: a second draw and a GPU copy per frame,
and the frame a pass later. `--notolibra-frame-source=capture` brings the
capturer back for comparison. `ne_frame.capture_time_us` is when the engine
had the frame (`base::TimeTicks` microseconds), so hosts can time the stages.

## Paint holding and the surface colour

No frame between two pages is blank (October 19, 2026, after Macklin saw a
white frame between a page and the search results he typed for). Measured
with the app's `NOTOLIBRA_FLASH_TEST` (native/README.md), which samples every
frame delivered: with RenderDocument (on by default) every cross-document
navigation of the main frame gets a new `RenderWidgetHostViewMac` with a
compositor of its own, and that compositor drew its first frames before the
new renderer's: its background, which is the new document's reported colour
before it paints (white, or `#121212` for `color-scheme: dark`), not the
previous page as Chrome's fallback content would show it. A page that paints
late (a late style sheet or body, a page its scripts build) also drew its
empty white document after Blink's 500 ms paint holding gave up, and
content's error page and `about:blank` are empty white documents. A killed
renderer's reload delivered no frames at all: its widget kept the bridge of
the view that died.

- **Holding** (`Tab::BeginHold`): from `ReadyToCommitNavigation` of a
  cross-document main-frame navigation (not a back-forward cache restore or
  prerender activation) until the new document's
  `OnFirstContentfulPaintInPrimaryMainFrame`, no frame is delivered, and the
  frame on screen is delivered once more as a CPU copy in an IOSurface of the
  engine's own (the compositor, maybe a recycled one, draws into its buffers
  again). The paint ends the hold with the first display frame drawn after a
  renderer frame activated (`RenderFrameMetadataProvider`) and no earlier
  than 25 ms before the paint's presentation time (frames reach the view up
  to about one refresh before it; 16 ms measured at 120 Hz). A loaded page
  that never paints contentful content ends it, 300 ms after the load (a
  page's paint often comes just after), with a frame that isn't blank (one
  colour, every second pixel, other than the surface colour, so a form
  control or a spinner counts); 5 s ends it whatever. A
  new tab holds until its first page paints. `on_first_paint` reports each
  hold's end, after its frame.
- **Surface colour** (`ne_tab_config.surface_color_argb`,
  `ne_tab_set_surface_color`): every main-frame view's background (as
  Chrome's `ContentsWebView` sets its window colour), the background of
  `about:blank` the app opened (a style added at document start; pages' own
  about:blank documents are left alone), and the error page, which the
  browser builds (`Tab::ErrorPageHtml`: the address and the net error, in the
  surface colour) and sends with the commit (`FrameAgent.SetErrorPage`,
  answered from `RendererClient::PrepareErrorPage`). The page's base
  background stays white (`background_color_argb`): pages without a
  background of their own are white on the web.
- **A crashed renderer:** bridges whose view went away while the widget
  stayed are dropped (`Tab::DropStaleBridges`), so the reload's new view gets
  one.

## Input

`ne_tab_send_input` hands events to the main frame's `RenderWidgetHostViewMac`
as AppKit's events reach it in Chrome: pointer and wheel events through
`RouteOrProcessMouseEvent` and `RouteOrProcessWheelEvent`, which hit-test
them to the widget under the pointer (a cross-site frame renders in another
process, with a widget of its own), and keys between `BeginKeyboardEvent` and
`EndKeyboardEvent`, which send them to the focused widget. The view's
`MouseWheelPhaseHandler` gives a wheel without phases (a mouse wheel) its
phases and ends a scroll after a pause. Until October 18, 2026 the tab
forwarded every event to the main frame's widget, so cross-site frames
(embedded players, sign-in and payment frames, Twitch's extensions) got no
input at all: the main frame's renderer only sees the frame's element. Where
the main frame's content lies over a cross-site frame, the browser asks the
main frame's renderer what is under the pointer, so on a busy page events
there can arrive a little late, as in Chrome. The app's check:
`NOTOLIBRA_ENGINE_UI_TEST=1` (native/README.md).

## Drags

A page starts a drag (selected text, a link, an image, a draggable element)
by asking the browser to start one, and `WebContentsViewMac::StartDragging`
starts an `NSDraggingSession`, whose image the window server draws on the
screen. Our pages' views are never in a window, so the image starts at the
screen's origin, its bottom-left corner. On October 18, 2026 a hidden check's
synthetic drag over a link drew a page thumbnail there, over Macklin's
windows, and it stayed for seconds (nobody held the button to end it).

`BrowserClient::IsDragAllowedByPolicy`, the embedder's hook before the
session starts, refuses unless the left button is really held
(`NSEvent.pressedMouseButtons`) and the pointer is over one of the app's
windows that is visible, on a screen, not occluded, not transparent and
taking mouse events (`engine/system_drag.mm`). Refusing, it sends the source
widget `DragSourceEndedAt` with no drop operation at the last pointer
position, as a cancelled drag ends (the page gets `dragend` with
`dropEffect` "none"), and `WebContentsViewMac` then sends
`SystemDragEnded`, as it does for drags it can't start. Real drags in a
visible window go on as before; where their image starts (from the view's
missing window) hasn't been checked, since a hidden run can't drag for real
and checks never show a window. The app's
check: `NOTOLIBRA_ENGINE_UI_TEST=1` drags selected text and a link in the
hidden window and checks that nothing of the run showed on screen, that the
page saw both drags start and end cancelled, and that it takes a click
after (native/README.md, Background verification).

## PDFs and crashed renderers

The engine has no PDF viewer (Chrome's is an extension and a plugin, neither
of which `//content` has), so until October 19, 2026 every PDF a page opened
downloaded, and an `<iframe>` of one did too, silently. Now, when the app
sets `ne_app_config.pdf_directory`, a navigation throttle
(`engine/pdf.cc`, from `BrowserClient::CreateURLLoaderThrottles`) looks at
every frame navigation's response, at the seam Chrome's own viewer uses
(`PluginResponseInterceptorURLLoaderThrottle`, whose CSP handling it copies):

- a response that is a PDF (`application/pdf`, or `text/pdf`, not sniffed
  from `text/plain`) and isn't an attachment (`Content-Disposition:
  attachment` still downloads, `content::download_utils::MustDownload`) gets
  its body replaced with a small document of the engine's, `text/html` and
  flagged `intercepted_by_plugin`, so the navigation commits at the PDF's
  address and has a history entry of its own: back, forward and reload work
  as for any page;
- in the main frame that document holds only the file's name, in the surface
  colour (the app's PDF view covers it), and is kept out of the back-forward
  cache; the original body (with its loader and client) goes to a `PdfSaver`
  owned by the `Tab`, which drains it into `<pdf_directory>/<tab>-<pdf>/<name>`
  on a blocking sequence and reports `ne_ui_delegate.on_pdf`: STARTED,
  PROGRESS (at most every 100 ms), FINISHED, and COMMITTED right before the
  commit's `NE_LOAD_COMMITTED`. Another main-frame commit cancels the tab's
  unfinished saves;
- in a subframe (`<iframe>`, or an `<embed>`/`<object>` loaded as a frame)
  it is a card with the file's name and an "Open PDF" link to the top frame;
  nothing is saved and the request is dropped. The app turns `<embed>` and
  `<object>` PDFs (which Blink otherwise leaves empty, without a request)
  into such frames.

A main frame's renderer that crashes, is killed or runs out of memory is
reported to `ne_ui_delegate.on_renderer_gone` (Chrome's sad-tab statuses;
not a normal exit, and not for tabs being closed); the app draws its own
"This page crashed" page with Reload. Discarded tabs (the app closes their
`ne_tab`) never report it. The app's check: `NOTOLIBRA_WEBPDF_TEST=1`
(native/README.md, Browser engine → PDFs, downloads and crashed pages).

## What pages see of the browser

Bot checks (Cloudflare's challenge, Turnstile) compare what a page can read
of the browser with what Chrome would show. Until October 19, 2026 the
engine disagreed with Chrome in four ways, and Cloudflare's challenge never
cleared (its "Verify you are human" box came back after every click; the
page Macklin hit was a novelupdates.com series page). Now, as in Chrome:

- **Client hints** are `embedder_support::GetUserAgentMetadata()` (a
  Chromium build's: Chromium and a GREASE brand such as `Not:A-Brand`, the
  same list with full versions in `fullVersionList`, the macOS version as
  `platformVersion`), for script (`BrowserClient::GetUserAgentMetadata`) and
  for the `Sec-CH-UA*` headers alike: the profile has a client-hints
  delegate (`client_hints::InMemoryClientHintsControllerDelegate`, as
  fuchsia's WebEngine has, fed by a `NetworkQualityTracker` in
  `BrowserMainParts`). Before, script saw a hand-made list (Chromium and
  "Notolibra", no GREASE, `fullVersionList` without the second brand, no
  platform version), and requests carried no `Sec-CH-UA` headers at all,
  with Chrome's user agent string. There is no Notolibra brand: a plain
  Chromium list is what Chromium builds send, and what passes.
- **Languages** come from the system's preferred languages
  (`engine/languages.mm`: `[NSLocale preferredLanguages]`, script subtags
  dropped, each followed by its base language as Chrome's accept-languages
  preference reads, e.g. `en-US,en`). `navigator.languages` is that list
  (renderer preferences, set per tab and for workers) and `Accept-Language`
  the same with Chrome's q-values (`en-US,en;q=0.9`). Before, script saw
  `["en-US"]` and requests sent `en-US,en`.
- **Notifications**: the profile has a `PlatformNotificationService`
  (`NotificationService`, `engine/browser_context.h`), so
  `Notification.permission` reads `"default"` until a site asks; content
  reports `"denied"` for a profile without one, a headless browser's tell.
  Asking goes through the app's permission prompt as before. The engine
  doesn't show notifications yet: the service drops them.
- **The PDF viewer**: when the app handles PDFs (`pdf_directory`, above),
  `ContentClient::AddPlugins` registers one `application/pdf` plugin, as
  Electron and CEF do, so `navigator.pdfViewerEnabled` is true and
  `navigator.plugins` and `mimeTypes` list Chrome's five PDF viewers and two
  types (Blink makes those fixed lists). The plugin is never created.

Left as they are: `window.chrome` is undefined (as in Electron), and the
hidden verification window's `outerWidth`/`outerHeight` equal the page's
and its `screenX`/`screenY` are 0. The app's check:
`NOTOLIBRA_ENGINE_UI_TEST=1` (its first step, `engine-ui: fingerprint:`).

## Document-start parts

The app's document-start script (`ne_document_start_fn`) may hold several
parts separated by a line of `//` and U+001E; the renderer
(`engine/renderer/frame_agent.cc`) runs each as a script of its own, in
order, so an exception in one doesn't stop the next (globals are shared, as
between a page's `<script>` elements). To an older engine the separator is
a comment, and the parts run as one script. Until October 19, 2026 they
always ran as one: Dark Reader's part ended in an unterminated call, so a
part after it was parsed as a call of its result and threw.

## Importing cookies and local storage

October 19, 2026, so Macklin's Zen sign-ins carry over (the app's
`browser_data` reads Zen's files; native/README.md → Browser data).
`ne_profile_import_cookies` and `ne_profile_import_local_storage`
(`engine/profile_import.cc`) write another browser's data through the
profile's own stores, never their files: Chromium's `Cookies` database is
encrypted with the profile's key (the mock keychain in unsigned builds, the
engine's keychain item in signed ones), and both stores are open while the
engine runs. Counts only are logged.

- **Cookies:** each becomes a `CanonicalCookie` (`FromStorage`, so its
  creation, last-access and last-update times, SameSite, source scheme and
  CHIPS partition are kept) and goes to the network service's
  `CookieManager::SetCanonicalCookie` with HttpOnly allowed and an
  inclusive SameSite context, from `https://<host><path>` (`http` when only
  http ever set a non-secure cookie). Expiry is capped at 400 days from now;
  a cookie whose lifetime from its creation would still exceed Chromium's
  400-day check gets a later creation date (Chromium itself checks a
  replacement against the time it's set and keeps the old creation date,
  which it can't be told from outside). `GetAllCookies` first makes the call
  idempotent: a cookie the profile has with the same value and attributes is
  unchanged; one the profile set more recently (`LastUpdateDate`) is kept;
  anything else is set. A partition's ancestor bit is cross-site unless the
  cookie's site is the top-level site and the source recorded no cross-site
  ancestor. `FlushCookieStore` runs before the completion.
- **Local storage:** per origin, one at a time, the storage partition's
  `LocalStorageControl` binds the first-party `StorageArea`; `GetAll` reads
  what's there, and `Put` writes each item in Blink's format
  (`CachedStorageArea::StringToUint8Vector`: a 1 and Latin-1 bytes, or a 0
  and UTF-16 code units), so pages read the values exactly, unpaired
  surrogates included. The app passes UTF-16 for that reason. Then `Flush`,
  and `GetUsage` as a barrier before the completion.
- **Session cookies now survive a restart.** The network context had
  `persist_session_cookies` but not `restore_old_session_cookies`, so
  Chromium wrote session cookies to the database and deleted them as it
  loaded it at the next start: every session-cookie sign-in was lost on
  quit, though this README and the app's said otherwise. Both are set now,
  as Chrome's "Continue where you left off" does.
- **Check:** the app's `NOTOLIBRA_COOKIE_IMPORT_TEST` (hidden; a synthetic
  Zen profile and a local `*.localhost` site): `document.cookie`, request
  `Cookie` headers (HttpOnly cookies in requests only), a path cookie,
  local storage values, importing again changing nothing, and all of it
  again in a new process on the same profile.

## Licences

Closed source. Only permissive code goes in here: our own, Chromium's
(BSD-3), CEF's (BSD-3, with notices). GPL code (Helium's patches) and LGPL code
(Vivaldi's FFmpeg-derived files) are reference only. Patches that modify
FFmpeg's LGPL files say so in their headers; the modified FFmpeg ships as its
own `libffmpeg.dylib` and is offered in source form under the LGPL.
