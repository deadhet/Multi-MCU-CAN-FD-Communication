"""The dashboard's HTTP server (noccarc_dashboard/server.py): what it serves, what it refuses,
and the live stream. Runs the real server on a free port of 127.0.0.1."""

import http.client
import json
import threading

import pytest

from noccarc_dashboard import server


@pytest.fixture
def running():
    stop = threading.Event()
    httpd = server.serve(lambda: {"link": "CONNECTED", "n": 1}, 0, stop)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    yield httpd.server_address[1]
    stop.set()
    httpd.shutdown()
    httpd.server_close()


def get(port, path, host=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    conn.request("GET", path, headers={"Host": host or f"127.0.0.1:{port}"})
    r = conn.getresponse()
    body = r.read()
    conn.close()
    return r, body


def test_serves_the_three_page_files_with_safety_headers(running):
    for path, ctype in (("/", "text/html"), ("/style.css", "text/css"), ("/app.js", "text/javascript")):
        r, body = get(running, path)
        assert r.status == 200 and r.getheader("Content-Type").startswith(ctype) and body
        assert "default-src 'self'" in r.getheader("Content-Security-Policy")
        assert r.getheader("X-Content-Type-Options") == "nosniff"
    assert b'src="initial.js"' in get(running, "/")[1]


def test_initial_model(running):
    r, body = get(running, "/initial.js")
    assert r.status == 200
    assert body.decode() == 'window.INITIAL = {"link":"CONNECTED","n":1};\n'


@pytest.mark.parametrize("path", ["/mock.js", "/../model.py", "/web/app.js", "/server.py", "/%2e%2e/link.py"])
def test_nothing_else_from_the_disk(running, path):
    assert get(running, path)[0].status == 404


def test_only_for_this_pc(running):
    assert get(running, "/", host="attacker.example")[0].status == 403  # DNS rebinding
    assert get(running, "/", host=f"localhost:{running}")[0].status == 200


def test_stream_sends_the_model(running):
    conn = http.client.HTTPConnection("127.0.0.1", running, timeout=3)
    conn.request("GET", "/stream", headers={"Host": f"127.0.0.1:{running}"})
    r = conn.getresponse()
    assert r.status == 200 and r.getheader("Content-Type").startswith("text/event-stream")
    first = r.fp.readline().decode()
    assert first.startswith("data: ") and json.loads(first[6:]) == {"link": "CONNECTED", "n": 1}
    assert r.fp.readline() == b"\n"
    conn.close()


def test_listens_on_this_pc_only(running):
    stop = threading.Event()
    httpd = server.serve(dict, 0, stop)
    assert httpd.server_address[0] == "127.0.0.1"
    httpd.server_close()
