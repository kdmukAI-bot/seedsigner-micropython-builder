"""Public `seedsigner_lvgl_screens` facade (ESP32 / MicroPython).

The shared SeedSigner app imports `seedsigner_lvgl_screens` and drives locale
selection through ONE dir-based API on every platform. On the Pi the native `.so`
implements that API directly (it can open files). On ESP32 the C module CANNOT open
the SD card -- ESP-IDF's fatfs can't link beside MicroPython's oofatfs (see
docs/knowledge/micropython-fatfs-vs-esp-idf-fatfs-collision.md) -- so it is byte-based
(`load_locale(locale, packs_dict)`, `register_pack_manifest(bytes)`,
`locale_picker_screen(cfg, endonym_images_dict)`). This facade closes that gap: it
mounts the microSD, does the pack reads in Python, and hands the bytes to the private
C module `_seedsigner_lvgl_screens`, exposing the SAME dir-based API the Pi native
module (seedsigner-raspi-lvgl native/python_bindings/module.cpp) exposes:

    set_locale(locale, font_dir="lang-packs") -> bool
    unload_locale()
    discover_locale_packs(font_dir="lang-packs") -> int
    list_available_locales(font_dir="lang-packs") -> list[{code,endonym,image,has_image}]
    locale_picker_screen(cfg)      # cfg carries font_dir + rows

Every other name (init, the screens, poll_for_result, mem_stats, qr_*, ...) passes
straight through from the C module.
"""
import json
import os

import _seedsigner_lvgl_screens as _c

# Re-export the whole C surface (init, screens, poll_for_result, mem_stats, qr_*, ...).
# The dir-based locale wrappers DEFINED BELOW then shadow the byte-based C versions.
globals().update({_k: getattr(_c, _k) for _k in dir(_c) if not _k.startswith("_")})


# --- microSD mount --------------------------------------------------------
# The packs live on the microSD (the user-writable "packs partition"). The C boot
# already powers the card's VDD rail (display_manager sd_power_on); we mount its FAT
# volume here so a relative font_dir ("lang-packs") resolves under this mount -- and
# so the app's gettext localedir (pointed at the same pack root) finds each pack's
# LC_MESSAGES/messages.mo. Fail-soft: no card -> packs unavailable, app runs on the
# baked Western floor + English.
_SD_MOUNT = "/sd"
_sd_ready = False


def _ensure_sd():
    global _sd_ready
    if _sd_ready:
        return True
    try:
        os.stat(_SD_MOUNT)             # already mounted (prior call / boot)?
        _sd_ready = True
        return True
    except OSError:
        pass
    try:
        import machine
        import vfs
        sd = machine.SDCard(slot=0, width=4)   # slot 0 = IOMUX; VDD via LDO_VO4
        vfs.mount(vfs.VfsFat(sd), _SD_MOUNT)
        _sd_ready = True
    except Exception:
        _sd_ready = False
    return _sd_ready


def _resolve(font_dir):
    """Absolute on-SD path for the app's pack root. A relative dir (the shared
    "lang-packs" constant) resolves under the SD mount; an absolute dir passes
    through (a host that sets LOCALE_PACK_DIR to a full path)."""
    if not font_dir:
        font_dir = "lang-packs"
    if font_dir.startswith("/"):
        return font_dir
    return _SD_MOUNT + "/" + font_dir


def _is_junk(name):
    """Desktop-OS cruft a cross-platform FAT/exFAT card accumulates -- never mistake
    it for a pack (defensive discovery on a user-writable volume)."""
    return name.startswith(".") or name == "System Volume Information"


def _read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def _listdir(path):
    try:
        return os.listdir(path)
    except OSError:
        return []


def _active_height():
    """Active display-profile height (240/320/480), for choosing the endonym image.
    Read from the C module's own profile report -- no extra native binding needed."""
    try:
        return json.loads(_c.list_available_locales())["profile"]["height"]
    except Exception:
        return 480


# --- dir-based locale API (mirrors the Pi native module) ------------------

