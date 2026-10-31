# Records the controller's USB serial output for 90 s WITHOUT resetting it, to serial.log.
import time, serial, serial.tools.list_ports
cands = [p.device for p in serial.tools.list_ports.comports() if p.vid in (0x10C4, 0x1A86, 0x0403, 0x303A)]
out = open("serial.log", "w", encoding="utf-8", errors="replace")
if not cands:
    out.write("No USB serial port found\n"); raise SystemExit(1)
s = serial.Serial()
s.port = cands[0]; s.baudrate = 115200; s.timeout = 0.5
s.dtr = False; s.rts = False  # keep EN/IO0 released so opening the port doesn't reset the board
s.open()
out.write(f"Port: {cands[0]}  start {time.strftime('%H:%M:%S')}\n"); out.flush()
end = time.time() + 90
while time.time() < end:
    d = s.read(4096)
    if d:
        out.write(d.decode("utf-8", "replace")); out.flush()
s.close(); out.write(f"\n=== END {time.strftime('%H:%M:%S')} ===\n")
