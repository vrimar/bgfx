# WebGPU external device repro

Dawn, Chrome and Firefox harnesses for "WGPU: Added external device via `PlatformData::context`".

| Build | Ref | |
|---|---|---|
| base | `HEAD~2` | parent; ignores `PlatformData::context` |
| control | `CONTROL_REF`, optional | earlier revision: `AllowProcessEvents` callbacks, pumps the app's instance, adapter caps |
| pr | `HEAD` | the change (this branch only adds `repro/`) |

Each test prints `OK`/`BUG` and `as expected`/`UNEXPECTED`, or `SKIP` when the browser gives bgfx
no frame to check; runners exit non-zero on `UNEXPECTED`. App-device tests skip base.

## Run

Needs Linux with Vulkan, `g++`, an active emsdk, Node 22+, Chrome and/or Firefox (`CHROME=`,
`FIREFOX=` if not on `PATH`), and `bx`/`bimg` beside `bgfx`.

```sh
git fetch https://github.com/vrimar/bgfx.git repro/wgpu-external-device-control   # optional
CONTROL_REF=FETCH_HEAD ./build.sh   # pinned Dawn; worktrees in .build/repro-webgpu, ~250 MB each
./native.sh [--tsan]                # TSan crashes on NVIDIA; use Mesa: VK_DRIVER_FILES=.../radeon_icd.json
node web.mjs [--browser=firefox] [W1 W3 ...]
```

Overrides: `BASE_REF`, `PR_REF`, `DAWN_FLAVOR=Debug`, `OUT`, `JOBS`.

## Results

base `66f6e9780`, control `30166e6e6`, pr `86cdb1a42`; emscripten 6.0.13, Chrome 156, Firefox 158,
RTX 4070 (RADV for TSan). All as expected.

| Native | base | control | pr |
|---|---|---|---|
| `caps --no-indirect` | — | BUG: `DRAW_INDIRECT` set | OK |
| `appcb` ×3 | — | BUG: bgfx runs the app's callback | OK |
| `lost` | — | BUG: not noticed | OK: `DeviceLost` → Noop |
| `readback [--own]` | OK | OK | OK |
| `cpu --own` | BUG: 100% of a core | BUG: 100% | OK: 5% |
| `pump` | — | OK | OK |
| `pump` under TSan ×3 | — | BUG: 10, 8, 3 races | OK: 0 |

| Browser | Device | base | control | pr |
|---|---|---|---|---|
| W1 render | page | — | OK | OK |
| W1n W1 with `GPUDevice.adapterInfo` hidden | page | — | BUG: init throws | OK |
| W2- / W2+ `DRAW_INDIRECT` | page | — | BUG / OK | OK / OK |
| W3 `read`, no Asyncify | page | — | BUG: hang | OK |
| W4 `device.destroy()` | page | — | BUG: not noticed | OK |
| W5 init A, then B; destroy A, B | page | — | BUG: B missed | OK |
| W6 `read`, Asyncify | own | BUG: hang | BUG: hang | OK |
| W6b W6 without `timestamp-query` | own | BUG: throws | BUG: throws | OK |
| W7 occlusion | page | — | BUG: "used in submit while mapped" | OK |
| W1t W1 with `timestamp-query` | page | — | — | INFO: blank |

page = `Module.preinitializedWebGPUDevice`; own = bgfx's device, needs Asyncify. The table is Chrome;
Firefox matches except W4 and W5.

- Firefox W4/W5: SKIP on control and pr. After `device.destroy()` Firefox stops delivering a scheduled
  `requestAnimationFrame`, so bgfx never gets the frame that would report the loss. A plain-JS page
  hits this too, intermittently.
- W3: `read` returns empty without Asyncify; web readback needs Asyncify/JSPI.
- Unrelated: `timestamp-query` breaks `TimerQuery` passes (`endOfPassWriteIndex 4294967295`) in every
  build and both browsers: W1t is blank, W6 checks only completion, W6b checks the data.

Cleanup: `rm -rf .build/repro-webgpu && git worktree prune`
