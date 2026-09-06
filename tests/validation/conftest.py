import os
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]

INTEROP_SERVER = os.environ.get(
    "SILA2_INTEROP_SERVER",
    str(_REPO_ROOT / "build" / "gcc" / "tests" / "interop" / "sila2_interop_server"),
)


def _find_free_port():
    with socket.socket() as s:
        s.bind(("", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="session")
def sila_server(tmp_path_factory):
    remote_host = os.environ.get("SILA2_HOST")
    if remote_host:
        remote_port = int(os.environ.get("SILA2_PORT", "50052"))
        root_certs_path = os.environ.get("SILA2_ROOT_CERTS")
        root_certs = Path(root_certs_path).read_bytes() if root_certs_path else None
        yield {"host": remote_host, "port": remote_port, "root_certs": root_certs}
        return

    server_path = Path(INTEROP_SERVER)
    if not server_path.is_file():
        pytest.skip(f"Interop server not found: {INTEROP_SERVER}")

    port = _find_free_port()
    cert_file = tmp_path_factory.mktemp("certs") / "server.pem"

    proc = subprocess.Popen(
        [str(server_path), "--port", str(port), "--cert-out", str(cert_file)]
    )
    for _ in range(50):
        try:
            with socket.create_connection(("localhost", port), timeout=0.1):
                break
        except OSError:
            time.sleep(0.1)
    else:
        proc.kill()
        pytest.fail("Interop server did not start within 5 s")

    root_certs = cert_file.read_bytes() if cert_file.exists() else None

    yield {"host": "localhost", "port": port, "root_certs": root_certs}

    proc.terminate()
    proc.wait(timeout=5)
