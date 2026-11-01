# SPDX-License-Identifier: GPL-3.0-or-later
"""Local HTTP fixture recording requests made by WebView permission tests."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import hashlib
from pathlib import Path
import sys

port_file, requests_file = map(Path, sys.argv[1:])


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        with requests_file.open("a", encoding="utf-8") as output:
            output.write(self.path + "\n")
        if self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
            ).digest()).decode("ascii")
            self.send_response(101, "Switching Protocols")
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, format, *args):
        pass


with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
    port_file.write_text(str(server.server_port), encoding="ascii")
    server.serve_forever()
