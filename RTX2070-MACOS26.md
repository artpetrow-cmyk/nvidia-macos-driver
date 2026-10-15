# RTX 2070 on macOS 26 — fork notes

This branch is a personal fork of [NullMoth's nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)
2.0.1 with the fixes I needed to run one machine day to day. It is not affiliated with NullMoth and is not
submitted upstream.

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
| NVRM: GPU statistics, surface share and page-off for small-BAR Turing | GPU utilisation, temperature and VRAM for Activity Monitor / iStat; shared IOSurface VRAM on a 256 MB BAR1 |
| NVAccel: shared IOSurface VRAM, flip and budget fixes for a 256 MB BAR1 | writer-claimed surface VRAM published to other processes, VRAM image cache |
| NVRMFB: console un-park, transient VRAM grants, flip latch with deferred waits | display VRAM that does not outlive its use; flips that do not wait for the latch twice |
| plugin: RTX 2070 / macOS 26 work ported from 1.15 | web pages drawing wrong in Safari (linear targets read inside their own pass), WebKit GPU sandbox, Unity tessellation and compute `stage_in`, upload and pass caches |
| plugin: ss1 only in iconservicesagent, Mesa disk cache in WebKit GPU processes | stutter in web maps from 2.0's scheduled-after-surface-writes; shaders recompiled on every Safari reload |
| plugin: conf loader runs before the ANGLE opt-in constructor | `NVMTL_ANGLE_METAL=1` in `nvrm610.conf` had no effect |
| plugin: [drawable present] waits for the GPU writes of its drawable | Unity games (Valheim): picture jumping back and forth, menu highlights flickering — WindowServer showed drawables the GPU had not drawn yet |
| NVRM: WindowServer channel groups get the HIGH interleave level and a 2 ms timeslice | WindowServer waiting 5–25 ms for the GPU behind a busy game |
| plugin: NVMTL_ROBUST_BUFFERS=0, per-program knob file beside the conf | ~6 % of GPU time back for games that never read past their buffers; per-program settings that survive a reboot |

## Settings added

- `NVMTL_PRESENT_AFTER_WRITES=0` — turn off the deferred `[drawable present]`.
- `NVMTL_ROBUST_BUFFERS=0` — leave Vulkan `robustBufferAccess` off for a program. Only for programs that never read
  past the end of a buffer: on NVK such a read is an MMU fault that kills the program's GPU channel.
- `NVMTL_SCHEDULE_AFTER_SURFACE_WRITES` — `1` on outside WindowServer, `2` everywhere, `0` nowhere; default: only
  `iconservicesagent`.
- `/Library/GPUBundles/nvmtl/nvmtl-knobs.<program name>` — the same `KEY=value` lines as the per-process file in the
  temporary directory, but kept across reboots. Example for Valheim:
  ```
  NVMTL_ROBUST_BUFFERS=0
  NVRM_610_PREEMPT=0
  ```
  (`NVRM_610_PREEMPT=0` gives the program's channels pixel-level graphics preemption, so WindowServer gets the GPU
  sooner behind it.)
- `debug.nvrm_gpr_on`, `debug.nvrm_gpr_ws_us`, `debug.nvrm_gpr_ws_level`, `debug.nvrm_gpr_app_us` (sysctl) and the
  boot argument `-nvrmnogpr` — WindowServer's GPU priority; settings apply to channel groups created afterwards, so
  WindowServer's own change only with a reboot.

## Building NVRM

`kexts/NVRM/patches/610-rm-gpr1-gpu-priority.patch` changes RM's `src/nvidia/arch/nvalloc/unix/src/escape.c` from
open-gpu-kernel-modules 610.57.04. Apply it to that file before building RM (`patch -p1` from the
open-gpu-kernel-modules root), then link NVRM as usual.

## Known issues

- On this machine Valheim runs about 15 % slower on 2.0.1 than on 1.15 at the same spot and settings (26.8 vs
  31.5 fps, GPU-bound). `robustBufferAccess` explains about 6 %; the rest is not found. The shader translator, NVK,
  the frame-present mode and the WindowServer priority were each ruled out by measurement.
- Turing's copy-engine queue still cannot be built (upstream issue #32); copies run on the graphics channel.
- Restarting WindowServer (`killall WindowServer`) freezes the machine with black screens on this setup.