def discover_locale_packs(font_dir="lang-packs"):
    """(Re)scan <font_dir> and register every SD language pack's manifest so
    set_locale()/list_available_locales() work for a locale not compiled into the
    firmware. Returns the count registered (0 when the card is absent). Defensive:
    a bad/half-copied manifest is skipped, never fatal."""
    base = _resolve(font_dir)
    if not _ensure_sd():
        return 0
    try:
        _c.clear_pack_manifests()
    except Exception:
        pass
    count = 0
    for name in _listdir(base):
        if _is_junk(name):
            continue
        mbytes = _read(base + "/" + name + "/manifest.json")
        if mbytes is None:
            continue          # e.g. a .mo-only pack (baked-Latin) -- no font manifest
        try:
            if _c.register_pack_manifest(mbytes):
                count += 1
        except Exception:
            pass
    return count


def list_available_locales(font_dir="lang-packs"):
    """One dict per FONT pack under <font_dir> -- {code, endonym, image, has_image} --
    for the app to build the locale-picker rows (unioned with its own baked-Latin
    locales). Pure read; .mo-only packs (no manifest.json) are skipped -- the app
    already knows those locales. Empty list when the card is absent."""
    base = _resolve(font_dir)
    out = []
    if not _ensure_sd():
        return out
    height = _active_height()
    for name in _listdir(base):
        if _is_junk(name):
            continue
        mbytes = _read(base + "/" + name + "/manifest.json")
        if mbytes is None:
            continue
        try:
            m = json.loads(mbytes)
        except Exception:
            continue          # malformed manifest -> skip (fail closed)
        code = m.get("locale")
        if not code:
            continue
        images = m.get("endonym_images") or {}
        entry = images.get(str(height))
        image = None
        if isinstance(entry, dict):
            image = entry.get("file") or ("endonym_%d.bin" % height)
        elif entry:
            image = "endonym_%d.bin" % height
        out.append({"code": code,
                    "endonym": m.get("endonym") or None,
                    "image": image,
                    "has_image": bool(image)})
    return out


def set_locale(locale, font_dir="lang-packs"):
    """Load <locale>'s font pack from <font_dir>/<locale>/ so screens render in its
    script. Reads the files the loader asks for off the SD, stages the bytes, and
    drives the byte-based C loader. Returns True on success; False if a pack file is
    missing/unreadable (the app keeps running on the baked Western floor). A
    baked-floor locale (en, es, ...) needs no font and succeeds trivially."""
    if not locale:
        try:
            _c.unload_locale()
        except Exception:
            pass
        return True
    base = _resolve(font_dir)
    pack_dir = base + "/" + locale
    if _ensure_sd():
        # Register the SD manifest (if present) so an SD-only locale becomes loadable
        # and locale_pack_files() knows its files (an SD pack overrides a compiled one).
        mbytes = _read(pack_dir + "/manifest.json")
        if mbytes is not None:
            try:
                _c.register_pack_manifest(mbytes)
            except Exception:
                pass
    try:
        files = json.loads(_c.locale_pack_files(locale))
    except Exception:
        files = []
    packs = {}
    for fn in files:
        data = _read(pack_dir + "/" + fn)
        if data is None:
            return False       # missing pack file -> loader restores the baked floor
        packs[fn] = data
    try:
        return bool(_c.load_locale(locale, packs))
    except Exception:
        return False


def locale_picker_screen(cfg=None):
    """The language-selection screen. Stages each image row's pre-rendered endonym
    image (endonym_<active-height>.bin) off the SD, keyed "<locale>/<file>", and hands
    the dict to the C screen -- which paints the native-script names with no runtime
    font. Live-text (Latin) rows carry no "image" and need no staging."""
    cfg = cfg or {}
    base = _resolve(cfg.get("font_dir"))
    endonym_images = {}
    if _ensure_sd():
        height = _active_height()
        for row in cfg.get("rows", []):
            img = row.get("image")
            if not img:
                continue
            fn = img if isinstance(img, str) else ("endonym_%d.bin" % height)
            locale = row.get("locale", "")
            data = _read(base + "/" + locale + "/" + fn)
            if data is not None:
                endonym_images[locale + "/" + fn] = data
    _c.locale_picker_screen(cfg, endonym_images)


# Mount the card at import so pack reads (and the app's gettext .mo open() under the
# same mount) work regardless of call order.
_ensure_sd()
