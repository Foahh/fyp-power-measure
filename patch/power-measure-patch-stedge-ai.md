# Patching ST Edge AI for power measurement (`avg_power_mW`)

This file is the **canonical** guide for the power-measure ST Edge AI patch.

This project benchmarks inference on the STM32N6570-DK using ST Edge AI (`stedgeai`, `n6_loader.py`) and can record **average power during NPU inference** when you use an external INA228 monitor (Arduino sketch in `fyp-power-measure.ino`).

After you **upgrade ST Edge AI** or **reinstall** the tree, re-apply the patch below.

## Which files need patching?

**Both validation files** may need patching depending on your build configuration:
- `aiValidation_ATON.c` - Standard ATON/NPU runtime
- `aiValidation_ATON_ST_AI.c` - ST_AI validation target

Use the **automated script** (recommended) to patch both files idempotently.

## Automated patching (recommended)

Run the idempotent patch script to automatically patch both validation files:

```bash
python3 patch_stedgeai_power.py
```

The script:
- Patches both `aiValidation_ATON.c` and `aiValidation_ATON_ST_AI.c`
- Is idempotent (safe to run multiple times)
- Adds HAL include, GPIO helpers, and function calls automatically
- Requires `STEDGEAI_CORE_DIR` environment variable

After patching, rebuild and flash NPU_Validation firmware.

## What the patch does

1. **`aiValidation_ATON.c`** — Adds **static** GPIO helpers and calls them around **`npu_run()`** so only **inference** is marked high on a pin (default **PD6** on STM32N6570-DK; checked against BSP for conflicts), not UART/USB protocol I/O.
2. **Host** — Benchmark reads the INA228 serial stream during `validate` when `--power-serial` flag is provided (`fyp-playground/scripts/benchmark/`).

There is **no** separate `power_measure_sync.c` and **no** NPU_Validation **Makefile** change: everything lives in one middleware file.

## One-file patch (recommended)

**Source in this repo:** `patch/aiValidation_ATON_power_sync.inc.c`

**Target in ST install:**

`$STEDGEAI_CORE_DIR/Middlewares/ST/AI/Validation/Src/aiValidation_ATON.c`

### Steps

1. Open `aiValidation_ATON.c`.
2. With the other `#include` lines, add **once** (if missing):

   ```c
   #include "stm32n6xx_hal.h"
   ```

3. Paste the **code block** from `aiValidation_ATON_power_sync.inc.c` (everything after the file comment) **immediately after** the `_dumpable_tensor_name[]` array (after the closing `};`), **before** `_APP_VERSION_MAJOR_` / `struct aton_context`.

4. **Call sites** — If your file is stock and does not already call the helpers, add:

   - In **`aiValidationInit()`**, after `cyclesCounterInit();`:

     ```c
       power_measurement_sync_init();
     ```

   - In **`aiPbCmdNNRun`**, wrap **`npu_run`**:

     ```c
       power_measurement_sync_begin();
       npu_run(&ctx->instance, &counters);
       power_measurement_sync_end();
     ```

   If ST refactors the file, search for `npu_run(` and keep **begin/end** directly around that call.

5. Rebuild and flash **NPU_Validation** (e.g. `BUILD_CONF=N6-DK` via `n6_loader.py` / benchmark load step).

### Disable without removing code

Define **`PWR_MEASUREMENT_SYNC_ENABLE`** to **`0`** before the pasted block (e.g. in `aiValidation_ATON.c` or project `CFLAGS`), or edit the `#ifndef PWR_MEASUREMENT_SYNC_ENABLE` default in the pasted section.

### Change GPIO

Edit the **`PWR_SYNC_GPIO_*`** / **`PWR_SYNC_GPIO_RCC_ENABLE`** defaults in the pasted block so the RCC macro matches the port (e.g. `__HAL_RCC_GPIOH_CLK_ENABLE()` if you move to port H).

## Arduino and host benchmark

1. Flash `fyp-power-measure.ino`. It uses **interrupt-driven edge detection** on the sync pin and the **INA228 energy accumulator** for accurate power measurement. Sends binary **protobuf messages** (PowerSample) over serial at 921600 baud. Wire **STM32 sync** (**PD6** by default) to **`IS_INFERENCING_PIN`** (GPIO 3) and common ground.

2. `pip install pyserial protobuf`

3. Run the benchmark with power measurement flags:

   ```bash
   python run_benchmark.py --power-serial /dev/ttyUSB1 --power-baud 921600 --validation-count 50
   ```

   - `--power-serial` — Serial port for INA228 (e.g., `/dev/ttyUSB1`). If omitted, auto-detects ESP32-C6.
   - `--power-baud` — Baud rate (default: `921600`, matches Arduino sketch)
   - `--validation-count` — Number of inference runs per model

When power measurement is enabled, a background thread **appends every protobuf sample** to **`fyp-playground/results/benchmark/power-measure.csv`** with a leading **`host_time_iso`** column (UTC). **`avg_power_mW`** in `fyp-playground/results/benchmark/benchmark_results.csv` uses only samples captured during each model’s **validate** step with `is_inference=True`.

## After an ST Edge AI upgrade

1. Re-paste **`aiValidation_ATON_power_sync.inc.c`** (and `#include "stm32n6xx_hal.h"` + call sites if the vendor file reset).
2. Rebuild and flash.

## Troubleshooting

| Issue | What to check |
|--------|----------------|
| Empty `avg_power_mW` | `--power-serial` not provided, wrong port, `pyserial`/`protobuf` missing |
| No serial output | If no `/dev/ttyUSB*` (and auto-detect) is found for the ESP32 monitor, enable USB CDC ("USC-CDC") so the board exposes a serial device (typically `/dev/ttyACM*`), then pass it via `--power-serial` |
| Power looks like whole-run average | Sync GPIO not wired; benchmark falls back to all samples |
| Build errors in the new block | Pin/port conflict or RIF/security; change `PWR_SYNC_*` / RCC macro |
| HAL / GPIO undeclared | `stm32n6xx_hal.h` include path / N6 build only |
| Serial read errors | Check baud rate is 921600, Arduino sketch flashed correctly |
