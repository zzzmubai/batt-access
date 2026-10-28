"""Verify seed contents without creating another private key or starting a VM."""
import io
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / ".local" / "v3-tools"))
import pycdlib

target = root / ".local" / "v3-cloud"
image = pycdlib.PyCdlib()
image.open(str(target / "seed.iso"))
try:
    data = io.BytesIO()
    image.get_file_from_iso_fp(data, rr_path="/user-data")
    user = data.getvalue().decode("utf-8")
    public = (target / "id_ed25519.pub").read_text(encoding="ascii").strip()
    assert public in user
    assert "PRIVATE KEY" not in user
    assert "ssh_pwauth: false" in user and "disable_root: true" in user
    assert "package_upgrade: false" in user
    assert "lock_passwd: true" in user
    print("M0_SEED_PASS public_key_only password_login_disabled no_automatic_upgrade")
finally:
    image.close()
