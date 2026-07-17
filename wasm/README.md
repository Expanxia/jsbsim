# kiro-wasm: JSBSim as a standalone WASM module

This directory (branch `kiro-wasm`) builds `jsbsim.wasm` — a WASI reactor
module exposing a flat, versioned C ABI (`abi.h`) — for consumption by the
Kiro engine (native via wasmtime, web via the browser's WASM engine).
No emscripten, no JS glue, no filesystem requirement.

Plan/status: `Kiro/docs/plans/jsbsim_wasm_flight_plan.md` (+`_status`).

## Layout

| Path | What |
|---|---|
| `abi.h` | The ABI: versioned `#[repr(C)]`-mirrorable structs, status codes |
| `facade.cpp` | Exports (`jsb_*`), instance table (gen-tagged handles), boundary try/catch |
| `memvfs.{h,cpp}` | In-memory VFS; serves the two guarded hooks in JSBSim source |
| `ground_host.{h,cpp}` | `FGGroundCallback` → host import `env.kiro_ground_query` |
| `CMakeLists.txt` / `build.sh` | wasi-sdk build → `dist/jsbsim.wasm` + import audit |
| `tools/pack.py` | Aircraft data packer → `.jsbpack` + manifest + `catalog.json` |
| `harness/` | Rust/wasmtime contract tests + native-vs-wasm trajectory validation |
| `ref/` | Native (MSVC) build of the same scenario as the golden reference |

## Source patches carried on this branch (all guarded, no-ops upstream)

- `simgear/misc/sg_path.cxx` — `SGPath::exists()` consults the MemVFS
  (`#ifdef JSBSIM_MEMVFS`) — covers every `CheckPathName` resolution site.
- `input_output/FGXMLFileRead.cpp` — `LoadXMLDocument` parses from the MemVFS.
- `models/FGOutput.cpp`, `models/FGInput.cpp` — socket types excluded under
  `JSBSIM_WASM_MINIMAL` (wasi-libc has no sockets).
- `simgear/misc/strutils.cxx` — `strerror_r`: wasi/musl takes the POSIX
  branch (returns `int` even under `_GNU_SOURCE`).

## Build

```
WASI_SDK=/path/to/wasi-sdk-33 ./build.sh      # → dist/jsbsim.wasm (+imports.txt)
python tools/pack.py --root .. --id c172 --model c172p \
       --name "Cessna C-172P" --out dist
```

Toolchain pins (verified 2026-07-17): wasi-sdk 33; flags
`-fwasm-exceptions -mllvm -wasm-use-legacy-eh=false` (+ same via `-Wl,-mllvm`
at link) and `-lunwind` — the exnref EH encoding is REQUIRED (wasmtime rejects
LLVM's default legacy encoding). LTO off. Runtime: wasmtime 46.0.1
(`gc` crate feature + `Config::wasm_exceptions(true)`; CLI `-W exceptions=y`);
browsers/V8 need exnref (node 22: `--experimental-wasm-exnref`).

## Validate

```
cmake -B ref/build ref && cmake --build ref/build --config Release
ref/build/Release/mini_ref ../.. > dist/ref_trajectory.csv
cargo run --release --manifest-path harness/Cargo.toml
```

Harness = 13 contract tests (handles, struct sizes, NaN rejection, VFS,
`<output>` stripping, multi-instance, ground hit/miss/counters) + a 100 s
c172p scenario diffed against the native build. Measured 2026-07-17:
**bit-identical for the full 30 s pre-transient segment**; post-transient
deltas stay inside chaos envelopes (cross-libm ULP differences amplified by
the c172p spiral mode — see `harness/src/main.rs` tolerance notes);
12.4 µs/step end-to-end through the batched ABI.
