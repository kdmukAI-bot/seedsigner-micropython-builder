# ESP32-P4 maximal-debug build

The P4 board config (`sdkconfig.board`) is built for **maximum diagnosability** —
we want every hang/crash to leave a usable trail. This documents what's on, why,
and how to read what it produces.

## What's enabled (and why)

| Setting | Why |
|---|---|
| `COMPILER_STACK_CHECK` (GCC `-fstack-protector*`): **DISABLED** | Incompatible with MicroPython's **NLR** (setjmp/longjmp exceptions): the canary epilogues conflict with NLR's longjmp out of functions, so *every* exception breaks with `TypeError: exceptions must derive from BaseException` and the REPL can't run one statement. Use FreeRTOS task-stack canaries (below) for stack-overflow detection instead. |

> **Note:** `-Og` (`COMPILER_OPTIMIZATION_DEBUG`) was tried for cleaner backtraces but
> trips a `-Werror=maybe-uninitialized` in MicroPython's upstream `esp32_rmt.c`
> (`resolution_hz`), which IDF's default `-Werror=all` turns into a build failure.
> We keep board_common's `-O2`/PERF; `.eh_frame` unwinding still yields usable
> backtraces. To use `-Og`, also patch/suppress that warning in `esp32_rmt.c`.
| `ESP_SYSTEM_USE_EH_FRAME` | RISC-V (P4) has no frame pointer — DWARF `.eh_frame` lets the panic handler print a **readable call stack** on serial. |
| `ESP_SYSTEM_PANIC_PRINT_REBOOT` + `ESP_ERR_TO_NAME_LOOKUP` | Panic prints backtrace + named `esp_err_t`, then reboots. |
| `ESP_COREDUMP_ENABLE_TO_FLASH` (ELF, CRC32) | Crash → coredump in the `coredump` flash partition (task stacks + registers; DRAM capture left off to keep dumps small). |
| `ESP_TASK_WDT_PANIC` + idle-task checks, 10 s | A spinning/hung task starves idle → WDT → **panic** → coredump + backtrace. Catches infinite-loop hangs. |
| IDF heap poisoning: **DISABLED** | Both LIGHT and COMPREHENSIVE fault at boot on this P4 build — Load-access-fault in `multi_heap_internal_lock` ← `multi_heap_malloc` on the **first malloc after `heap_init`** (before app code). A poisoning vs. P4 heap-config incompatibility (likely `SPIRAM_XIP_FROM_PSRAM` / a special heap region), not our code; the non-poisoned build boots fine. We use the **LVGL asserts** below instead (better-targeted at the render/font path) + WDT-panic + coredump. Revisit IDF poisoning as its own ESP-IDF investigation. |
| `FREERTOS_CHECK_STACKOVERFLOW_CANARY` + `WATCHPOINT_END_OF_STACK` | Stack overflow detection. |
| `LOG_DEFAULT_LEVEL_INFO`, `LOG_MAXIMUM_LEVEL_VERBOSE` | INFO by default; VERBOSE compiled in — bump any tag at runtime via `esp_log_level_set("tag", ESP_LOG_VERBOSE)`. **Do not** default to DEBUG: the MSPI-DQS tuning component then prints thousands of lines/sec, flooding the shared serial (breaks REPL automation, slows boot). |
| `LV_USE_LOG` + `LV_USE_ASSERT_{NULL,MALLOC,MEM_INTEGRITY,OBJ,STYLE}` | LVGL aborts at the point of corruption (bad font/object pointer) with a backtrace. |

Partition table (`partitions-32MiB-waveshare.csv`) gains a `coredump` partition
(`0xC10000`, 256 KB) — still within the 16 MB-compatible window.

**Tradeoffs:** heap poisoning + `-Og` + stack canaries make the firmware notably
slower and a bit larger. That's intended — this is a debug build. For a perf/
release build, drop the debug block (restore `COMPILER_OPTIMIZATION_PERF`, heap
poisoning `LIGHT`/`NONE`, coredump off).

## Reading a crash

1. **Serial backtrace (primary).** On panic the handler prints the guru-meditation
   reason + a call stack. Capture it:
   ```
   stty -F /dev/ttyACM0 115200 raw -echo
   timeout 15 cat /dev/ttyACM0
   ```
2. **Coredump (deeper).** Extract a full backtrace + task states from flash:
   ```
   docker run --rm --device=/dev/ttyACM0 -v <build>:/workspace -w /workspace IMAGE \
     bash -lc 'source /opt/toolchains/esp-idf/export.sh && \
       python -m esp_coredump --port /dev/ttyACM0 info_corefile /workspace/micropython.elf'
   ```
   (esptool resets the board, which clears the live session — use after the crash.)
3. **LVGL assert / heap-poison abort** fires as a normal panic, so the same
   backtrace tooling applies; the message names the failed check.

## Limitation: blocked-wait deadlocks

The WDT-panic catches *spinning* hangs (starve idle). A task **blocked** on a
semaphore/lock forever does NOT starve idle, so the WDT may not fire. For that
case rely on heap poisoning / LVGL asserts catching the corruption *before* the
block, or trigger a manual coredump (`esp_core_dump_to_flash()`) from a watchdog
path. (The i18n render hang under repeated load/unload is the live example.)
