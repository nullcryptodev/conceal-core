# CN-GPU PoW GPU offload

## 1. Preamble

GPU offload speeds up **sync** after the mainnet **checkpoint zone** (heights ≤ **2 070 000** skip PoW; full CN-GPU verify starts at **2 070 001**).

It does **not** change consensus. The CPU reference hash is still authoritative. The GPU only helps when it finishes **before** the CPU needs a block’s PoW.

**Speculative method (short):**

1. When P2P delivers blocks ahead of the validated tip, the daemon **queues** PoW jobs on a background OpenCL worker.
2. When the CPU validates a block, it **checks a cache** first.
3. **Cache hit** → use the GPU hash for `check_hash` (fast).
4. **Cache miss** → run CPU longhash immediately (no waiting on the GPU).

Near the chain tip (small catch-up), the daemon turns this off and uses CPU only — OpenCL round-trips would cost more than they save.

---

## 2. Install

**Build with OpenCL:**

```bash
cmake -B build -DWITH_OPENCL=ON
cmake --build build
```

If OpenCL is missing, the daemon builds without GPU PoW (CPU only).

**AMD GPU access (typical):** install ROCm OpenCL (or vendor ICD), add your user to `render` and `video`, log out and back in:

```bash
sudo usermod -aG render,video $USER
```

**Test (optional):**

```bash
./build/tests/PowGpuEquivalenceTests
```

---

## 3. How to use

### List devices

```bash
./build/src/conceald --gpu-list
```

Prints OpenCL compute devices with a **global index** (use that index with `--gpu-device`). Exits without starting the daemon. If OpenCL sees nothing, sysfs PCI hints may still be listed.

### Offload tuning flags

```bash
./build/src/conceald --gpu-offload-help
```

Prints batch/prefetch/debug options (omitted from main `--help`). Tuning flags still work on the command line and in the config file.

### Run with GPU (typical)

```bash
./build/src/conceald --gpu-device 0 --data-dir /path/to/data
```

GPU tuning flags apply only when OpenCL init and self-test succeed.

### Flags at a glance

| Flag | Role | Default |
|------|------|---------|
| `--gpu-device N` | Enable GPU; `N` = index from `--gpu-list` | off (`-1`) |
| `--gpu-list` | List devices and exit | — |
| `--gpu-offload-help` | List tuning/debug flags and exit | — |
| `--gpu-batch-size` | Max hashes per OpenCL dispatch | `128` |
| `--gpu-min-batch-size` | Min queued jobs before dispatch (unless wait expires) | auto: `peers × 4` |
| `--gpu-max-wait-us` | Max wait (µs) to fill a batch before partial flush | auto: `peers × 2500` |
| `--gpu-prefetch-window` | Max height ahead of tip allowed for prefetch | auto: `peers × 128` |
| `--gpu-prefetch-depth` | Max PoW jobs queued/in-flight | auto: `peers × 64` |
| `--gpu-backlog-threshold` | Min catch-up gap or batch size to use GPU | `16` |
| `--gpu-cpu-verify` | Always CPU longhash at verify (paranoid) | off |
| `--gpu-debug-crosscheck` | CPU re-check every GPU result on worker | off |
| `--gpu-debug-inner` | CN-GPU inner trace (debug) | off |

Environment: `CN_GPU_INNER_TRACE=1` — same effect as `--gpu-debug-inner` for CPU trace.

---

## 4. Each flag (with example)

### a) `--gpu-device N`

Turns on OpenCL PoW offload. `N` is the global index from `--gpu-list`. Omit or use default `-1` for CPU-only.

```bash
./build/src/conceald --gpu-device 0 --data-dir ~/.conceal
```

If init or self-test fails, the daemon logs the error and falls back to CPU (no silent GPU).

### b) `--gpu-list`

Discovery only; does not enable the GPU.

```bash
./build/src/conceald --gpu-list
```

### c) `--gpu-batch-size`

**Hard cap** on how many hashes one OpenCL kernel launch processes. Not “blocks downloaded from P2P.”

```bash
./build/src/conceald --gpu-device 0 --gpu-batch-size 64 --data-dir ~/.conceal
```

Use when the GPU is fast but driver overhead per launch is high — sometimes lowering helps stability; raising can help throughput if the queue stays full.

### d) `--gpu-min-batch-size`

**Soft floor:** the worker waits until at least this many jobs are queued before dispatching (unless `max-wait` fires). `0` = no floor (dispatch smaller batches sooner).

```bash
./build/src/conceald --gpu-device 0 --gpu-min-batch-size 0 --data-dir ~/.conceal
```

Good if metrics show tiny `avgBatch` and many cache misses. Default **32** with `batch-size 128` is aimed at bulk sync.

**Default (auto):** `peers × 4`, capped by `--gpu-batch-size` (8 peers → **32**, 12 → **48**). Rises with P2P auto-scale so the worker waits for larger dispatches when jobs arrive in smaller bursts per connection. Set the flag to pin a fixed floor.

### e) `--gpu-max-wait-us`

