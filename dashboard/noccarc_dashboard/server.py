"""The monitoring dashboard: reads MCU1's UART link and shows it in the browser.

    uv run --frozen python -m noccarc_dashboard.server --port 3456 --lost-ms 1500    # Renode
    uv run --frozen python -m noccarc_dashboard.server --serial /dev/ttyUSB0           # the board

Opens http://127.0.0.1:8765/. Python's standard library only:
  - link.py reads the bytes in a background thread (TCP from Renode, or the serial port);
  - model.py turns Link.view() into the page model;
  - this file serves the page and pushes the model 5 times a second over Server-Sent Events
    (/stream: one long HTTP response, one "data: {json}" message each time; the browser's
    EventSource reconnects by itself). /initial.js gives the model at page load, so the page is
    filled at once, before the stream's first message.

Safety: listens on 127.0.0.1 only, so nothing on the network can reach it; serves exactly the
page's three files, the initial model and the stream, nothing from the disk by name; answers
only when the Host header is this PC (against DNS rebinding); the page may load nothing from
anywhere else (Content-Security-Policy). The page is read-only: nothing can be sent to MCU1.
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable

from . import model
from .link import DEFAULT_LOST_MS, LINK_BAUD, Link, SerialSource, TcpSource

WEB = Path(__file__).parent / "web"
FILES = {  # URL path -> (file in web/, content type). mock.js (layout review) is never served.
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}
STREAM_PERIOD_S = 0.2
HTTP_PORT = 8765
HEADERS = {
    "Content-Security-Policy": "default-src 'self'; object-src 'none'; base-uri 'none'; "
                               "form-action 'none'; frame-ancestors 'none'",
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "Cache-Control": "no-store",
}


def make_handler(page_model: Callable[[], dict], stop: threading.Event, port: int) -> type:
    allowed_hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}

    class Handler(BaseHTTPRequestHandler):
        server_version = "noccarc-dashboard"
        sys_version = ""

        def log_message(self, format: str, *args) -> None:  # noqa: A002 (stdlib signature)
            pass  # quiet: the terminal shows the link log instead

        def _send(self, code: int, ctype: str, body: bytes) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in HEADERS.items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802 (stdlib name)
            if self.headers.get("Host", "") not in allowed_hosts:
                self._send(403, "text/plain; charset=utf-8", b"forbidden\n")
                return
            path = self.path.split("?", 1)[0]
            if path == "/stream":
                self._stream()
            elif path == "/initial.js":
                body = "window.INITIAL = " + json.dumps(page_model(), separators=(",", ":")) + ";\n"
                self._send(200, "text/javascript; charset=utf-8", body.encode())
            elif path in FILES:
                name, ctype = FILES[path]
                self._send(200, ctype, (WEB / name).read_bytes())
            else:
                self._send(404, "text/plain; charset=utf-8", b"not found\n")

        def _stream(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            for k, v in HEADERS.items():
                self.send_header(k, v)
            self.end_headers()
            try:
                while not stop.is_set():
                    msg = "data: " + json.dumps(page_model(), separators=(",", ":")) + "\n\n"
                    self.wfile.write(msg.encode())
                    self.wfile.flush()
                    time.sleep(STREAM_PERIOD_S)
            except (BrokenPipeError, ConnectionResetError):
                pass  # the browser tab was closed or reloaded

    return Handler


def serve(page_model: Callable[[], dict], port: int, stop: threading.Event) -> ThreadingHTTPServer:
    """An HTTP server on 127.0.0.1:port (0 = any free port), not started yet."""
    httpd = ThreadingHTTPServer(("127.0.0.1", port), make_handler(page_model, stop, port))
    if port == 0:  # the real port is known only now
        httpd.RequestHandlerClass = make_handler(page_model, stop, httpd.server_address[1])
    httpd.daemon_threads = True
    return httpd


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="localhost", help="Renode's host (TCP)")
    ap.add_argument("--port", type=int, default=3456, help="Renode's TCP port for MCU1's USART3")
    ap.add_argument("--serial", help="the board's USB-UART adapter, e.g. /dev/ttyUSB0 (instead of TCP)")
    ap.add_argument("--baud", type=int, default=LINK_BAUD)
    ap.add_argument("--lost-ms", type=int, default=DEFAULT_LOST_MS,
                    help=f"LOST after this long without a valid frame (default {DEFAULT_LOST_MS}; Renode: 1500)")
    ap.add_argument("--http-port", type=int, default=HTTP_PORT, help="the page's port on 127.0.0.1")
    ap.add_argument("--no-browser", action="store_true", help="do not open the browser")
    args = ap.parse_args()

    if args.serial:
        source, where = SerialSource(args.serial, args.baud), f"{args.serial}, {args.baud} Bd"
    else:
        source, where = TcpSource(args.host, args.port), f"{args.host}:{args.port} (Renode)"
    link = Link(source, args.lost_ms)
    stop = threading.Event()
    try:
        httpd = serve(lambda: model.build(link.view(), where), args.http_port, stop)
    except OSError as err:
        print(f"dashboard: cannot use 127.0.0.1:{args.http_port} ({err.strerror}). Another dashboard "
              f"running? Stop it, or pick another port with --http-port.", file=sys.stderr)
        return 1
    url = f"http://127.0.0.1:{httpd.server_address[1]}/"

    link.start()
    print(f"dashboard: {url}  (reading MCU1 from {where}; Ctrl-C to stop)", flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        httpd.server_close()
        link.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
