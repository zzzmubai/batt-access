#!/usr/bin/env python3
"""Create a NoCloud seed and a local SSH key for a NEW, isolated test VM.

Requires pycdlib (for ISO generation) and cryptography. No password is generated,
printed, or embedded. Outputs remain in ignored .local/v3-cloud/.
"""
import argparse
import io
import json
import os
import re
import subprocess
from pathlib import Path


def prepare(name="v3-cloud"):
    import pycdlib
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    root = Path(__file__).resolve().parents[1]
    if not re.fullmatch(r"v3-cloud(?:-[a-z0-9]+)?", name):
        raise ValueError("invalid private seed directory name")
    target = root / ".local" / name
    target.mkdir(mode=0o700)  # Never overwrite an existing machine identity.
    key = Ed25519PrivateKey.generate()
    private = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.OpenSSH,
                               serialization.NoEncryption())
    public = key.public_key().public_bytes(serialization.Encoding.OpenSSH,
                                          serialization.PublicFormat.OpenSSH).decode("ascii")
    descriptor = os.open(target / "id_ed25519", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(private)
    if os.name == "nt":
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(root / "scripts" / "protect-v3-key.ps1"), "-Name", name], check=True)
    (target / "id_ed25519.pub").write_text(public + "\n", encoding="ascii")
    user_data = """#cloud-config
hostname: batt-v3-mysql
manage_etc_hosts: true
ssh_pwauth: false
disable_root: true
users:
  - name: book
    groups: [sudo]
    shell: /bin/bash
    lock_passwd: true
    sudo: ['ALL=(ALL) NOPASSWD:ALL']
    ssh_authorized_keys:
      - PUBLIC_KEY
growpart:
  mode: auto
  devices: ['/']
resize_rootfs: true
package_update: false
package_upgrade: false
""".replace("PUBLIC_KEY", public)
    metadata = "instance-id: batt-v3-m0-20261028\nlocal-hostname: batt-v3-mysql\n"
    image = pycdlib.PyCdlib()
    image.new(interchange_level=3, joliet=3, rock_ridge="1.09", vol_ident="cidata")
    for filename, iso_name, data in (("user-data", "/USERDATA.;1", user_data),
                                     ("meta-data", "/METADATA.;1", metadata)):
        raw = data.encode("utf-8")
        image.add_fp(io.BytesIO(raw), len(raw), iso_path=iso_name,
                     rr_name=filename, joliet_path="/" + filename)
        (target / filename).write_bytes(raw)
    image.write(str(target / "seed.iso"))
    image.close()
    print(json.dumps({"result": "SEED_READY", "directory": str(target),
                      "user": "book", "password_authentication": False}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="v3-cloud")
    prepare(parser.parse_args().name)
