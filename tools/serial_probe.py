import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM9'
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
commands = sys.argv[3:] or ['heap']

last_err = None
for _ in range(150):
    try:
        p = serial.Serial(port, 115200, timeout=0.2)
        break
    except Exception as exc:  # port is sometimes grabbed by the IDE monitor
        last_err = exc
        time.sleep(0.2)
else:
    raise last_err
p.reset_input_buffer()
end = time.time() + seconds
buf = bytearray()
while time.time() < end:
    data = p.read(4096)
    if data:
        buf.extend(data)
for cmd in commands:
    if cmd.startswith('sleep:'):
        end = time.time() + float(cmd.split(':', 1)[1])
        while time.time() < end:
            data = p.read(4096)
            if data:
                buf.extend(data)
        continue
    p.write((cmd + '\r\n').encode('utf-8'))
    time.sleep(0.6)
    end = time.time() + 1.5
    while time.time() < end:
        data = p.read(4096)
        if data:
            buf.extend(data)
p.close()
sys.stdout.buffer.write(bytes(buf))