How long (microseconds) the oldest queued job may wait before the worker flushes a **partial** batch.

```bash
./build/src/conceald --gpu-device 0 --gpu-max-wait-us 15000 --data-dir ~/.conceal
```

**Default (auto):** `peers × 2500` µs (8 peers → **20000**, 12 → **30000**).

Increase further if prefetch is bursty and batches stay undersized; decrease if the GPU sits idle while the CPU catches up.

### f) `--gpu-prefetch-window`

**Upper bound** on how far **ahead of the validated tip** a height may be queued: `tip < height ≤ tip + window`.

Does **not** require P2P to send that many blocks. You only prefetch blocks you already have (usually a smaller P2P batch). The window only limits how far ahead GPU work is allowed during large catch-up.

**Default (auto):** `peers × 128` (8 peers → **1024**, 10 → **1280**).

```bash
./build/src/conceald --gpu-device 0 --gpu-prefetch-window 512 --data-dir ~/.conceal
```

Omit the flag to keep auto scaling; set it explicitly to pin a fixed window.

### g) `--gpu-backlog-threshold`

GPU prefetch and cache use run only when **either** the peer catch-up gap **or** the current P2P batch remainder is ≥ this value. Below that (near tip), verify stays on CPU.

```bash
./build/src/conceald --gpu-device 0 --gpu-backlog-threshold 32 --data-dir ~/.conceal
```

Higher → GPU used only on heavier catch-up. Lower → GPU used sooner (may hurt near-tip latency).

### h) `--gpu-prefetch-depth`

Max PoW jobs **in the GPU pipeline** at once (queued + in flight). Usually matters more than `prefetch-window` for typical P2P batch sizes.

**Default (auto):** `peers × 64` (8 peers → **512**, 10 → **640**), updated when P2P auto-scale changes the target connection count.

```bash
./build/src/conceald --gpu-device 0 --gpu-prefetch-depth 512 --data-dir ~/.conceal
```

Raise if `jobsSubmitted` grows but `gpuHits` stay low (GPU busy but CPU not consuming results in time). Omit the flag to scale with outgoing peers.

### i) `--gpu-cpu-verify`

Disables the fast path: every verify runs CPU longhash. If a GPU result was ready, it is compared (`gpuCacheMismatch` in metrics).

```bash
./build/src/conceald --gpu-device 0 --gpu-cpu-verify --data-dir ~/.conceal
```

For debugging drivers/kernels — not for production sync speed.

### j) `--gpu-debug-crosscheck`

On the OpenCL worker, re-runs full CPU PoW for each completed GPU hash. Expensive; logs mismatches.

```bash
./build/src/conceald --gpu-device 0 --gpu-debug-crosscheck --data-dir ~/.conceal
```

### k) `--gpu-debug-inner`

Enables CN-GPU inner-loop trace on stderr (also `CN_GPU_INNER_TRACE=1`), mainly for self-test / kernel debugging.

```bash
./build/src/conceald --gpu-device 0 --gpu-debug-inner --data-dir ~/.conceal
```

---

## 5. CN-GPU PoW metrics line

Every **256** CN-GPU blocks (with GPU enabled), the daemon logs one line, for example:

```text
CN-GPU PoW metrics @ height 2070256: gpuHits=180 gpuMismatch=0 cpuPowUsed=76 jobsSubmitted=512 batches=8 avgBatch=64 cpuFallback=0
```

Counters are **cumulative since daemon start**.

| Field | Meaning |
|-------|---------|
| `gpuHits` | Verify used a **ready** GPU hash (no CPU longhash) |
| `cpuPowUsed` | Verify ran **CPU** longhash (miss, near-tip, or `--gpu-cpu-verify`) |
| `gpuMismatch` | GPU cache differed from CPU (`gpuCacheMismatch`; only with `--gpu-cpu-verify`) |
| `jobsSubmitted` | Hashes the GPU worker **finished** |
| `batches` | OpenCL **dispatch** count |
| `avgBatch` | Average jobs per dispatch (compare to `min-batch` / `batch-size`) |
| `cpuFallback` | OpenCL batch failed; worker used CPU inner — should stay **0** |

**Quick health check**

- **Good sync:** `gpuHits` rises; `gpuHits / (gpuHits + cpuPowUsed)` goes **up** during long catch-up.
- **GPU too slow:** `jobsSubmitted` grows but `gpuHits` barely move → try higher `prefetch-depth` or `batch-size`, or lower `min-batch-size`.
- **Tiny batches:** `avgBatch` always low → raise `min-batch-size` or `max-wait-us`.
- **Broken GPU path:** `cpuFallback` > 0 → driver/OpenCL errors; check logs.
- **Wrong hash:** `gpuMismatch` > 0 with `--gpu-cpu-verify` → treat as a bug.

**Example:** `gpuHits=180`, `cpuPowUsed=76` → about **70%** of verifies used the GPU cache at that point in the run. `avgBatch=32` matches default `min-batch-size` (worker often flushes at the floor).

