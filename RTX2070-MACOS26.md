# RTX 2070 on macOS 26 — fork notes

This branch is a personal fork of [NullMoth's nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)
2.6.2 with the fixes I needed to run one machine day to day. It is not affiliated with NullMoth and is not submitted
upstream. The earlier branches, `rtx2070-macos26` (2.0.1) and `rtx2070-macos26-2.3.4`, are kept as they were.

**How it was made.** The changes were developed with an AI assistant (Claude, Anthropic) doing the investigation,
the code and these notes, and me testing every build on the machine below. NullMoth's contribution rules do not
accept AI-written code, so nothing here is offered to them as a pull request.

**Tested on one machine only:** GeForce RTX 2070 (TU106, 8 GB, 256 MB BAR1), Ryzen 5 5600G, MSI B550 GAMING GEN3, macOS 26.7.1
(25G241), a 2560x1440 100 Hz and a 1920x1080 60 Hz display. Other cards and macOS versions are untested. These are
kernel extensions: a bad build can stop the machine from starting. Use at your own risk.

**License.** Unchanged from upstream: PolyForm Noncommercial 1.0.0 ([LICENSE](LICENSE), [NOTICE](NOTICE)).
Noncommercial use only. Source only — no binaries are published here.

## What changed, by commit

| Commit | What it fixes |
|---|---|
| translator: Unity skinning stores, ANGLE compiler barriers, sampled cube faces | Unity skinning/blend-shape kernels and ANGLE (Chrome/Electron) shaders that failed to translate |
| NVRM: GPU statistics, surface share and page-off for small-BAR Turing; WindowServer channel priority | GPU utilisation and VRAM for Activity Monitor / Stats; shared IOSurface VRAM on a 256 MB BAR1; WindowServer waiting for the GPU behind a busy game |
| NVAccel: shared IOSurface VRAM, parked VRAM rewire, GPU statistics, slow-start headless check | window moves at 50–94 frames a second on a 100 Hz display after the login page-off; GPU figures in monitors; a late-starting card taken for a display-less one |
| NVRMFB: console un-park, transient VRAM grants, pure flips wait outside the display gate | display VRAM that does not outlive its use; one display's flip wait no longer holds up the other's |
| plugin: RTX 2070 / macOS 26 work ported to 2.6.2 | see below |
| tools: tsync2test | regression test for buffer textures whose rows are written after the texture was made |

The plugin commit carries the 2.3.4 branch's work (web pages drawn wrong in Safari because linear targets were read
inside their own pass, the WebKit GPU sandbox, Unity tessellation and compute `stage_in`, upload and pass caches,
descriptor-set recycling that skips framebuffer-fetch sets, the deferred `[drawable present]`, commit-ahead in
WindowServer only) and four fixes found on 2.5/2.6:

- **Safari: letters missing from canvas web apps, scrambled pages.** WebKit's GPU process takes an IOSurface for the
  CPU (putImageData, getImageData) once its command buffer is *scheduled*, and an IOSurface's seed moves at the lock.
  With *scheduled* reported before the GPU had finished writing the surface, a CPU write landed under a VRAM copy
  already called current and a read saw old pixels. Scheduled-after-surface-writes is now on by default in
  `com.apple.WebKit.GPU`.
- **Electron/Chromium with Skia Ganesh: green JPEG avatars, blank emoji.** ANGLE stages every texture upload in a fresh
  managed buffer. The pre-submit prologue took the owed VRAM clears before the managed uploads, so a buffer made and
  written on another thread in between got its upload in one submission and its clear in the next — on top of the
  uploaded bytes, before the copy that read them. Owed clears now go first in the same prologue.
- **Buffer textures that fall back to a copy** (the linear view refused for the offset or row pitch) upload their rows
  at their first use instead of at creation: CoreGraphics writes the glyph mask after it makes the texture.
- **Release syslog**: refusals without a FAIL word (a pixel format answered nil, a surface left unsynced, a storage
  write lost) are logged, so a page drawn wrong leaves a trace.

