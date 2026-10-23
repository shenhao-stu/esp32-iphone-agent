#!/usr/bin/env python3
"""TLS video transport acceptance with synthetic bytes; Python 3.13+ and pyserial.

Uses USB serial for diagnostics/key provisioning, not for video or phone input.
No screen capture, credentials, or raw command lines are written to the report.
"""
import argparse
import json
import secrets
import socket
import ssl
import time
from pathlib import Path

import serial


class Probe:
    def __init__(self, port):
        self.serial = serial.Serial(port, 115200, timeout=0.1)
        self.request = 970000
        self.diagnostics = []

    def send(self, command):
        self.request += 1
        self.serial.write(f"{self.request} {command}\n".encode())
        return str(self.request)

    def reply(self, request, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.serial.readline().decode(errors="replace").strip()
            if line.startswith("video Wi-Fi:"):
                self.diagnostics.append(line)
            if line.startswith("video: "):
                line = line.removeprefix("video: ")
            if line.startswith(request + " "):
                if not line.startswith(request + " ok"):
                    raise RuntimeError("Board rejected diagnostic command")
                return line[len(request) + 3:].strip()
        raise TimeoutError("Board diagnostic reply timed out")

    def command(self, command):
        return self.reply(self.send(command))

    def connect(self, host, wrong_key=False):
        key = secrets.token_bytes(32)
        port = int(self.command("vnet " + key.hex()))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE  # PSK, not certificate authentication
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
        context.set_ciphers("PSK-AES128-GCM-SHA256")
        secret = secrets.token_bytes(32) if wrong_key else key
        context.set_psk_client_callback(lambda _: ("gagent-video-v1", secret))
        time.sleep(0.15)
        connection = context.wrap_socket(socket.create_connection((host, port), timeout=8), server_hostname=None)
        connection.settimeout(8)
        return connection


def read_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise EOFError("Video socket closed")
        data.extend(part)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--kb", type=int, default=1024)
    args = parser.parse_args()
    if not 1 <= args.kb <= 4096 or not 1 <= args.rounds <= 30:
        parser.error("kb must be 1–4096 and rounds 1–30")
    probe = Probe(args.port)
    report = {"rounds": [], "transport": "TLS 1.2 PSK / Wi-Fi", "synthetic_only": True}
    try:
        probe.command("stream off")
        baseline = json.loads(probe.command("status"))
        report["before"] = baseline
        try:
            with probe.connect(baseline["ip"], wrong_key=True):
                raise RuntimeError("Incorrect key was accepted")
        except ssl.SSLError:
            report["wrong_key_rejected"] = True
        with probe.connect(baseline["ip"]) as connection:
            report["cipher"] = connection.cipher()
            report["authenticated"] = json.loads(probe.command("status"))
            last_seq = None
            for index in range(args.rounds):
                start = time.monotonic()
                request = probe.send(f"wbench {args.kb}")
                count = corrupt = gaps = records = 0
                while count < args.kb * 1024:
                    length = int.from_bytes(read_exact(connection, 2), "big")
                    if not 3 <= length <= 16003:
                        raise RuntimeError("Invalid video record")
                    record = read_exact(connection, length)
                    seq = int.from_bytes(record[:2], "little")
                    if last_seq is not None and seq != (last_seq + 1) % 65536:
                        gaps += (seq - last_seq - 1) % 65536
                    last_seq = seq
                    if record[2] != 16:
                        raise RuntimeError("Unexpected screen data; refusing to capture")
                    count += length - 3
                    corrupt += sum(byte != 0xA5 for byte in record[3:])
                    records += 1
                elapsed = time.monotonic() - start
                board_ms = int(probe.reply(request))
                row = {"round": index + 1, "bytes": count, "seconds": elapsed, "board_ms": board_ms,
                       "payload_kbps": count * 8 / 1000 / elapsed, "corrupt": corrupt,
                       "sequence_gaps": gaps, "records": records,
                       "status": json.loads(probe.command("status"))}
                report["rounds"].append(row)
                print(json.dumps({key: value for key, value in row.items() if key != "status"}), flush=True)
                if corrupt or gaps or count != args.kb * 1024:
                    raise RuntimeError("Synthetic payload verification failed")
        report["completed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        try:
            probe.command("vnet off")
        finally:
            probe.serial.close()
            report["diagnostics"] = probe.diagnostics
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
