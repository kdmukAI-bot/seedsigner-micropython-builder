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
_sd_dev = None                       # the live machine.SDCard object (None when unmounted)
_sd_scratch = bytearray(512)         # preallocated one-sector probe buffer (no per-poll alloc)
_SD_POLL_INTERVAL_MS = 1000          # min gap between hotplug bus probes
_sd_last_poll = None

# SDMMC data lines this board actually routes. A board fact, not a fleet constant: most
# route all four, but one that brings out only DAT0 cannot enumerate a card if asked for
# 4, so the C side reports what is wired. Resolved once here (it is compile-time truth)
# rather than inside _ensure_sd, whose fail-soft except would swallow a missing binding
# into a silent "no SD card" -- firmware predating the binding falls back to the 4 this
# used to hardcode.
try:
    _SD_BUS_WIDTH = sd_bus_width()
except NameError:
    _SD_BUS_WIDTH = 4


def _ensure_sd():
    """Mount the microSD at _SD_MOUNT if not already, holding the block device in _sd_dev.
    Idempotent + fail-soft. The facade is the SINGLE owner of the /sd lifecycle (D-8): the
    app delegates via sd_ensure()/sd_live()/sd_poll() rather than mounting itself, so there
    is exactly one authority over the mount when hotplug adds umount/remount.

    The SDCard object is constructed ONCE and kept for the device's lifetime. A hotplug
    remount reuses it and re-mounts — it must NEVER be deinit()'d (that frees the SDMMC
    host's transaction mutex, and the esp-idf re-init leaves it NULL → the next read
    crashes; see docs/knowledge/esp32-p4-sdcard-hotplug-no-host-deinit.md). Re-enumeration
    of a freshly-inserted card is handled by the C primitive _sd_dev.reinit_slot(), which
    re-inits the SDMMC *slot* in place (restoring 400 kHz/1-bit probing) while keeping the
    global host + mutex alive; the first read after remount then re-runs CMD0/enumeration."""
    global _sd_ready, _sd_dev
    if _sd_ready:
        return True
    try:
        os.stat(_SD_MOUNT)             # already mounted (prior call / boot)?
        _sd_ready = True
        return True
    except OSError:
        pass
    try:
        import vfs
        if _sd_dev is None:
            import machine
            _sd_dev = machine.SDCard(slot=0, width=_SD_BUS_WIDTH)   # slot 0 = IOMUX
        else:
            # Remount after a hotplug removal: re-init the SDMMC *slot* in place so a
            # freshly-powered (reinserted) card re-enumerates. Construction ran the slot
            # init once; reusing the object never does, so the slot stays at the previous
            # card's clock/width and a fresh card can't probe (EBUSY). reinit_slot()
            # restores 400 kHz/1-bit probing without touching the global host + mutex. If
            # the card is still absent, the mount below fails and we retry next poll.
            try:
                _sd_dev.reinit_slot()
            except Exception:
                pass
        vfs.mount(vfs.VfsFat(_sd_dev), _SD_MOUNT)        # first read re-enumerates a fresh card
        _sd_ready = True
    except Exception:
        _sd_ready = False              # keep _sd_dev; a later poll retries the remount
    return _sd_ready


def sd_ensure():
    """Public: ensure /sd is mounted; returns whether it is. The app's
    MicroSD.ensure_mounted() delegates here so the facade stays the single mounter."""
    return _ensure_sd()


def sd_live():
    """Public: honest "is /sd usable right now". A bare os.stat(/sd) stays True after a
    physical pull (the mount registration lingers); this probes the card itself (read
    sector 0) so a gone card reads False. Backs the app's ESP MicroSD.is_inserted."""
    if not _sd_ready:
        return False
    dev = _sd_dev
    if dev is None:
        # Mounted but we don't hold the handle (shouldn't happen under single-owner) —
        # fall back to the registration check.
        try:
            os.stat(_SD_MOUNT)
            return True
        except OSError:
            return False
    try:
        return bool(dev.readblocks(0, _sd_scratch))
    except Exception:
        return False


