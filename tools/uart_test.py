"""UART test: send hello continuously at 1Hz"""
import serial
import time

PORT = "/dev/serial0"
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=1)
print(f"Sending on {PORT} @{BAUD}bps...")
count = 0
try:
    while True:
        msg = f"hello {count}\n"
        ser.write(msg.encode())
        print(f"Sent: {msg.strip()}", flush=True)
        count += 1
        time.sleep(1.0)
except KeyboardInterrupt:
    print("\nStopped.")
finally:
    ser.close()
