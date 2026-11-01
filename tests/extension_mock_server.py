"""Local-only HTTP contract fixtures for the native extension integration tests."""
import hashlib
import http.server
import json
import pathlib
import sys
import time
import urllib.parse

PACKAGE = b"verified package"
SUBTITLE = b"1\n00:00:00,000 --> 00:00:01,000\nMock subtitle\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, value, status=200, content_type="application/json"):
        data = value if isinstance(value, bytes) else json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    @property
    def base(self):
        return "http://127.0.0.1:" + str(self.server.server_port)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/json":
            self.reply({"answer": 42})
        elif path == "/redirect-blocked":
            self.send_response(302)
            self.send_header("Location", "http://localhost:" + str(self.server.server_port) + "/json")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif path == "/subtitles":
            if self.headers.get("Api-Key") != "test-key":
                return self.reply({"error": "missing API key"}, 403)
            self.reply({"data": [{"id": "subtitle-1", "attributes": {"release": "Test", "language": "en", "files": [{"file_id": 7, "file_name": "test.srt"}]}}]})
        elif path == "/sub/search":
            if self.headers.get("Authorization") != "Bearer test-token":
                return self.reply({"status": 1}, 403)
            self.reply({"status": 0, "sub": {"subs": [{"id": 9, "native_name": "Test subtitle", "lang": {"desc": "English"}}]}})
        elif path == "/sub/detail":
            self.reply({"status": 0, "sub": {"subs": [{"id": 9, "filelist": [{"url": self.base + "/subtitle.srt"}]}]}})
        elif path == "/catalog":
            self.reply([{"id": "org.velin.fixture", "name": "Fixture", "url": "https://example.invalid/plugin"}])
        elif path == "/subtitle.srt":
            self.reply(SUBTITLE, content_type="text/plain")
        elif path == "/package":
            self.reply(PACKAGE, content_type="application/octet-stream")
        elif path == "/update":
            self.reply({"version": "9.0.0", "url": self.base + "/package", "sha256": hashlib.sha256(PACKAGE).hexdigest()})
        elif path == "/slow":
            time.sleep(5)
            self.reply({"slow": True})
        else:
            self.reply({"error": "unknown fixture"}, 404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/login":
            credentials = json.loads(body)
            if credentials.get("username") != "test" or credentials.get("password") != "test":
                return self.reply({"error": "invalid fixture credentials"}, 401)
            self.reply({"token": "test-token", "base_url": "127.0.0.1"})
        elif self.path == "/download":
            self.reply({"link": self.base + "/subtitle.srt", "file_name": "test.srt"})
        elif self.path == "/rpc":
            if b"<methodName>test</methodName>" not in body:
                return self.reply({"error": "invalid XML-RPC request"}, 400)
            self.reply(b'<?xml version="1.0"?><methodResponse><params><param><value><struct><member><name>answer</name><value><int>42</int></value></member></struct></value></param></params></methodResponse>', content_type="text/xml")
        else:
            self.reply({"error": "unknown fixture"}, 404)


if __name__ == "__main__":
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        pathlib.Path(sys.argv[1]).write_text(str(server.server_port), encoding="ascii")
        server.serve_forever()
