# ESP32-P4 Hard Hardware Facts — Cited Research Synthesis

Date: 2026-06-27. Primary sources: Espressif ESP32-P4 Chip Revision v1.3 Datasheet (v1.2),
ESP32-P4 datasheet HTML, ESP-IDF Programming Guide (PPA + external-RAM), espressif.com product page.

---

## 1. PSRAM

**FACT:**
- In-package (in chip's package), not external on the v1.3 part: **16 MB or 32 MB PSRAM** (datasheet cover page: "16 MB or 32 MB PSRAM in the chip's package"). The Waveshare board's 32 MB matches the 32 MB SKU (ESP32-P4NRW32 / its v3.x successor).
- Interface: **16-bit data bus** (a.k.a. "16-line" / "HEX"), OPI/HPI-style controller. ESP-IDF refers to it as **"HEX (16-line PSRAM on ESP32P4)"**.
- Max clock frequency: **250 MHz** (datasheet §4.1.3.1 System and Memory).
- Theoretical peak bandwidth: **8 Gbit/s = 1 GB/s**, computed by Espressif in-datasheet as **"16 × 2 × 250 MHz = 8 Gbit/s"** in DDR mode (16-bit bus × 2 for DDR × 250 MHz). 8 Gbit/s ÷ 8 = **1.0 GB/s**.

**DISCREPANCY TO FLAG:** The ESP-IDF external-RAM guide uses **"200 MHz"** as its illustrative figure for the 16-line PSRAM, whereas the v1.3 datasheet states a **250 MHz** max. The 8 Gbit/s number is the datasheet's own max-spec arithmetic at 250 MHz. At 200 MHz the same formula gives 16×2×200 MHz = 6.4 Gbit/s = 0.8 GB/s. Real-world throughput is lower than either theoretical max. The Zephyr features page also says "up to 64 MB" external PSRAM (HEX/octal) — likely forward-looking/other SKUs; the official v1.3 datasheet caps the in-package part at 32 MB.

**SOURCE URLs:**
- Datasheet HTML §4.1.3.1: https://documentation.espressif.com/esp32-p4_datasheet_en.html  ("16 × 2 × 250 MHz = 8 Gbit/s")
- Datasheet PDF cover ("16 MB or 32 MB PSRAM in the chip's package"): https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.pdf
- ESP-IDF external RAM (HEX 16-line @ 200 MHz example): https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/external-ram.html

**CONFIDENCE:** high (interface, bus width, 8 Gbit/s = 1 GB/s from datasheet). medium on the single "true" clock (250 vs 200 MHz source conflict).
**TYPE:** datasheet (official) + official ESP-IDF docs.

---

## 2. Internal SRAM

**FACT:** On-chip SRAM totals **808 KB**, broken down (datasheet Features / §4.1.3.1) as:
- **768 KB HP L2MEM** (high-performance L2 memory — this is the framebuffer-usable pool)
- **32 KB LP SRAM** (low-power)
- **8 KB system SPM** (Scratchpad Memory)
- (plus 128 KB HP ROM + 16 KB LP ROM, which are ROM, not RAM)

The **768 KB HP L2MEM** is the usable on-chip RAM for framebuffers/working data. Note: when external PSRAM is enabled, part of L2MEM also serves as the two-level cache, so not all 768 KB is freely available to the application. The ~768 KB figure you saw is **confirmed**.

**SOURCE URLs:**
- Datasheet PDF Features list (p.3): https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.pdf — "768 KB HP L2MEM", "32 KB LP SRAM", "8 KB system SPM"
- Datasheet HTML §4.1.3.1: https://documentation.espressif.com/esp32-p4_datasheet_en.html
- Corroboration: https://docs.zephyrproject.org/latest/boards/espressif/common/soc-esp32p4-features.html ("768 KB of high-performance SRAM")

**CONFIDENCE:** high.  **TYPE:** datasheet (official).

---

## 3. CPU

**FACT:**
- HP: **32-bit RISC-V dual-core** processor. **IMPORTANT NUANCE:** the v1.3 datasheet states "up to **360 MHz**" as the default, with a note: *"The default clock frequency is configured to 360 MHz. If you require a higher clock frequency of 400 MHz, please contact us."* The **400 MHz** figure (widely advertised) was made the standard max in the **v3.x chip upgrade** ("now reaches a maximum clock speed of 400 MHz (up from 360 MHz)"). So: **360 MHz default on older silicon; 400 MHz on v3.x / by request.**
- CoreMark (dual-core @ 360 MHz): **2489.62 CoreMark; 6.92 CoreMark/MHz**.
- LP: **32-bit RISC-V single-core** processor, **up to 40 MHz**.

**SOURCE URLs:**
- Datasheet PDF Features (p.3): https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.pdf — "32-bit RISC-V dual-core processor up to 360 MHz", "single-core processor up to 40 MHz", "2489.62 CoreMark"
- v3.x upgrade (400 MHz): https://www.espressif.com/en/news/ESP32_P4_v3.x_Upgrade
- Product page ("dual-core RISC-V CPU running at speeds up to 400 MHz"): https://www.espressif.com/en/products/socs/esp32-p4

**CONFIDENCE:** high. (Dual-core RISC-V + 40 MHz LP core certain; the 400 vs 360 MHz depends on silicon revision — flagged.)
**TYPE:** datasheet (official) + official news.

---

## 4. PPA (Pixel-Processing Accelerator)

**FACT:**
- Operations: **scale, rotate, mirror (SRM), blend (FG over BG), and fill.** Confirmed by ESP-IDF PPA API docs.
- Rotation: **0°, 90° CCW, 180° CCW, 270° CCW** (discrete 90° steps only).
- Scaling: bilinear; scaling factor has an **8-bit integer part + 4-bit fractional part** (per-axis H and V), i.e. ~0.0625 to 255.9375.
- Color-space conversion: **YUV<->RGB (BT.601 and BT.709).**
- Pixel formats — SRM: ARGB8888, RGB888, RGB565, YUV420, YUV444, YUV422 variants, GRAY8. Blend: adds A8, A4 alpha-only. Fill: ARGB8888/RGB888/RGB565/YUV422_UYVY/GRAY8.
- v3.x upgrade adds **32x32 block processing** and broader **YUV422/YUV420** support.

**ENGINE COUNT — GAP / LOWER CONFIDENCE:** The ESP-IDF PPA API docs and datasheet describe the PPA as having **a (one) SRM engine and a (one) Blending engine** as the two functional units (the API exposes one SRM operation path and one Blend operation path; clients are serialized onto them). However, **neither the public datasheet HTML nor the ESP-IDF API guide states an explicit integer count** ("the PPA has N SRM engines"). The exact count "1 SRM + 1 Blend" is **strongly implied but not verbatim-confirmed** from the public docs I could fetch — the definitive Features bullet list lives in TRM Chapter 29 (PPA), which was too large to fetch in full. Treat "one SRM + one Blend engine" as **medium confidence**.
- No explicit max throughput (pixels/s or MB/s) is published; datasheet only says transaction time is "proportional to the amount of data in the block."

**SOURCE URLs:**
- ESP-IDF PPA API (operations, rotation angles, formats, CSC): https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html
- v3.x upgrade (32x32 blocks, YUV422/420): https://www.espressif.com/en/news/ESP32_P4_v3.x_Upgrade
- TRM (Chapter 29 PPA — engine counts, not fully fetched): https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_technical_reference_manual_en.pdf
- LVGL PPA-SRM tracking issue (community corroboration): https://github.com/lvgl/lvgl/issues/10260

**CONFIDENCE:** high on operations/formats/rotation/scaling; **medium on "exactly one SRM + one Blend engine"**; gap on throughput.
**TYPE:** official ESP-IDF docs + official news; engine-count gap is TRM-only.

---

## 5. 2D-DMA / DW-GDMA (memory-to-memory 2D transfers)

**FACT:** The ESP32-P4 has multiple DMA controllers (datasheet §4.1.2, functional block diagram):
- **GDMA Controller (GDMA-AHB, GDMA-AXI):** general DMA, "**three transmit + three receive** channels per controller" (§4.1.2.1).
- **VDMA Controller (VDMA):** "**four channels** for unidirectional data transfer" (§4.1.2.2).
- **2D-DMA Controller (2D-DMA):** the image-dedicated engine — "**four memory-to-peripheral channels + three peripheral-to-memory channels**" (§4.1.2.3). It has "all the features of GDMA-AXI" **plus macroblock reordering and color-space conversion (CSC)."**
- **DW-GDMA:** a separate Synopsys DesignWare GDMA; appears as a block in the diagram and is one of the **ISP input channels** (MIPI-CSI / DVP / DW-GDMA).

**ROTATION:** The DMA engines themselves do **not** perform arbitrary image rotation. 2D-DMA does block/macroblock reordering + CSC and feeds the **PPA**, which is the engine that performs scale/rotate(90/180/270)/mirror/blend. So memory-to-memory **rotation = PPA's job, with 2D-DMA moving the 2D blocks.**

**SOURCE URLs:**
- Datasheet HTML §4.1.2: https://documentation.espressif.com/esp32-p4_datasheet_en.html
- ESP-IDF / docs (2D-DMA = GDMA-AXI + macroblock reorder + CSC; DW-GDMA as ISP input): see Memory/ISP guides under https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/

**CONFIDENCE:** high on channel counts and the "2D-DMA feeds PPA, PPA rotates" architecture.
**TYPE:** datasheet (official) + official docs.

---

## 6. MIPI-CSI and MIPI-DSI + ISP

**FACT:**
- **MIPI-CSI (camera):** complies with **MIPI CSI-2**, **D-PHY v1.1**, **2 data lanes × 1.5 Gbps/lane** = **3.0 Gbps aggregate**. Input formats: RGB888, RGB666, RGB565, YUV422, YUV420, RAW8, RAW10, RAW12.
- **MIPI-DSI (display):** complies with **MIPI DSI**, **D-PHY v1.1**, **2 data lanes × 1.5 Gbps/lane** = **3.0 Gbps aggregate**. Output formats: RGB888, RGB666, RGB565. Supports **video mode** (DPI-style streaming) and fixed test-pattern output.
- **ISP:** integrated with MIPI-CSI. **Max resolution 1920×1080** (datasheet §4.2.1.2). Input: **RAW8/RAW10/RAW12**; pipeline includes demosaic, white balance, color correction; outputs RGB and YUV variants. Three input channels: **MIPI-CSI, DVP, DW-GDMA.** Chip-level H.264 encoder rated **1080p@30fps**.

**AGGREGATE LINK BANDWIDTH:** 2 lanes × 1.5 Gbps = **3.0 Gbit/s = 375 MB/s** per interface (CSI in, DSI out), independent.

**DISCREPANCY/CONFIDENCE NOTE:** The **2-lane × 1.5 Gbps** and **D-PHY v1.1** figures are **consistent across many board/spec pages** (VIEWE specs page, multiple Waveshare/DFRobot/board datasheets) but the **public Espressif datasheet HTML truncated §4.2.1.6/4.2.1.7** in my fetch, so I could not get the verbatim Espressif sentence for lane rate. The numbers are almost certainly correct (D-PHY v1.1 max is 1.5 Gbps/lane, matching) — graded **medium-high**: corroborated widely, but the exact Espressif-datasheet quote is in the TRM/datasheet PDF body I could not extract. ISP 1920×1080 + RAW formats **are** directly from the Espressif datasheet HTML (high confidence).

**SOURCE URLs:**
- ISP (datasheet HTML §4.2.1.2, 1920×1080, RAW8/10/12): https://documentation.espressif.com/esp32-p4_datasheet_en.html
- MIPI 2-lane × 1.5 Gbps, D-PHY v1.1 (corroborating spec pages):
  - https://viewedisplay.com/esp32-p4-specs/
  - https://www.waveshare.com/wiki/ESP32-P4-Nano-StartPage
  - https://wiki.dfrobot.com/dfr1237/
- H.264 1080p@30 (product page): https://www.espressif.com/en/products/socs/esp32-p4

**CONFIDENCE:** ISP specs high; MIPI lane/rate **medium-high** (widely corroborated, exact Espressif quote unverified due to HTML truncation).
**TYPE:** datasheet (official) for ISP; board datasheets + spec aggregator for MIPI lane rate.

---

## 7. Display/DSI scanout bandwidth (480×800 RGB565 @ 60 Hz)

**FORMULA:** scanout_BW = width × height × bytes_per_pixel × refresh_Hz

**ARITHMETIC (active pixels only, RGB565 = 2 bytes/px, 60 Hz):**
- 480 × 800 = 384,000 pixels
- × 2 bytes = **768,000 bytes per frame** (≈ 750 KB/frame)
- × 60 Hz = **46,080,000 bytes/s = 46.08 MB/s ≈ 0.046 GB/s** (≈ **368.64 Mbit/s** on the wire-equivalent)

(800×480 is identical — same pixel count.)

**Headroom check vs the buses:**
- vs **PSRAM 1.0 GB/s theoretical (8 Gbit/s @ 250 MHz)**: 46 MB/s ÷ 1000 MB/s ≈ **4.6%** of peak PSRAM bandwidth for scanout. Plenty of headroom in theory; in practice double-buffering + PPA composite reads/writes multiply the traffic (a full-frame PPA blit reads+writes ~1.5 MB, and at UI refresh rates adds tens of MB/s more), but 480×800@60 scanout is comfortably within budget.
- vs **MIPI-DSI 2-lane × 1.5 Gbps = 3.0 Gbit/s link**: 368.64 Mbit/s of payload ≈ **12.3%** of the DSI link's raw 3.0 Gbit/s (before D-PHY 8b/encoding overhead). Comfortable.

**Real overscan note:** with typical blanking the pixel clock is higher than the active-pixel rate (e.g. ~525×840 total → ~26.5 MHz pixel clock → ~53 MB/s RGB565), so budget ~50–55 MB/s of PSRAM read for a real 480×800@60 DPI panel.

**SOURCE:** arithmetic (self-derived from the panel geometry + the RGB565 = 2 bytes/pixel and 60 Hz inputs the task supplied); bus ceilings from facts 1 and 6 above.
**CONFIDENCE:** high (arithmetic).  **TYPE:** computed.

---

## Gaps / disagreements flagged
1. **PSRAM clock 250 MHz (datasheet) vs 200 MHz (ESP-IDF guide example).** 8 Gbit/s = 1 GB/s is the datasheet max at 250 MHz; at 200 MHz it'd be 6.4 Gbit/s. Real throughput < both.
2. **CPU 400 MHz (v3.x / advertised) vs 360 MHz default (v1.3 datasheet, 400 "on request").** Silicon-revision dependent.
3. **PSRAM size 32 MB (v1.3 in-package max) vs "up to 64 MB" (Zephyr page).** Use 32 MB for the Waveshare part.
4. **PPA engine count "1 SRM + 1 Blend"** is implied by the API but not verbatim in fetched public docs — exact count is in TRM Ch.29 (not fully fetched). Medium confidence.
5. **MIPI lane rate (2×1.5 Gbps, D-PHY v1.1)** widely corroborated but the exact Espressif-datasheet sentence was truncated in the HTML render; not pulled verbatim from Espressif. Medium-high.
6. **No published PPA throughput number** (pixels/s or MB/s).

## All source URLs used
- https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.pdf  (official datasheet PDF, v1.3 / v1.2 doc — primary)
- https://documentation.espressif.com/esp32-p4_datasheet_en.html  (official datasheet HTML — §4.1.2, §4.1.3.1, §4.2.1.2)
- https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_technical_reference_manual_en.pdf  (TRM — PPA Ch.29; too large to fully fetch)
- https://www.espressif.com/en/products/socs/esp32-p4  (official product page)
- https://www.espressif.com/en/news/ESP32_P4_v3.x_Upgrade  (official: 400 MHz, PPA 32x32/YUV)
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html  (official PPA API)
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/external-ram.html  (official: HEX 16-line @ 200 MHz)
- https://docs.zephyrproject.org/latest/boards/espressif/common/soc-esp32p4-features.html  (corroboration)
- https://viewedisplay.com/esp32-p4-specs/  (MIPI 2×1.5 Gbps corroboration — spec aggregator, lower tier)
- https://www.waveshare.com/wiki/ESP32-P4-Nano-StartPage  (board datasheet, MIPI corroboration)
- https://wiki.dfrobot.com/dfr1237/  (board datasheet, MIPI corroboration)
- https://github.com/lvgl/lvgl/issues/10260  (PPA-SRM community corroboration)
