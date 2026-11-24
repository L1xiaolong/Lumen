#!/usr/bin/env python3
"""Sign a Lumen DMG and create its version-specific GitHub release appcast."""

import argparse
import base64
import binascii
import os
from pathlib import Path
import subprocess
from urllib.parse import quote
from xml.etree import ElementTree as ET


def sign(signer, key, *args):
    environment = os.environ.copy()
    environment.pop("MVPVIEW_SPARKLE_PRIVATE_KEY", None)
    result = subprocess.run(
        [signer, "--ed-key-file", "-", *args],
        input=key + "\n",
        capture_output=True,
        text=True,
        env=environment,
    )
    if result.returncode != 0:
        raise SystemExit("Sparkle could not sign the release. Check the signing key and archive.")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("signer")
    parser.add_argument("repository")
    parser.add_argument("tag")
    parser.add_argument("version")
    parser.add_argument("archive", type=Path)
    parser.add_argument("appcast", type=Path)
    args = parser.parse_args()

    private_key = os.environ.pop("MVPVIEW_SPARKLE_PRIVATE_KEY", "").strip()
    try:
        private_bytes = base64.b64decode(private_key, validate=True)
        public_bytes = base64.b64decode(os.environ["MVPVIEW_SPARKLE_PUBLIC_KEY"].strip(), validate=True)
    except (KeyError, ValueError, binascii.Error):
        raise SystemExit("Sparkle signing keys must be configured as base64 values.") from None
    if len(private_bytes) == 32:
        # Current Sparkle exports the seed. Older exports append the public key
        # to the 64-byte expanded private key.
        result = subprocess.run(
            ["/usr/bin/swift", "-e", """
import CryptoKit
import Foundation
let seed = Data(base64Encoded: readLine()!)!
let key = try Curve25519.Signing.PrivateKey(rawRepresentation: seed)
print(key.publicKey.rawRepresentation.base64EncodedString())
"""],
            input=private_key + "\n",
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise SystemExit("Could not derive the Sparkle public key from the private seed.")
        matching_key = base64.b64decode(result.stdout.strip(), validate=True) == public_bytes
    else:
        matching_key = len(private_bytes) == 96 and private_bytes[-32:] == public_bytes
    if len(public_bytes) != 32 or not matching_key:
        raise SystemExit("The Sparkle public key does not match the configured private key.")

    sparkle = "http://www.andymatuschak.org/xml-namespaces/sparkle"
    ET.register_namespace("sparkle", sparkle)
    rss = ET.Element("rss", version="2.0")
    channel = ET.SubElement(rss, "channel")
    repository_url = f"https://github.com/{args.repository}"
    release_tag = quote(args.tag, safe="")
    ET.SubElement(channel, "title").text = "Lumen Updates"
    ET.SubElement(channel, "link").text = f"{repository_url}/releases"
    ET.SubElement(channel, "description").text = "Signed Lumen updates for macOS"
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = f"Lumen {args.version}"
    ET.SubElement(item, f"{{{sparkle}}}version").text = args.version
    ET.SubElement(item, f"{{{sparkle}}}shortVersionString").text = args.version
    signature = sign(args.signer, private_key, "-p", str(args.archive))
    ET.SubElement(item, "enclosure", {
        "url": f"{repository_url}/releases/download/{release_tag}/{quote(args.archive.name)}",
        "length": str(args.archive.stat().st_size),
        "type": "application/octet-stream",
        f"{{{sparkle}}}edSignature": signature,
    })
    ET.indent(rss)
    ET.ElementTree(rss).write(args.appcast, encoding="utf-8", xml_declaration=True)
    sign(args.signer, private_key, str(args.appcast))


if __name__ == "__main__":
    main()
