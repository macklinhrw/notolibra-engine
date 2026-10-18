# Building on macOS without Xcode.app

Findings from October 18, 2026, at Chromium `156.0.8078.36`, on the M3 Max with
the Command Line Tools 26.6 (`com.apple.pkg.CLTools_Executables`
26.6.0.0.1781586589) and no Xcode.app.

## What Chromium's macOS build takes from Xcode

Google's bots use a "hermetic Xcode" fetched from CIPD into
`src/build/mac_files/xcode_binaries` (`build/mac_toolchain.py`). Its package
definition, `build/xcode_binaries.yaml`, lists everything the macOS build
uses from Xcode:

| Item | Used for | In the Command Line Tools 26.6? |
| --- | --- | --- |
| `MacOSX26.5.sdk` (build `25F70`) | headers, frameworks, `-isysroot` | **Yes**, the exact SDK `mac_sdk.gni` pins for official builds (`mac_sdk_official_version = "26.5"`, `mac_sdk_official_build_version = "25F70"`) |
| `bison`, `gm4` | a few generated parsers | Yes |
| Metal toolchain (`metal`, `metallib`, `air-lld`) | ANGLE's internal Metal shaders, compiled at build time (`third_party/angle/src/libANGLE/renderer/metal/BUILD.gn`) | **No.** Xcode 26 unbundled it too: `xcodebuild -downloadComponent MetalToolchain` |
| `xcodebuild -version` | the Xcode version and build string only (`build/config/apple/sdk_info.py`) | No (`xcodebuild` refuses to run from the Command Line Tools) |
| Xcode's directory layout | `build/mac/find_sdk.py` looks under `Platforms/MacOSX.platform/Developer/SDKs` and `Toolchains/XcodeDefault.xctoolchain/usr/bin` | No (the Command Line Tools use `SDKs/` and `usr/bin/`) |

What it does **not** need:
- **`ibtool`**: the tree has no `.xib`, `.nib` or `.storyboard` files outside
  iOS. `compile_ib_files.py` is used only by `mobile_bundle_data` (iOS).
- **`actool`**: `build/toolchain/apple/toolchain.gni` defines
  `compile_xcassets` only for iOS ("we intentionally disallow future uses of
  xcassets on macOS", crbug.com/965663). Chrome's icon is a checked-in,
  precompiled `chrome/app/theme/chromium/mac/Assets.car`.
- **The compiler and linker**: Chromium's own clang and lld
  (`third_party/llvm-build`), plus `llvm-otool`, `llvm-nm`, `llvm-strip` and
  `llvm-install-name-tool`.
- **Signing and notarizing**: `codesign` is in `/usr/bin`, and `notarytool`
  and `stapler` are in the Command Line Tools.

## How we build without it

1. `scripts/clt-xcode check` builds `src/build/mac_files/xcode_binaries`
   (gitignored by Chromium) out of symlinks into
   `/Library/Developer/CommandLineTools`, with the layout `find_sdk.py`
   expects and a `Contents/version.plist` that `sdk_info.py` reads instead of
   running `xcodebuild`. Its build string, `CLT26.6.0.0.1781586589`, says
   plainly that it is the Command Line Tools.
2. `build/args/clt.gn` sets `use_system_xcode = false`, so `mac_sdk.gni` uses
   that directory as the hermetic Xcode. `scripts/build` imports `clt.gn`
   whenever `xcodebuild -version` fails.
3. `patches/clt-build/` adds `angle_metal_build_time_internal_shaders` (default
   `true`) to ANGLE; `clt.gn` sets it `false`, and ANGLE then compiles its
   internal shaders from source at run time (`DisplayMtl.mm` already has that
   path: `CreateShaderLibrary(gDefaultMetallibSrc)`). The cost is a one-off
   shader compile when a GPU process first creates an ANGLE Metal display.

No tracked Chromium file changes except the one-line ANGLE GN arg.

## With Xcode installed

Install **Xcode 26.6** (it carries the same macOS 26.5 SDK, build `25F70`, as
these Command Line Tools; any Xcode whose SDK is 26.5 or newer works for
non-official builds, since `find_sdk.py` takes the lowest SDK at or above
`mac_sdk_min = "15"`), then:

```sh
sudo xcode-select -s /Applications/Xcode.app
sudo xcodebuild -license accept
xcodebuild -downloadComponent MetalToolchain      # Xcode 26 ships without it
cd ~/chromium/src/notolibra
scripts/clt-xcode remove                           # back to system Xcode
scripts/build ...                                  # stops importing clt.gn
```

Switching changes the SDK path in every compile command, so the next build is
a full rebuild. There is no need to switch for development builds: the
Command Line Tools build uses the same SDK. Switch when ANGLE's precompiled
shaders matter: performance checks and release builds. (The official-build
assert in `mac_sdk.gni` that demands hermetic Xcode applies only to
Chrome-branded builds, so it doesn't affect us either way.)
