from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
data = path.read_bytes()
# ASCII printable runs
for m in re.finditer(rb"[\x20-\x7e]{5,}", data):
    s = m.group().decode("ascii", "ignore")
    low = s.lower()
    if any(k in low for k in ("cs431", "codec", "i2c", "es831", "dac", "audio")):
        try:
            off = m.start()
        except Exception:
            off = -1
        print(f"{off:08x}: {s}")
