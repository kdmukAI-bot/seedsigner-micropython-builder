# Board schematics & design files — local reference collection

Downloaded for the ESP32-P4 hardware evaluation ([../esp32-p4-hardware-evaluation.md](../esp32-p4-hardware-evaluation.md)). Reference material, re-fetchable from the sources below.

**Only one binary here is committed:** `elecrow-crowpanel-p4/05_ESP32-P4_Display_5.0_inch_V1.0.sch`. Everything else in this directory is local-only — the collection is ~28 MB of vendor PDFs and Eagle board files, too much to carry in-tree for material that can be re-downloaded.

The Elecrow `.sch` is the exception because it is **load-bearing**, not reference: it is an Eagle XML netlist, and parsing it is what established that board's radio-reset pin, SD width, touch-reset routing and backlight topology — several of which contradict both the vendor wiki and the vendor BSP. Those findings are cited from `docs/elecrow-crowpanel-p4-5in-bringup-plan.md` and the hardware-kb entry, so the source needs to stay reproducible. Parse it rather than reading the PDF:

```python
import xml.etree.ElementTree as ET
root = ET.parse("05_ESP32-P4_Display_5.0_inch_V1.0.sch").getroot()
for net in root.iter('net'):                       # net -> the pins on it
    print(net.get('name'), [(p.get('part'), p.get('pin')) for p in net.iter('pinref')])
```

The Olimex `.kicad_sch` is likewise a greppable text netlist if that board is ever needed.

| Board | Files here | Source |
|---|---|---|
| **Guition JC4880P443** (owned) | ESPHome config = working **pinmap**; vendor schematic zip is a **manual download** — see `guition-jc4880p443/VENDOR-DOCS-NOTE.md` | [jtenniswood/esphome-lvgl](https://github.com/jtenniswood/esphome-lvgl); `pan.jczn1688.com` |
| **Waveshare P4-43** (release target / Guition's twin) | `ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf` | [waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3) |
| **Elecrow CrowPanel Advance 5" P4** (ordered) | Eagle `.sch` + `.brd` + schematic `.pdf` + firmware/C6 upgrade guides | [Elecrow-RD/-CrowPanel-Advanced-5inch-ESP32-P4-…](https://github.com/Elecrow-RD/-CrowPanel-Advanced-5inch-ESP32-P4-HMI-AI-Display-800x480-IPS-Touch-Screen) |
| **M5Stack Tab5** (ordered) | `Tab5_Schematics_PDF.pdf` | m5stack-doc.oss-cn-shenzhen.aliyuncs.com |
| **Olimex ESP32-P4-DevKit** (radio-free reference) | schematic `.pdf` + KiCad `.kicad_sch` (text netlist — greppable) | [OLIMEX/ESP32-P4-DevKit](https://github.com/OLIMEX/ESP32-P4-DevKit) |

Not collected (set-aside boards): Espressif ESP32-P4-Function-EV-Board (reference panels EK79007/ILI9881C) — schematics available from Espressif's `esp-dev-kits` / `esp-bsp` if needed later.

**Guition caveat:** the vendor's full doc package (schematics + pinout + ST7701/GT911/ES8311 drivers + Arduino examples) lives at `pan.jczn1688.com/directlink/1/HMI display/JC4880P443C_I_W.zip` but the network drive requires its JS web UI — a direct fetch returns an HTML landing page, not the file. Download it via a browser and drop it in `guition-jc4880p443/`. Until then, the ESPHome config here provides the working pinmap (see the note).
