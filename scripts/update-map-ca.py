"""Refresh public TLS trust anchors from certifi's Mozilla CA bundle."""
from pathlib import Path
import re
import socket
import ssl
import tempfile
import certifi

root = Path(__file__).resolve().parents[1]
certificates = re.findall(
    r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
    Path(certifi.where()).read_text(), re.S,
)
selected = []
for certificate in certificates:
    with tempfile.NamedTemporaryFile(mode="w", suffix=".pem", delete=False) as file:
        file.write(certificate)
        path = Path(file.name)
    try:
        # CPython's certificate parser is used only by this maintenance script.
        subject = str(ssl._ssl._test_decode_cert(str(path))["subject"])
        if ("GlobalSign" in subject and "Root" in subject) or "ISRG Root" in subject:
            selected.append(certificate)
    finally:
        path.unlink()
assert selected, "No trusted map CA roots found"
pem = "\n".join(selected) + "\n"
context = ssl.create_default_context(cadata=pem)
with socket.create_connection(("tile.openstreetmap.org", 443), timeout=10) as tcp:
    with context.wrap_socket(tcp, server_hostname="tile.openstreetmap.org"):
        print("OSM TLS chain verified with embedded trust anchors")
(root / "include" / "map_root_ca.h").write_text(
    '#pragma once\n// Public GlobalSign/ISRG roots from the Mozilla CA bundle.\n'
    'const char map_root_ca[] = R"CERT(' + pem + ')CERT";\n',
    encoding="utf-8",
)
