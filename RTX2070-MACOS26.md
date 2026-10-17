# RTX 2070 on macOS 26 — fork notes

This branch is a personal fork of [NullMoth's nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)
2.3.4 with the fixes I needed to run one machine day to day. It is not affiliated with NullMoth and is not
submitted upstream. The earlier branch on 2.0.1, `rtx2070-macos26`, is kept as it was.

**How it was made.** The changes were developed with an AI assistant (Claude, Anthropic) doing the investigation,
the code and these notes, and me testing every build on the machine below. NullMoth's contribution rules do not
accept AI-written code, so nothing here is offered to them as a pull request.

**Tested on one machine only:** GeForce RTX 2070 (TU106, 8 GB, 256 MB BAR1), Ryzen 5 5600G, MSI B550 GAMING GEN3, macOS 26.7.1
(25G241), a 2560x1440 100 Hz and a 1920x1080 60 Hz display. Other cards and macOS versions are untested. These are
kernel extensions: a bad build can stop the machine from starting. Use at your own risk. NullMoth published 2.3.4 as
a pre-release for its early-updates testers.

**License.** Unchanged from upstream: PolyForm Noncommercial 1.0.0 ([LICENSE](LICENSE), [NOTICE](NOTICE)).
Noncommercial use only. Source only — no binaries are published here.

## What changed, by commit

| Commit | What it fixes |
|---|---|
| translator: Unity skinning stores, ANGLE compiler barriers, sampled cube faces | Unity skinning/blend-shape kernels and ANGLE (Chrome/Electron) shaders that failed to translate |
| NVRM: GPU statistics, surface share and page-off for small-BAR Turing | GPU utilisation, temperature and VRAM for Activity Monitor / iStat; shared IOSurface VRAM on a 256 MB BAR1 |
| NVAccel: shared IOSurface VRAM, flip and budget fixes for a 256 MB BAR1 | writer-claimed surface VRAM published to other processes, VRAM import cache |
| NVRMFB: console un-park, transient VRAM grants, pure flips wait outside the display gate | display VRAM that does not outlive its use; one display's flip wait no longer holds up the other's |
| plugin: RTX 2070 / macOS 26 work ported to 2.3.4 | web pages drawing wrong in Safari (linear targets read inside their own pass), WebKit GPU sandbox, Unity tessellation and compute `stage_in`, upload and pass caches |
| plugin: ss1 only in iconservicesagent, Mesa disk cache in WebKit GPU processes | stutter in web maps from scheduled-after-surface-writes; shaders recompiled on every Safari reload |
| plugin: conf loader runs before the ANGLE opt-in constructor | `NVMTL_ANGLE_METAL=1` in `nvrm610.conf` had no effect |
| plugin: [drawable present] waits for the GPU writes of its drawable | Unity games (Valheim): picture jumping back and forth, menu highlights flickering |
| NVRM: WindowServer channel groups get the HIGH interleave level and a 2 ms timeslice | WindowServer waiting for the GPU behind a busy game |
| plugin: NVMTL_ROBUST_BUFFERS=0, per-program knob file beside the conf | ~6 % of GPU time back for games that never read past their buffers; per-program settings that survive a reboot |
| plugin: descriptor-set recycling skips framebuffer-fetch sets | with 2.3.4's framebuffer fetch: GPU device lost in Dock, Spotlight, icon rendering and the lock screen |
| NVAccel: a dirty mark on a deallocated surface wires its parked VRAM back; frame timing | moving a window ran at 50 frames a second on a 100 Hz display after WindowServer's surfaces were paged off at login |
| NVRMFB: VBL offset knob and in-flight tick counter (diagnostics) | nothing by default; used to rule out a VBL/flip race |
| plugin: WindowServer commits submit ahead while the execution pump waits for the GPU | short stalls while moving windows with glass (System Settings): WindowServer waited for each frame's composite |

Moving a window on the 100 Hz display, measured with a window that moves on every display-link tick: 50 frames a
second before the last two fixes, 99–100 after (0–2 repeated refreshes in 10 s).

### Left to 2.3.4's own version

The 2.0.1 branch carried its own framebuffer fetch, a present fence for WindowServer and a 1 ms flip-status poll.
2.3.4 has its own for all three (input-attachment fetch, completion marks for WindowServer's composites, flip waits
woken by the display's event), so this branch uses those and only keeps what they do not cover.

## Settings added

- `NVMTL_PRESENT_AFTER_WRITES=0` — turn off the deferred `[drawable present]`.
- `NVMTL_ROBUST_BUFFERS=0` — leave Vulkan `robustBufferAccess` off for a program. Only for programs that never read
  past the end of a buffer: on NVK such a read is an MMU fault that kills the program's GPU channel.
- `NVMTL_SCHEDULE_AFTER_SURFACE_WRITES` — `1` on outside WindowServer, `2` everywhere, `0` nowhere; default: only
  `iconservicesagent`.
- `NVMTL_COMMIT_AHEAD` — `1` on, `0` off; default: on in WindowServer only. In a process that draws IOSurfaces for
  another one it lets the consumer read tiles the producer's GPU has not finished (seen as flickering web pages).
- `NVMTL_DESC_RECYCLE=0` — turn off descriptor-set recycling.
- `/Library/GPUBundles/nvmtl/nvmtl-knobs.<program name>` — the same `KEY=value` lines as the per-process file in the
  temporary directory, but kept across reboots. Example for Valheim:
  ```
  NVMTL_ROBUST_BUFFERS=0
  NVRM_610_PREEMPT=0
  ```
  (`NVRM_610_PREEMPT=0` gives the program's channels pixel-level graphics preemption, so WindowServer gets the GPU
  sooner behind it.)
- `debug.nvaccel_sv_rewire` (sysctl, default 1) — the VRAM rewire of a paged-off surface; counters
  `debug.nvaccel_sv_rewired`, `debug.nvaccel_sv_rewire_other`.
- `debug.nvaccel_ftm` (sysctl, read) — per-head histograms of the zero-copy present path in 1 ms buckets: swap
  interval, WindowServer held in the swap, wait for the composite, flip to latch.
- `debug.nvrmfb_vbl_offset_us`, `debug.nvrmfb_vbl_inflight` (sysctl) — diagnostics, default 0.
- `debug.nvrm_gpr_on`, `debug.nvrm_gpr_ws_us`, `debug.nvrm_gpr_ws_level`, `debug.nvrm_gpr_app_us` (sysctl) and the
  boot argument `-nvrmnogpr` — WindowServer's GPU priority; settings apply to channel groups created afterwards, so
  WindowServer's own change only with a reboot.

## Building NVRM

`kexts/NVRM/patches/610-rm-gpr1-gpu-priority.patch` changes RM's `src/nvidia/arch/nvalloc/unix/src/escape.c` from
open-gpu-kernel-modules 610.57.04. Apply it to that file before building RM (`patch -p1` from the
open-gpu-kernel-modules root), together with 2.3.4's own patches in the same folder, then link NVRM as usual.

## Known issues

- Restarting WindowServer (`killall WindowServer`) freezes the machine with black screens on this setup.
- CoreDisplay runs a full-screen colour pass on the GPU for every frame (the display pipe reports no hardware
  gamma). It costs a fraction of a millisecond here and is left as it is.

## Thanks

Everything in this driver is the work of [NullMoth Systems](https://nullmothsystems.com) ([GitHub](https://github.com/nullmoth)) — the kernel extensions, the Metal plugin,
the shader translator and the NVK port. This branch only adds the fixes listed above on top of their 2.3.4 release.
If the driver is useful to you, support its author: [buymeacoffee.com/nullmoth](https://buymeacoffee.com/nullmoth).
The driver builds on [metal2vulkan](https://github.com/steelbrain/metal2vulkan) by Anees Iqbal, NVIDIA's
[open GPU kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules) and Mesa's NVK — see [NOTICE](NOTICE).
