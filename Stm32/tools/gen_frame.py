#!/usr/bin/env python3
"""生成 FaceRecognition 测试帧 → VOFA+ HEX 发送"""
import struct

CRC8_POLY = 0x07

def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ CRC8_POLY) if (crc & 0x80) else (crc << 1)
    return crc & 0xFF

def cobs_encode(data):
    out = bytearray()
    code_idx = 0
    out.append(1)
    for b in data:
        if b == 0:
            out[code_idx] = out[code_idx]
            code_idx = len(out)
            out.append(1)
        else:
            out.append(b)
            out[code_idx] += 1
            if out[code_idx] == 0xFF:
                code_idx = len(out)
                out.append(1)
    return bytes(out)

def make_frame(cmd, payload=b""):
    raw = struct.pack(">BBH", cmd, 0x01, len(payload)) + payload
    encoded = cobs_encode(raw)
    crc = crc8(raw)
    return encoded + bytes([crc, 0x00])

def hex_str(frame):
    return " ".join(f"{b:02X}" for b in frame)

if __name__ == "__main__":
    frames = {
        "识别成功(Test)": make_frame(0x10, b"Test"),
        "识别成功(张三)": make_frame(0x10, "张三".encode("utf-8")),
        "未注册人脸":     make_frame(0x11),
        "无人脸":         make_frame(0x12),
        "多人脸":         make_frame(0x13),
        "心跳":           make_frame(0x1F),
    }
    for name, frm in frames.items():
        print(f"/* {name} */")
        print(hex_str(frm))
        print()
