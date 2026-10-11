#!/usr/bin/env python3
"""Switch PenguinScreen2's network settings between our private Outbreak server and obsrv.

  python netmode.py private 100.101.102.103   # play on the private server at that address (Tailscale or LAN IP)
  python netmode.py obsrv                      # play on the public obsrv servers
  python netmode.py off                        # network adapter off (offline play, testing)
  python netmode.py status                     # show what is set now

Close the emulator first: it rewrites its ini on exit. Works on Windows, macOS and Linux.
The ini is found next to a portable install (D:\\Games\\PS2\\PenguinScreen2\\inis\\PCSX2.ini) or in the user
folder; pass --ini PATH to choose. A backup is kept as PCSX2.ini.bak.

private:
- adapter on (Sockets, Auto);
- DNS1 Manual 192.0.2.1, so only the Internal DNS hosts below resolve;
- kddi-mmbb.jp and www01.kddi-mmbb.jp go to the server; DNAS stays on its public replay (153.121.44.150);
- [VR] PrivateCoopFirstPerson = true, so first person runs online there.

obsrv:
- adapter on;
- DNS1 Manual 208.72.237.31 and no host overrides;
- [VR] PrivateCoopFirstPerson = false, so first person stays off and the 3D screen is used.
"""
import argparse
import ipaddress
import os
import re
import shutil
import sys

OBSRV_DNS = "208.72.237.31"
DNAS = ("gate1.jp.dnas.playstation.org", "153.121.44.150")
LOGIN_HOSTS = ("kddi-mmbb.jp", "www01.kddi-mmbb.jp")
CANDIDATES = [
    r"D:\Games\PS2\PenguinScreen2\inis\PCSX2.ini",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "inis", "PCSX2.ini"),
    os.path.expanduser(r"~\Documents\PenguinScreen2\inis\PCSX2.ini"),
    os.path.expanduser("~/Library/Application Support/PenguinScreen2/inis/PCSX2.ini"),
    os.path.expanduser("~/.config/PenguinScreen2/inis/PCSX2.ini"),
]


def find_ini(explicit):
    if explicit:
        return explicit
    for c in CANDIDATES:
        if os.path.exists(c):
            return os.path.normpath(c)
    sys.exit("PCSX2.ini not found; run the emulator once, or pass --ini PATH")


def parse(lines):
    """[(section or None, [lines])] in file order."""
    blocks, cur = [(None, [])], None
    for line in lines:
        m = re.match(r"^\[(.+)\]\s*$", line)
        if m:
            cur = m.group(1)
            blocks.append((cur, [line]))
        else:
            blocks[-1][1].append(line)
    return blocks


def set_keys(blocks, section, values):
    for name, body in blocks:
        if name == section:
            for key, val in values.items():
                for i, line in enumerate(body):
                    if re.match(r"^\s*" + re.escape(key) + r"\s*=", line):
                        body[i] = f"{key} = {val}\n"
                        break
                else:
                    at = len(body)
                    while at > 1 and body[at - 1].strip() == "":
                        at -= 1
                    body.insert(at, f"{key} = {val}\n")
            return
    blocks.append((section, [f"[{section}]\n"] + [f"{k} = {v}\n" for k, v in values.items()] + ["\n"]))


def write_hosts(blocks, entries, desc):
    blocks[:] = [b for b in blocks if not (b[0] or "").startswith("DEV9/Eth/Hosts")]
    if blocks and blocks[-1][1] and blocks[-1][1][-1].strip() != "":
        blocks[-1][1].append("\n")
    body = ["[DEV9/Eth/Hosts]\n", f"Count = {len(entries)}\n", "\n"]
    blocks.append(("DEV9/Eth/Hosts", body))
    for i, (url, addr) in enumerate(entries):
        blocks.append((f"DEV9/Eth/Hosts/Host{i}", [f"[DEV9/Eth/Hosts/Host{i}]\n", f"Url = {url}\n", f"Desc = {desc}\n",
                                                   f"Address = {addr}\n", "Enabled = true\n", "\n"]))


def read_keys(blocks, section):
    out = {}
    for name, body in blocks:
        if name == section:
            for line in body[1:]:
                m = re.match(r"^\s*([^=;#]+?)\s*=\s*(.*?)\s*$", line)
                if m:
                    out[m.group(1)] = m.group(2)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["private", "obsrv", "off", "status"])
    ap.add_argument("address", nargs="?", help="private server IPv4 address (Tailscale 100.x or LAN)")
    ap.add_argument("--ini")
    a = ap.parse_args()
    ini = find_ini(a.ini)
    with open(ini, encoding="utf-8") as fh:
        blocks = parse(fh.readlines())

    if a.mode == "status":
        eth = read_keys(blocks, "DEV9/Eth")
        vr = read_keys(blocks, "VR")
        hosts = [read_keys(blocks, f"DEV9/Eth/Hosts/Host{i}") for i in range(int(read_keys(blocks, "DEV9/Eth/Hosts").get("Count", "0") or 0))]
        login = next((h.get("Address") for h in hosts if h.get("Url") == "www01.kddi-mmbb.jp" and h.get("Enabled") == "true"), None)
        where = "off" if eth.get("EthEnable") != "true" else (f"private server {login}" if login and not login.startswith("208.72.237.") else "obsrv")
        print(f"{ini}\n  network: {where}\n  adapter: {eth.get('EthEnable')} {eth.get('EthApi')} {eth.get('EthDevice')}, "
              f"DNS1 {eth.get('ModeDNS1')} {eth.get('DNS1')}\n  private co-op first person: {vr.get('PrivateCoopFirstPerson', 'false')}")
        return

    if a.mode == "private":
        try:
            addr = ipaddress.ip_address(a.address or "")
            assert addr.version == 4
        except (ValueError, AssertionError):
            sys.exit("private needs the server's IPv4 address, e.g. python netmode.py private 100.101.102.103")
        if str(addr).startswith("208.72.237."):
            sys.exit("that is obsrv's address, not a private server")
        set_keys(blocks, "DEV9/Eth", {"EthEnable": "true", "EthApi": "Sockets", "EthDevice": "Auto",
                                      "ModeDNS1": "Manual", "DNS1": "192.0.2.1"})
        write_hosts(blocks, [(h, str(addr)) for h in LOGIN_HOSTS] + [DNAS], "private")
        set_keys(blocks, "VR", {"PrivateCoopFirstPerson": "true"})
        msg = f"private server {addr}: login hosts -> {addr}, DNAS -> {DNAS[1]}, private co-op first person on"
    elif a.mode == "obsrv":
        set_keys(blocks, "DEV9/Eth", {"EthEnable": "true", "EthApi": "Sockets", "EthDevice": "Auto",
                                      "ModeDNS1": "Manual", "DNS1": OBSRV_DNS})
        write_hosts(blocks, [], "obsrv")
        set_keys(blocks, "VR", {"PrivateCoopFirstPerson": "false"})
        msg = f"obsrv: DNS1 {OBSRV_DNS}, no host overrides, first person off online (3D screen)"
    else:
        set_keys(blocks, "DEV9/Eth", {"EthEnable": "false"})
        msg = "network adapter off"

    shutil.copyfile(ini, ini + ".bak")
    with open(ini, "w", encoding="utf-8") as fh:
        fh.writelines(line for _, body in blocks for line in body)
    print(f"{ini}\n  {msg}\n  Start the emulator now.")


if __name__ == "__main__":
    main()
