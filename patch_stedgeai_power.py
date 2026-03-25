#!/usr/bin/env python3
"""
Idempotent patch script for ST Edge AI power measurement sync.
Patches both aiValidation_ATON.c and aiValidation_ATON_ST_AI.c
"""
import os
import sys
import re
from pathlib import Path

# Read the GPIO sync code from the patch file
PATCH_FILE = Path(__file__).parent / "patch/aiValidation_ATON_power_sync.inc.c"

def read_sync_code():
    """Read the GPIO sync helper code from patch file"""
    with open(PATCH_FILE, 'r') as f:
        lines = f.readlines()

    # Skip the header comment, start from the separator line
    start_idx = None
    for i, line in enumerate(lines):
        if line.strip().startswith('/* ----------'):
            start_idx = i
            break

    if start_idx is None:
        raise ValueError("Could not find sync code start marker")

    return ''.join(lines[start_idx:])

def is_already_patched(content):
    """Check if file already has the power measurement sync code"""
    return 'power_measurement_sync_init' in content

def has_hal_include(content):
    """Check if file already includes stm32n6xx_hal.h"""
    return re.search(r'#include\s+"stm32n6xx_hal\.h"', content)

def add_hal_include(content):
    """Add HAL include after other includes"""
    # Find last #include line
    lines = content.split('\n')
    last_include_idx = -1
    for i, line in enumerate(lines):
        if line.strip().startswith('#include'):
            last_include_idx = i

    if last_include_idx == -1:
        raise ValueError("No #include lines found")

    # Insert after last include
    lines.insert(last_include_idx + 1, '#include "stm32n6xx_hal.h"')
    return '\n'.join(lines)

def insert_sync_helpers(content, sync_code):
    """Insert GPIO sync helpers after _dumpable_tensor_name array"""
    # Find the closing }; of _dumpable_tensor_name array
    pattern = r'(static\s+const\s+char\s+\*_dumpable_tensor_name\[\]\s*=\s*\{[^}]*\};)'
    match = re.search(pattern, content, re.DOTALL)

    if not match:
        raise ValueError("Could not find _dumpable_tensor_name array")

    insert_pos = match.end()
    return content[:insert_pos] + '\n\n' + sync_code + '\n' + content[insert_pos:]

def add_init_call(content):
    """Add power_measurement_sync_init() after cyclesCounterInit()"""
    if 'power_measurement_sync_init();' in content:
        return content  # Already added

    pattern = r'(cyclesCounterInit\(\);)'
    replacement = r'\1\n  power_measurement_sync_init();'
    return re.sub(pattern, replacement, content)

def add_npu_run_wrapper(content):
    """Wrap npu_run() with begin/end calls"""
    if 'power_measurement_sync_begin();' in content:
        return content  # Already wrapped

    # Find npu_run call and wrap it
    pattern = r'(\s*)(npu_run\(&ctx->instance, &counters\);)'
    replacement = r'\1power_measurement_sync_begin();\n\1\2\n\1power_measurement_sync_end();'
    return re.sub(pattern, replacement, content)

def patch_file(filepath, sync_code):
    """Patch a single validation file (idempotent)"""
    print(f"Patching {filepath}...")

    if not os.path.exists(filepath):
        print(f"  ⚠️  File not found: {filepath}")
        return False

    with open(filepath, 'r') as f:
        content = f.read()

    # Check if already patched
    if is_already_patched(content):
        print(f"  ✓ Already patched")
        return True

    original_content = content

    try:
        # Step 1: Add HAL include if missing
        if not has_hal_include(content):
            print(f"  → Adding stm32n6xx_hal.h include")
            content = add_hal_include(content)

        # Step 2: Insert GPIO sync helpers
        print(f"  → Inserting GPIO sync helper functions")
        content = insert_sync_helpers(content, sync_code)

        # Step 3: Add init call
        print(f"  → Adding power_measurement_sync_init() call")
        content = add_init_call(content)

        # Step 4: Wrap npu_run
        print(f"  → Wrapping npu_run() with sync begin/end")
        content = add_npu_run_wrapper(content)

        # Write back
        with open(filepath, 'w') as f:
            f.write(content)

        print(f"  ✓ Patched successfully")
        return True

    except Exception as e:
        print(f"  ✗ Error: {e}")
        # Restore original content on error
        with open(filepath, 'w') as f:
            f.write(original_content)
        return False

def main():
    """Main entry point"""
    print("ST Edge AI Power Measurement Patch Script")
    print("=" * 50)

    # Get ST Edge AI path from environment
    stedgeai_dir = os.environ.get('STEDGEAI_CORE_DIR')
    if not stedgeai_dir:
        print("✗ STEDGEAI_CORE_DIR environment variable not set")
        print("  Set it to your ST Edge AI installation path")
        sys.exit(1)

    validation_src = Path(stedgeai_dir) / "Middlewares/ST/AI/Validation/Src"
    if not validation_src.exists():
        print(f"✗ Validation source directory not found: {validation_src}")
        sys.exit(1)

    # Read sync code
    print(f"\nReading sync code from: {PATCH_FILE}")
    sync_code = read_sync_code()

    # Patch both validation files
    files_to_patch = [
        validation_src / "aiValidation_ATON.c",
        validation_src / "aiValidation_ATON_ST_AI.c"
    ]

    print()
    results = []
    for filepath in files_to_patch:
        results.append(patch_file(filepath, sync_code))

    # Summary
    print("\n" + "=" * 50)
    if all(results):
        print("✓ All files patched successfully")
        print("\nNext steps:")
        print("  1. Rebuild NPU_Validation firmware")
        print("  2. Flash to STM32N6570-DK")
        print("  3. Run benchmark with power measurement")
    else:
        print("⚠️  Some files failed to patch")
        sys.exit(1)

if __name__ == "__main__":
    main()
