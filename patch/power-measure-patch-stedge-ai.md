# Patching ST Edge AI for Power Measurement

This document is the **canonical guide** for applying the ST Edge AI power-measurement patch.

This project benchmarks inference on the **STM32N6570-DK** using **ST Edge AI** (`stedgeai`, `n6_loader.py`). It can also record **average power during NPU inference** when used with an external **INA228** monitor running the Arduino sketch `fyp-power-measure.ino`.

If you **upgrade ST Edge AI** or **reinstall** the ST Edge AI tree, you must **re-apply this patch**.

## Files That May Need Patching

Depending on your build configuration, **both validation source files** may need to be patched:

- `aiValidation_ATON.c` — standard ATON/NPU runtime
- `aiValidation_ATON_ST_AI.c` — ST_AI validation target

Use the **automated patch script** whenever possible. It patches both files **idempotently**.

## Automated Patching (Recommended)

Run the patch script:

```bash
python3 patch_stedgeai_power.py
```

The script:

- Patches both `aiValidation_ATON.c` and `aiValidation_ATON_ST_AI.c`
- Is **idempotent**, so it is safe to run multiple times
- Automatically adds the HAL include, GPIO helpers, and required function calls
- Requires the `STEDGEAI_CORE_DIR` environment variable to be set

After patching, **rebuild and flash** the `NPU_Validation` firmware.

## What the Patch Does

1. **Firmware patch (`aiValidation_ATON.c`)**  
   Adds **static GPIO helper functions** and places calls around **`npu_run()`** so that a GPIO pin is driven high **only during inference**.

   - Default pin: **PD6** on the **STM32N6570-DK** (D7 on CN11, on the back of the board)
   - This was chosen after checking the BSP for conflicts
   - UART/USB protocol I/O is **not** included in the measurement window

2. **Host-side benchmark integration**  
   During `validate`, the benchmark reads the INA228 serial stream when the `--power-serial` flag is provided.

   Relevant path:

   `fyp-playground/src/benchmark/`

There is **no separate** `power_measure_sync.c` file and **no Makefile change** required for `NPU_Validation`. All changes live inside a single middleware source file.

## One-File Patch (Manual Method)

**Source in this repository:**

`patch/aiValidation_ATON_power_sync.inc.c`

**Target in the ST Edge AI installation:**

`$STEDGEAI_CORE_DIR/Middlewares/ST/AI/Validation/Src/aiValidation_ATON.c`

### Manual Steps

1. Open `aiValidation_ATON.c`.

2. Add the following include **once**, alongside the other `#include` lines, if it is not already present:

   ```c
   #include "stm32n6xx_hal.h"
   ```

3. Copy the contents of `aiValidation_ATON_power_sync.inc.c`  
   Paste **everything after the file comment** immediately after the `_dumpable_tensor_name[]` array — that is, after its closing `};` and before `_APP_VERSION_MAJOR_` or `struct aton_context`.

4. Add the required call sites if they are not already present:

   - In **`aiValidationInit()`**, add this line immediately after `cyclesCounterInit();`:

     ```c
     power_measurement_sync_init();
     ```

   - In **`aiPbCmdNNRun`**, wrap the `npu_run()` call like this:

     ```c
     power_measurement_sync_begin();
     npu_run(&ctx->instance, &counters);
     power_measurement_sync_end();
     ```

   If ST changes the file layout in a future release, search for `npu_run(` and ensure that `power_measurement_sync_begin()` and `power_measurement_sync_end()` remain placed **directly around that call**.

5. Rebuild and flash **`NPU_Validation`**  
   For example, use `BUILD_CONF=N6-DK` via `n6_loader.py` or your benchmark loading step.

## Disabling the Patch Without Removing It

To disable the synchronization logic without deleting the code, define:

```c
PWR_MEASUREMENT_SYNC_ENABLE 0
```

Do this **before** the pasted block, either:

- directly in `aiValidation_ATON.c`, or
- via project `CFLAGS`

Alternatively, edit the default value in the pasted section:

```c
#ifndef PWR_MEASUREMENT_SYNC_ENABLE
```

## Changing the GPIO Pin

To use a different GPIO pin, edit the defaults for:

- `PWR_SYNC_GPIO_*`
- `PWR_SYNC_GPIO_RCC_ENABLE`

Make sure the RCC macro matches the selected port. For example, if you move the pin to **port H**, use:

```c
__HAL_RCC_GPIOH_CLK_ENABLE()
```