def sd_poll():
    """Public: microSD hotplug tick, called from the app's LVGL pump loop. Self-throttled
    to _SD_POLL_INTERVAL_MS so the pump can call it every iteration cheaply. Returns
    "removed" or "inserted" on a state change (having umounted / remounted /sd), else None.
    The facade owns the umount-on-remove / remount-on-insert; the quiescent-point,
    GIL-serialized call site (no other thread touches the SD) makes the umount safe."""
    global _sd_ready, _sd_dev, _sd_last_poll
    import time
    now = time.ticks_ms()
    if _sd_last_poll is not None and time.ticks_diff(now, _sd_last_poll) < _SD_POLL_INTERVAL_MS:
        return None
    _sd_last_poll = now

    if _sd_ready:
        if sd_live():
            return None                # still present
        # Removal: drop the stale FS mount so is_inserted reads false and later opens fail
        # cleanly instead of reading a gone card. No open handles at this quiescent point,
        # so the umount is metadata-only; swallow any error regardless. Then clear the
        # CACHED card-init (ioctl DEINIT) defensively so no stale read is served while the
        # card is out (the actual slot re-init on reinsert is done by reinit_slot() in
        # _ensure_sd). Do NOT deinit the SDCard/host — that frees the SDMMC transaction
        # mutex and the re-init leaves it NULL, crashing the next read
        # (docs/knowledge/esp32-p4-sdcard-hotplug-no-host-deinit.md). The host is idle while
        # the card is out; the LDO rail is held for life regardless.
        try:
            import vfs
            vfs.umount(_SD_MOUNT)
        except Exception:
            pass
        dev = _sd_dev
        if dev is not None:
            try:
                dev.ioctl(2, 0)        # MP_BLOCKDEV_IOCTL_DEINIT: clear cached card-init only
            except Exception:
                pass
        _sd_ready = False
        return "removed"

    # Unmounted: try to (re)mount a freshly-inserted card.
    if _ensure_sd():
        return "inserted"
    return None


def _resolve(font_dir):
    """Absolute path for the app's pack root. An absolute dir passes through -- this is how
    the app selects the store: "/lang-packs" (packs baked into the on-board vfs) or "/sd"
    (the microSD). A relative dir resolves under the SD mount (legacy default)."""
    if not font_dir:
        font_dir = "lang-packs"
    if font_dir.startswith("/"):
        return font_dir
    return _SD_MOUNT + "/" + font_dir


def _ensure_base(base):
    """Ensure `base` (a resolved pack root) is readable. An on-board pack root lives in the
    internal vfs, always mounted at "/", so it needs no card -- this is what lets the app
    read the baked /lang-packs packs with NO microSD inserted. Only an SD-anchored base
    requires the card be mounted. (SD-first override precedence over an on-board copy is a
    later phase; today the app passes one root.)"""
    if base == _SD_MOUNT or base.startswith(_SD_MOUNT + "/"):
        return _ensure_sd()
    return True


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
    """Active display-profile height (240/320/480), for choosing the endonym image
    and for the animated-QR density lookup. Read straight from the C display profile
    via display_size(), so it resolves the real panel BEFORE any locale load (the
    older list_available_locales() path needs the locale table to be populated)."""
    try:
        return _c.display_size()[1]
    except Exception:
        return 480


# --- dir-based locale API (mirrors the Pi native module) ------------------

def discover_locale_packs(font_dir="lang-packs"):
    """(Re)scan <font_dir> and register every SD language pack's manifest so
    set_locale()/list_available_locales() work for a locale not compiled into the
    firmware. Returns the count registered (0 when the card is absent). Defensive:
    a bad/half-copied manifest is skipped, never fatal."""
    base = _resolve(font_dir)
    if not _ensure_base(base):
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
    if not _ensure_base(base):
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
    if _ensure_base(base):
        # Register the pack's manifest (if present) so a pack-only locale becomes loadable
        # and locale_pack_files() knows its files (a pack manifest overrides a compiled one).
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


def settings_locale_picker_screen(cfg=None):
    """The language-selection screen. Stages each image row's pre-rendered endonym
    image (endonym_<active-height>.bin) off the SD, keyed "<locale>/<file>", and hands
    the dict to the C screen -- which paints the native-script names with no runtime
    font. Live-text (Latin) rows carry no "image" and need no staging.

    Named to match the native binding (`_c.settings_locale_picker_screen`) so this
    wrapper OVERRIDES the plain re-export (globals().update above) and the app's
    `run_screen("settings_locale_picker_screen", ...)` dispatch lands here -- otherwise
    the app calls the raw binding with no endonym_images and non-Latin names render
    blank. The binding wires its endonym image provider only when the dict is passed."""
    cfg = cfg or {}
    base = _resolve(cfg.get("font_dir"))
    endonym_images = {}
    if _ensure_base(base):
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
    return _c.settings_locale_picker_screen(cfg, endonym_images)


# Mount the card at import so pack reads (and the app's gettext .mo open() under the
# same mount) work regardless of call order.
_ensure_sd()
