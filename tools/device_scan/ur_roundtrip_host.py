"""Host (CPython) baseline for the UR-fountain solve diagnostic.

Encodes a deterministic message into UR fountain parts, then feeds ONLY the
mixed parts (seq_num > fragment_count) into the decoder — forcing the
Xoshiro256-seeded fragment selection + XOR peeling (the reduction path most
likely to diverge on MicroPython, and hidden behind URDecoder.receive_part's
silent `except Exception: return False`).

Records the exact mixed-part sequence that completes on CPython into
`ur_parts.json` next to this file, so the device replay (ur_roundtrip_dev.py)
can feed the identical input and isolate a MicroPython solve bug from camera
optics.

Run from a checkout of seedsigner at ../../seedsigner relative to this repo, or
edit SS_SRC below.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # -> tools/
import _devenv  # noqa: E402
SS_SRC = _devenv.SS_SRC_ROOT   # $SS_APP_DIR/src (env-driven; see .env.example)
sys.path.insert(0, SS_SRC)

from seedsigner.helpers.ur2.ur import UR
from seedsigner.helpers.ur2.ur_encoder import UREncoder
from seedsigner.helpers.ur2.ur_decoder import URDecoder

msg = bytes((i * 37 + 11) & 0xFF for i in range(1500))
ur = UR("bytes", msg)

seq_len = int(UREncoder(ur, max_fragment_len=100).next_part().split("/")[1].split("-")[1])
print("fragment count (seq_len):", seq_len)

enc = UREncoder(ur, max_fragment_len=100)
mixed = []
n = 0
while len(mixed) < 200 and n < 2000:
    p = enc.next_part()
    n += 1
    if int(p.split("/")[1].split("-")[0]) > seq_len:  # keep only mixed parts
        mixed.append(p)
print("collected", len(mixed), "mixed parts (seq_num >", seq_len, ")")

dec = URDecoder()
done_at = None
for i, p in enumerate(mixed, 1):
    dec.receive_part(p)
    if dec.is_complete():
        done_at = i
        break

print("MIXED-ONLY is_complete:", dec.is_complete(), " at mixed-part #", done_at,
      " processed:", dec.fountain_decoder.processed_parts_count)
if dec.is_complete():
    print("round-trips exactly:", bytes(dec.result_message().cbor) == msg)

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ur_parts.json")
open(out, "w").write(json.dumps({
    "mixed_parts": mixed[:(done_at + 10) if done_at else len(mixed)],
    "host_done_at": done_at,
    "seq_len": seq_len,
    "msg_len": len(msg),
}))
print("wrote", out)
