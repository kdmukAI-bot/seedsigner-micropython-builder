#!/usr/bin/env python3
"""Host-side byte-identical verifier for PSBTParser output.

Computes the SAME canonical digest as the device driver (psbt_bench_dev._canon)
over each fixture, so:
  * it cross-checks the on-device harness (host digest must equal the device's), and
  * it is the golden reference for Phase-2 (2a/2b/2c must keep every digest identical).

Run with the seedsigner venv:
  /home/kdmukai/dev/seedsigner/.venv/bin/python psbt_parse_verify.py
"""
import hashlib
import os
import sys

SS_SRC = "/home/kdmukai/dev/seedsigner/src"
sys.path.insert(0, SS_SRC)

from seedsigner.models.psbt_parser import PSBTParser
from seedsigner.models.seed import Seed
from seedsigner.models.settings_definition import SettingsConstants
from embit.psbt import PSBT

ALICE = ("fence runway woman funny loan vote anxiety alpha neither filter mechanic "
         "silent burger sphere athlete visit intact under film frequent manage few "
         "wife round")
FIX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
TAGS = ("3in", "10in", "100in")


def canon(pp):
    """Byte-identical MUST match psbt_bench_dev._canon exactly."""
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
    return hashlib.sha256(s.encode()).hexdigest()[:16]


def main():
    seed = Seed(mnemonic=ALICE.split())
    for tag in TAGS:
        path = os.path.join(FIX, "regtest_2of3_p2wsh_%s_xpubs.txt" % tag)
        if not os.path.exists(path):
            print("%-6s MISSING %s" % (tag, path))
            continue
        b64 = open(path).read().strip()
        pp = PSBTParser(p=PSBT.from_base64(b64), seed=seed,
                        network=SettingsConstants.REGTEST)
        t = getattr(pp, "_timing", {})
        print("%-6s digest=%s  hmac=%s ripemd=%s  cosigners=%s  "
              "spend=%d change=%d fee=%d"
              % (tag, canon(pp), t.get("hmac"), t.get("ripemd"),
                 "cosigners" in pp.policy, pp.spend_amount, pp.change_amount,
                 pp.fee_amount))


if __name__ == "__main__":
    main()
