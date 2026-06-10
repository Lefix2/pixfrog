---
name: flash
description: Flash the board over /dev/ttyACM0 (docker, root) and capture the boot log
---

Flash (root container — host user isn't in dialout):
```bash
docker run --rm --device /dev/ttyACM0 -v "$PWD":/project -w /project espressif/idf:v5.5 idf.py -p /dev/ttyACM0 flash
```

Afterwards reclaim build/ ownership:
```bash
docker run --rm -v "$PWD":/project espressif/idf:v5.5 chown -R "$(id -u):$(id -g)" /project/build
```

Boot log (idf.py monitor can't reset in non-interactive docker; pulse RTS=EN then read):
```python
import serial, time
s = serial.Serial(); s.port="/dev/ttyACM0"; s.baudrate=115200; s.timeout=1
s.dtr=False; s.rts=False; s.open()
s.rts=True; time.sleep(0.2); s.rts=False
end=time.time()+6; out=b""
while time.time()<end: out+=s.read(4096)
print(out.decode(errors="replace"))
```
(run it in the image: `docker run --rm --device /dev/ttyACM0 -v /tmp/x.py:/x.py espressif/idf:v5.5 python3 /x.py`)

WSL2: if the port vanished after replug, re-attach from Windows: `usbipd attach --wsl --busid <BUSID>`.
