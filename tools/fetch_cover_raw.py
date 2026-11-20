import base64
import re
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM9'
out = sys.argv[2] if len(sys.argv) > 2 else 'build/cover_raw.bin'

last_err = None
for _ in range(150):
    try:
        p = serial.Serial(port, 115200, timeout=0.3)
        break
    except Exception as exc:
        last_err = exc
        time.sleep(0.2)
else:
    raise last_err

p.reset_input_buffer()
time.sleep(0.3)
p.write(b'uifiles\r\n')
time.sleep(0.8)
p.write(b'uiplayname ReDreaming\r\n')
time.sleep(5.0)
p.write(b'coverraw\r\n')

buf = bytearray()
deadline = time.time() + 90
while time.time() < deadline:
    data = p.read(8192)
    if data:
        buf.extend(data)
        if b'-----END-B64-----' in buf:
            break
p.close()

text = bytes(buf).decode('utf-8', errors='ignore')
m = re.search(r'-----BEGIN-B64-----(.*?)-----END-B64-----', text, re.S)
if not m:
    sys.stderr.write('no base64 markers; captured %d bytes\n' % len(buf))
    sys.exit(2)
payload = ''.join(ch for ch in m.group(1) if ch in
                  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=')
raw = base64.b64decode(payload)
with open(out, 'wb') as f:
    f.write(raw)
print('wrote %s (%d bytes)' % (out, len(raw)))
