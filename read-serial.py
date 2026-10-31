# Reads the controller's USB serial output for a while after flashing and saves it to boot.log.
import sys, time, serial, serial.tools.list_ports
port = sys.argv[1] if len(sys.argv) > 1 else None
if not port:
    cands = [p.device for p in serial.tools.list_ports.comports() if (p.vid in (0x10C4, 0x1A86, 0x0403, 0x303A))]
    port = cands[0] if cands else None
out = open("boot.log", "w", encoding="utf-8", errors="replace")
if not port:
    out.write("No USB serial port found\n"); sys.exit(1)
out.write(f"Port: {port}\n"); out.flush()
s = serial.Serial(port, 115200, timeout=0.5)
# Pulse EN via RTS to reboot so the full boot log is captured
s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
end = time.time() + 45
while time.time() < end:
    d = s.read(4096)
    if d:
        out.write(d.decode("utf-8", "replace")); out.flush()
s.close(); out.write("\n=== END ===\n")