Safari draws pages on the GPU here only because NVAccel points Metal at a copy of the plugin under
`/System/Library/Extensions` (WebKit's GPU sandbox maps code only from `/System`); with the stock layout Safari draws
on the CPU, as upstream does. Most of the Safari fixes above matter only with that copy in place.

Measured on the 100 Hz display with a window that moves on every display-link tick: 99.5 frames a second (one
repeated refresh in 6 s); with a window that animates its content: 98 % of refreshes carry a new frame.

### Left to upstream's own version

The 2.0.1 branch carried its own framebuffer fetch, a present fence for WindowServer, a 1 ms flip-status poll, a dirty
mark that rewired parked VRAM and a commit-time submit-ahead. Upstream now has its own for all of them, so this branch
uses those and only keeps what they do not cover.

## Settings added

- `NVMTL_SCHEDULE_AFTER_SURFACE_WRITES` — `1` on outside WindowServer, `2` everywhere, `0` nowhere; default: only
  `iconservicesagent` and `com.apple.WebKit.GPU`.
- `NVMTL_COMMIT_AHEAD` — `1` on, `0` off; default: on in WindowServer only.
- `NVMTL_PRESENT_AFTER_WRITES=0` — turn off the deferred `[drawable present]`.
- `NVMTL_ROBUST_BUFFERS=0` — leave Vulkan `robustBufferAccess` off for a program that never reads past its buffers.
- `NVMTL_DESC_RECYCLE=0` — turn off descriptor-set recycling.
- `NVMTL_TEXTURE_SYNC=0` — no refresh of textures over host-copied buffers.
- `/Library/GPUBundles/nvmtl/nvmtl-knobs.<program name>` — the same `KEY=value` lines as `nvrm610.conf`, for one program,
  kept across reboots.
- `debug.nvaccel_sv_rewire`, `debug.nvaccel_ftm`, `debug.nvaccel_gst_*` (sysctl), boot argument `-nvaccelnogst`.
- `debug.nvrm_gpr_on`, `debug.nvrm_gpr_ws_us`, `debug.nvrm_gpr_ws_level`, `debug.nvrm_gpr_app_us` (sysctl), boot argument
  `-nvrmnogpr` — WindowServer's GPU priority.

## Building NVRM on NVIDIA 615.78.08

Apply to the open-gpu-kernel-modules 615.78.08 tree, from its root: `kexts/NVRM/patches/610-rm-gpr1-gpu-priority.patch`
and `615-fsp-timer-ostimer-handle.patch` (RM), and upstream's NVKMS patches in the same folder. 615's
`src/nvidia-modeset/Makefile` adds `-mfunction-return=thunk-extern`: every function then returns through
`__x86_return_thunk`, which the macOS kernel does not export, and the kernel collection refuses NVRM ("Failed to bind
'___x86_return_thunk'"). Remove that line before building NVKMS; the other compiler flags are the same as for 610.

## Known issues

- Restarting WindowServer (`killall WindowServer`) freezes the machine with black screens on this setup.
- HiDPI "looks like" sizes larger than the panel are not offered by macOS for a 2560x1440 display here, even with
  `nvfbhidpi=1` and a display override; scaled non-HiDPI sizes work.

## Thanks

Everything in this driver is the work of [NullMoth Systems](https://nullmothsystems.com) ([GitHub](https://github.com/nullmoth)) — the kernel extensions, the Metal plugin,
the shader translator and the NVK port. This branch only adds the fixes listed above on top of their 2.6.2 release.
If the driver is useful to you, support its author: [buymeacoffee.com/nullmoth](https://buymeacoffee.com/nullmoth).
The driver builds on [metal2vulkan](https://github.com/steelbrain/metal2vulkan) by Anees Iqbal, NVIDIA's
[open GPU kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules) and Mesa's NVK — see [NOTICE](NOTICE).
