"""Device-side PSBT parse-timing driver (runs on MicroPython / ESP32-P4).

Pushed to /lib by tools/device_scan/psbt_ab_timing.py, then invoked over the raw
REPL as `import psbt_bench_dev as B; B.run('/psbt_fix/<name>.txt', n)`.

Loads a fixture from the internal-flash VFS, builds Seed(alice) + PSBT, runs
`PSBTParser.parse()` n times, reads the instrumented `_timing` (the `# ── TEMP ──`
block in psbt_parser.py), and prints one machine-readable RESULT line the host
scrapes. `digest` = a stable hash of the canonicalized parse result, so
"byte-identical across phases" is an automatable on-device assertion.

Seed = fixture `alice` (fp 814d5ff8); network = REGTEST (regtest tpub wallet).
"""
import sys
import gc
import time
import hashlib
import binascii

ALICE = ("fence runway woman funny loan vote anxiety alpha neither filter mechanic "
         "silent burger sphere athlete visit intact under film frequent manage few "
         "wife round")


def _fresh_import():
    # Drop the app + embit so an edited/redeployed module is re-imported.
    for m in list(sys.modules):
        if m.split('.')[0] in ('seedsigner', 'embit'):
            del sys.modules[m]


def _canon(pp):
    """Canonical hash of the parse result: policy + amounts + destinations +
    change_data. Two parses that agree here produced byte-identical output."""
    d = []
    d.append("type=%s" % pp.policy.get("type"))
    d.append("mn=%s/%s" % (pp.policy.get("m"), pp.policy.get("n")))
    d.append("cos=%s" % ",".join(pp.policy.get("cosigners", [])))
    d.append("amt=%d/%d/%d/%d" % (pp.spend_amount, pp.change_amount,
                                  pp.fee_amount, pp.input_amount))
    d.append("dst=%s" % ";".join("%s:%d" % (a, v) for a, v in
                                 zip(pp.destination_addresses, pp.destination_amounts)))
    for c in pp.change_data:
        d.append("chg=%s:%s:%d:%s:%s" % (
            c["output_index"], c["address"], c["amount"],
            ",".join(c["fingerprint"]), ",".join(c["derivation_path"])))
    s = "|".join(d)
    return binascii.hexlify(hashlib.sha256(s.encode()).digest()).decode()[:16]


def run(fixture, n=5, network=None):
    _fresh_import()
    from seedsigner.models.psbt_parser import PSBTParser
    from seedsigner.models.seed import Seed
    from seedsigner.models.settings_definition import SettingsConstants
    from embit.psbt import PSBT

    net = network or SettingsConstants.REGTEST
    seed = Seed(mnemonic=ALICE.split())
    b64 = open(fixture).read().strip()

    times, tim, digest = [], None, None
    for _ in range(n):
        gc.collect()
        p = PSBT.from_base64(b64)          # byte-parse OUTSIDE the timed region
        t0 = time.ticks_ms()
        pp = PSBTParser(p=p, seed=seed, network=net)
        times.append(time.ticks_diff(time.ticks_ms(), t0))
        tim = pp._timing
        digest = _canon(pp)

    times.sort()
    med = times[len(times) // 2]
    name = fixture.split('/')[-1]
    print("RESULT fixture=%s n=%d total_ms_med=%d min=%d max=%d "
          "set_root=%d fill_fp=%d parse_inputs=%d parse_outputs=%d "
          "hmac=%d ripemd=%d digest=%s free=%d"
          % (name, n, med, times[0], times[-1],
             tim['set_root'], tim['fill_fp'], tim['parse_inputs'], tim['parse_outputs'],
             tim['hmac'], tim['ripemd'], digest, gc.mem_free()))
    return med
