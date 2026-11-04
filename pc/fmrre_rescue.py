#!/usr/bin/env python3
"""FMRRE PC rescue - fix a phone that doesn't start, from a computer.

For a phone that bootloops, stops at a "verification failed" / vbmeta error, or can't get into
recovery any more. Plug it in, in whatever mode it is in - Android, recovery, sideload, the
bootloader (fastboot), fastbootd or Samsung Download mode. The tool tells you which phone it is
and whether its bootloader is unlocked, works out what is wrong and fixes it:

  - starts FMRRE from the computer (fastboot boot: nothing is written) and runs its Auto repair,
    which checks the logs, boot images, vbmeta, both slots and /data from the inside
  - goes back to the other A/B slot when this one can't boot (rollback after a bad update)
  - flashes vbmeta with verification disabled (after a GSI or a custom image)
  - cancels a stuck Virtual A/B update
  - flashes the stock images you give it (fastboot, or Heimdall for Samsung - images packed as .lz4
    in a Samsung AP .tar.md5 are unpacked for you) and installs FMRRE in Download mode
  - wipes data, only after you type ERASE

Anything that writes to the phone needs an UNLOCKED bootloader ("OEM unlocked"). On a locked phone
it explains how to unlock it, and which official tool restores the firmware instead.

  fmrre_rescue.py                    a window (or the status, on a computer without tkinter)
  fmrre_rescue.py status [--json]    which phone, which mode, bootloader locked or not
  fmrre_rescue.py diagnose           the status and what is wrong
  fmrre_rescue.py fix [--recovery fmrre-recovery.img] [--ap AP.tar.md5] [--yes]
                                      "fix it for me" (asks before every change)
  fmrre_rescue.py boot-fmrre IMG     start FMRRE from RAM (fastboot boot)
  fmrre_rescue.py autofix            run FMRRE's Auto repair over adb
  fmrre_rescue.py switch-slot | cancel-update | disable-vbmeta [--vbmeta STOCK.img] [--both-slots]
  fmrre_rescue.py flash PARTITION IMAGE
  fmrre_rescue.py samsung-fmrre IMG  Download mode: FMRRE to RECOVERY + vbmeta disabled
  fmrre_rescue.py samsung-stock AP.tar.md5 [--parts boot,vbmeta,...]
  fmrre_rescue.py sideload OTA.zip   install an OTA / ROM with adb sideload - official OTAs work on LOCKED phones
  fmrre_rescue.py factory FACTORY.zip [--wipe]   reinstall the official firmware (Pixel-style factory image),
                                     keeping apps and files; the image must name this phone
  fmrre_rescue.py wipe-data          factory reset (type ERASE to confirm)
  fmrre_rescue.py reboot [system|recovery|bootloader|fastboot|download]
  fmrre_rescue.py logs [DIR]         save the phone's logs for a bug report

Common options: --lang en|ro (default: the computer's language), --serial SERIAL, --dry-run (show the
commands, change nothing), --yes, --unlocked (you checked on the phone that the bootloader is unlocked -
needed when the phone doesn't say, e.g. Samsung Download mode), --workdir DIR.

Needs Android platform-tools (adb, fastboot) and, for Samsung, Heimdall: in PATH, next to this
script (or in a platform-tools folder beside it), or named by ADB= FASTBOOT= HEIMDALL=. Python 3.7+,
standard library only.
"""
import argparse
import json
import os
import queue
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tarfile
import zipfile
import tempfile
import threading
import time

VERSION = "2.0.0"
FMRRE_CLI = ["/system/bin/recovery", "fmrre"]  # FMRRE's command line tool, over adb shell

PLATFORM_TOOLS = "https://developer.android.com/tools/releases/platform-tools"
INSTALL_HINT = {
    "adb": "install Android platform-tools: " + PLATFORM_TOOLS + " (unzipping it next to this script works too)",
    "fastboot": "install Android platform-tools: " + PLATFORM_TOOLS + " (unzipping it next to this script works too)",
    "heimdall": "install Heimdall (Linux: sudo apt install heimdall-flash, macOS: brew install heimdall), or use Odin on Windows",
}

CONNECT_STEPS = """Plug it in with a USB cable that carries data, then put it in one of these modes:
  - bootloader / fastboot (most phones): power off, then hold Volume Down + Power
  - Samsung Download mode: power off, hold Volume Up + Volume Down and plug the cable in, then press Volume Up
  - Android or a recovery with USB debugging on (adb)
A bootlooping phone: hold the key combination while it restarts.
Windows needs the phone's USB driver (Google USB driver for fastboot / adb, Samsung USB driver for Download mode)."""
HOW_TO_CONNECT = "No phone found. " + CONNECT_STEPS

# Pixel codenames: the bootloader only reports the codename.
PIXELS = {
    "sunfish": "Pixel 4a", "bramble": "Pixel 4a (5G)", "redfin": "Pixel 5", "barbet": "Pixel 5a", "oriole": "Pixel 6",
    "raven": "Pixel 6 Pro", "bluejay": "Pixel 6a", "panther": "Pixel 7", "cheetah": "Pixel 7 Pro", "lynx": "Pixel 7a",
    "felix": "Pixel Fold", "tangorpro": "Pixel Tablet", "shiba": "Pixel 8", "husky": "Pixel 8 Pro", "akita": "Pixel 8a",
    "tokay": "Pixel 9", "caiman": "Pixel 9 Pro", "komodo": "Pixel 9 Pro XL", "comet": "Pixel 9 Pro Fold", "tegu": "Pixel 9a",
}

UNLOCK_HELP = {
    "google": "Settings > About phone: tap Build number 7 times. Developer options: turn on OEM unlocking. Then "
              "'adb reboot bootloader' and 'fastboot flashing unlock' (confirm with the volume keys).",
    "samsung": "Developer options: turn on OEM unlocking (if it is missing, this model or region can't be unlocked - e.g. US "
               "models). Power off, hold Volume Up + Volume Down and plug in the cable, then long-press Volume Up to unlock. "
               "This trips the Knox warranty bit for good.",
    "xiaomi": "Developer options: turn on OEM unlocking and add your Mi account under Mi Unlock status, then use Xiaomi's "
              "official Mi Unlock tool on Windows (there is a waiting period; HyperOS phones need approval in the Xiaomi "
              "Community app first).",
    "oneplus": "Developer options: turn on OEM unlocking, then 'fastboot flashing unlock' (older models: 'fastboot oem unlock').",
    "nothing": "Developer options: turn on OEM unlocking, then 'fastboot flashing unlock'.",
    "motorola": "Get an unlock code from Motorola's bootloader unlock website with the output of 'fastboot oem get_unlock_data', "
                "then 'fastboot oem unlock <code>'.",
    "sony": "Get an unlock code from Sony's developer website (it needs the IMEI), then 'fastboot oem unlock 0x<code>'.",
    "huawei": "Huawei stopped giving out unlock codes in 2018: most Huawei phones can't be unlocked.",
    "honor": "Honor doesn't give out unlock codes: most Honor phones can't be unlocked.",
    "oppo": "Only with the official Deep Testing app, on some models and regions.",
    "realme": "Only with the official Deep Testing app, on some models and regions.",
}
UNLOCK_DEFAULT = ("Developer options: turn on OEM unlocking, then 'fastboot flashing unlock' (some makers: 'fastboot oem unlock'). "
                  "Check the maker's website - some need an unlock code.")

OFFICIAL_RESTORE = {
    "google": "Google's Pixel Update and Software Repair tool (pixelrepair.withgoogle.com) reinstalls the official software on a "
              "locked Pixel; the stock recovery's 'Apply update from ADB' with a full OTA from developers.google.com works too.",
    "samsung": "Smart Switch on a computer (Emergency Software Recovery), or Odin with the official firmware (BL, AP, CP, CSC) - "
               "official firmware flashes on a locked phone.",
    "xiaomi": "A Xiaomi service center, or Mi Flash with an unlocked bootloader; the stock recovery can sideload the official package.",
    "motorola": "Motorola's Software Fix (Rescue and Smart Assistant) on a computer.",
}
OFFICIAL_DEFAULT = ("The maker's official repair tool, or the stock recovery: 'Apply update from ADB' and then "
                    "'adb sideload <official full OTA>.zip' reinstalls the system on a locked phone, keeping data where possible.")

MODE_TEXT = {
    "none": "not found",
    "android": "Android is running (adb)",
    "recovery": "recovery (adb)",
    "sideload": "recovery, waiting for 'adb sideload'",
    "rescue": "Android rescue mode (adb)",
    "unauthorized": "connected - but USB debugging isn't allowed for this computer yet",
    "offline": "connected - but adb sees it as offline",
    "fastboot": "bootloader (fastboot)",
    "fastbootd": "fastbootd (userspace fastboot)",
    "download": "Samsung Download mode (Heimdall)",
}

SAMSUNG_STOCK_PARTS = ("boot", "init_boot", "vendor_boot", "recovery", "vbmeta", "dtbo")

# ---------------------------------------------------------------------------------------------------
# Languages: English in the code, Romanian from fmrre_rescue_ro.py (next to this file). Whole messages are
# looked up when they are shown; "{}" in a key matches a changing part, which is itself translated if known.

LANG = "en"
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fmrre_rescue_ro import RO
except ImportError:  # the dictionary wasn't copied along: English only
    RO = {}
finally:
    sys.path.pop(0)
_PATTERNS = []


def detect_language():
    for v in (os.environ.get("FMRRE_LANG"), os.environ.get("LC_ALL"), os.environ.get("LC_MESSAGES"), os.environ.get("LANG")):
        if v:
            return "ro" if v.lower().startswith("ro") else "en"
    try:
        import locale
        loc = (locale.getlocale()[0] or "").lower()
    except (ValueError, ImportError):
        loc = ""
    return "ro" if loc.startswith(("ro", "romanian")) else "en"


def set_language(code):
    global LANG, _PATTERNS
    LANG = code if code == "en" or (code == "ro" and RO) else "en"
    _PATTERNS = []
    for k, v in RO.items():
        if "{}" in k or "{w}" in k:
            rx = re.escape(k).replace(re.escape("{w}"), r"(\S+)").replace(re.escape("{}"), r"(.+?)")
            _PATTERNS.append((len(k.replace("{}", "").replace("{w}", "")), re.compile("^" + rx + "$", re.S), v))
    _PATTERNS.sort(key=lambda p: -p[0])  # the most specific pattern wins


def T(s, depth=0):
    """The text in the current language (unchanged when there is no translation)."""
    if LANG == "en" or not s or depth > 5:
        return s
    body = s.strip()
    lead, trail = s[:len(s) - len(s.lstrip())], s[len(s.rstrip()):]
    if body in RO:
        return lead + RO[body] + trail
    for _, rx, v in _PATTERNS:
        m = rx.match(body)
        if m:
            parts = v.split("{}")
            groups = [T(g, depth + 1) for g in m.groups()]
            out = parts[0]
            for i, part in enumerate(parts[1:]):
                out += (groups[i] if i < len(groups) else "") + part
            return lead + out + trail
    if "\n" in body:
        return lead + "\n".join(T(line, depth + 1) for line in body.split("\n")) + trail
    return s


def quote(a):
    a = str(a)
    if os.name == "nt":
        return '"%s"' % a if (" " in a or not a) else a
    return shlex.quote(a)


def find_tool(name, env):
    p = os.environ.get(env)
    if p:
        return p
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (here, os.path.join(here, "platform-tools"), os.path.join(os.path.dirname(here), "platform-tools")):
        for n in (name, name + ".exe"):
            c = os.path.join(d, n)
            if os.path.isfile(c) and os.access(c, os.X_OK):
                return c
    return shutil.which(name)


class Refused(Exception):
    """A repair that isn't safe or possible right now; the message says why and what to do instead."""


# ---------------------------------------------------------------------------------------------------
# Running the tools

class Runner:
    """Runs adb / fastboot / heimdall. Commands that change the phone are shown first and skipped with --dry-run."""

    def __init__(self, log=None, dry=False, serial=None, wait=None):
        self.log = log or (lambda s: print(T(s), flush=True))
        self.dry = dry
        self.serial = serial or os.environ.get("ANDROID_SERIAL") or None
        self.wait_s = float(os.environ.get("FMRRE_WAIT", "120")) if wait is None else wait
        self.tools = {"adb": find_tool("adb", "ADB"), "fastboot": find_tool("fastboot", "FASTBOOT"),
                      "heimdall": find_tool("heimdall", "HEIMDALL")}
        self.last_output = ""

    def have(self, tool):
        return bool(self.tools.get(tool))

    def _cmd(self, tool, args):
        cmd = [self.tools[tool]]
        if self.serial and tool in ("adb", "fastboot"):
            cmd += ["-s", self.serial]
        return cmd + [str(a) for a in args]

    def call(self, tool, *args, timeout=30):
        """A command that only reads: (exit code, stdout + stderr)."""
        if not self.have(tool):
            return 127, tool + " not found"
        try:
            p = subprocess.run(self._cmd(tool, args), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout)
            return p.returncode, p.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            return 124, tool + " timed out"
        except OSError as e:
            return 126, str(e)

    def write(self, tool, *args):
        """A command that changes the phone: shown, its output streamed to the log. True on success."""
        shown = [tool] + (["-s", self.serial] if self.serial and tool in ("adb", "fastboot") else []) + [str(a) for a in args]
        self.log("   $ " + " ".join(quote(a) for a in shown))
        self.last_output = ""
        if self.dry:
            return True
        if not self.have(tool):
            raise Refused("%s is not installed - %s" % (tool, INSTALL_HINT[tool]))
        try:
            p = subprocess.Popen(self._cmd(tool, args), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except OSError as e:
            self.log("     " + str(e))
            return False
        failed = False
        lines = []
        for raw in p.stdout:
            line = raw.decode("utf-8", "replace").rstrip()
            if line:
                lines.append(line)
                self.log("     " + line)
                failed |= "FAILED" in line or line.startswith("ERROR:")
        rc = p.wait()
        self.last_output = "\n".join(lines)
        return rc == 0 and not failed

    def wait_for(self, what, timeout=None):
        """Wait until the phone shows up as `what`: fastboot, download, or an adb state (device, recovery, sideload)."""
        if self.dry:
            return True
        end = time.time() + (self.wait_s if timeout is None else timeout)
        self.log("   waiting for the phone (%s)..." % what)
        while True:
            if what == "fastboot":
                if fastboot_serials(self):
                    return True
            elif what == "download":
                if self.call("heimdall", "detect", timeout=15)[0] == 0:
                    return True
            else:
                rc, out = self.call("adb", "get-state", timeout=10)
                if rc == 0 and out.strip().splitlines()[-1:] == [what]:
                    return True
            if time.time() >= end:
                return False
            time.sleep(1)


# ---------------------------------------------------------------------------------------------------
# What phone, in which mode, locked or not

class Phone:
    def __init__(self):
        self.mode = "none"
        self.serial = ""
        self.manufacturer = ""
        self.model = ""
        self.device = ""         # codename
        self.android = ""
        self.slot = ""           # "a" / "b"; "" = not an A/B phone (or unknown)
        self.vars = {}           # fastboot variables
        self.props = {}          # Android / recovery properties
        self.locked = None       # True / False / None = the phone doesn't say
        self.lock_how = ""
        self.unlock_allowed = None  # Developer options > OEM unlocking
        self.fmrre = False      # FMRRE is the running recovery
        self.cli = FMRRE_CLI      # how to call its command line tool (builds from before the rename: "twrp")
        self.notes = []

    @property
    def maker(self):
        return (self.manufacturer or "").strip().lower()

    @property
    def name(self):
        parts = [p for p in (self.manufacturer if self.manufacturer and not self.model.lower().startswith(self.maker) else "",
                             self.model) if p]
        n = " ".join(parts) or T("Unknown phone")
        if self.device and self.device.lower() not in n.lower():
            n += " (%s)" % self.device
        return n

    def to_dict(self):
        return {"mode": self.mode, "mode_text": MODE_TEXT.get(self.mode, self.mode), "serial": self.serial,
                "manufacturer": self.manufacturer, "model": self.model, "device": self.device, "android": self.android,
                "slot": self.slot, "locked": self.locked, "lock_how": self.lock_how, "unlock_allowed": self.unlock_allowed,
                "fmrre": self.fmrre, "notes": self.notes}


def cache_path():
    return os.path.join(os.path.expanduser("~"), ".fmrre_rescue.json")


def load_cache():
    try:
        with open(cache_path()) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_cache(ph):
    """Remember the model by serial: the bootloader later only reports a codename."""
    if not ph.serial or not ph.model:
        return
    c = load_cache()
    c[ph.serial] = {"manufacturer": ph.manufacturer, "model": ph.model, "device": ph.device}
    try:
        with open(cache_path(), "w") as f:
            json.dump(c, f, indent=1)
    except OSError:
        pass


def parse_getvars(out):
    """fastboot getvar output -> {name: value}. Handles '(bootloader) slot-unbootable:a:no' and 'unlocked: yes'."""
    v = {}
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("(bootloader)"):
            line = line[len("(bootloader)"):].strip()
        if ":" not in line or line.startswith(("Finished", "OKAY", "FAILED", "getvar:", "<", "fastboot:")):
            continue
        k, _, val = line.rpartition(":")
        k, val = k.strip(), val.strip()
        if k and k != "all":
            v[k] = val
    return v


def fastboot_serials(r):
    rc, out = r.call("fastboot", "devices", timeout=15)
    s = []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 2 and f[1] in ("fastboot", "fastbootd"):
            s.append(f[0])
    return [x for x in s if not r.serial or x == r.serial]


def lock_from_props(p):
    fl, st, vbs = p.get("ro.boot.flash.locked", ""), p.get("ro.boot.vbmeta.device_state", ""), p.get("ro.boot.verifiedbootstate", "")
    how = ", ".join("%s=%s" % (k, v) for k, v in (("ro.boot.flash.locked", fl), ("vbmeta.device_state", st),
                                                   ("verifiedbootstate", vbs)) if v)
    if fl == "0" or st == "unlocked" or vbs == "orange":
        return False, how
    if fl == "1" or st == "locked" or vbs in ("green", "yellow"):
        return True, how
    return None, how or "the phone reports no lock state"


def detect(r):
    ph = Phone()
    # ---- bootloader / fastbootd
    fb = fastboot_serials(r) if r.have("fastboot") else []
    if fb:
        if len(fb) > 1:
            ph.notes.append("%d phones in fastboot - using %s (choose one with --serial)" % (len(fb), fb[0]))
        r.serial = r.serial or fb[0]
        ph.serial = fb[0]
        rc, out = r.call("fastboot", "getvar", "all", timeout=45)
        ph.vars = parse_getvars(out)
        for k in ("product", "unlocked", "current-slot", "is-userspace", "snapshot-update-status"):
            if k not in ph.vars:  # not every bootloader lists everything under "all"
                rc, o = r.call("fastboot", "getvar", k, timeout=15)
                vv = parse_getvars(o)
                if k in vv and "FAILED" not in o:
                    ph.vars[k] = vv[k]
        v = ph.vars
        ph.mode = "fastbootd" if v.get("is-userspace") == "yes" else "fastboot"
        ph.device = v.get("product", "")
        ph.slot = v.get("current-slot", "").lstrip("_")
        u = v.get("unlocked", "").lower()
        if u in ("yes", "true"):
            ph.locked, ph.lock_how = False, "fastboot getvar unlocked: yes"
        elif u in ("no", "false"):
            ph.locked, ph.lock_how = True, "fastboot getvar unlocked: no"
        else:
            rc, o = r.call("fastboot", "oem", "device-info", timeout=15)
            m = re.search(r"Device unlocked:\s*(true|false)", o, re.I)
            if m:
                ph.locked = m.group(1).lower() == "false"
                ph.lock_how = "fastboot oem device-info: Device unlocked: " + m.group(1).lower()
            else:
                ph.lock_how = "this bootloader doesn't report it"
        c = load_cache().get(ph.serial, {})
        ph.manufacturer, ph.model = c.get("manufacturer", ""), c.get("model", "")
        if not ph.model and ph.device in PIXELS:
            ph.manufacturer, ph.model = "Google", PIXELS[ph.device]
        return ph
    # ---- Samsung Download mode
    if r.have("heimdall") and r.call("heimdall", "detect", timeout=15)[0] == 0:
        ph.mode = "download"
        ph.manufacturer = "Samsung"
        ph.lock_how = "Download mode doesn't tell the computer - it is on the phone's screen"
        return ph
    # ---- adb: Android, recovery, sideload
    if r.have("adb"):
        rc, out = r.call("adb", "devices", "-l", timeout=20)
        devs = []
        for line in out.splitlines():
            f = line.split()
            if len(f) >= 2 and not line.startswith(("*", "List of")):
                devs.append(f)
        if r.serial:
            devs = [d for d in devs if d[0] == r.serial]
        if devs:
            if len(devs) > 1:
                ph.notes.append("%d phones on adb - using %s (choose one with --serial)" % (len(devs), devs[0][0]))
            d = devs[0]
            r.serial = r.serial or d[0]
            ph.serial = d[0]
            info = dict(x.split(":", 1) for x in d[2:] if ":" in x)
            ph.mode = {"device": "android", "recovery": "recovery", "sideload": "sideload", "rescue": "rescue",
                       "unauthorized": "unauthorized", "offline": "offline", "bootloader": "fastboot"}.get(d[1], d[1])
            ph.model, ph.device = info.get("model", "").replace("_", " "), info.get("device", "")
            if ph.mode in ("android", "recovery"):
                rc, o = r.call("adb", "shell", "getprop", timeout=25)
                ph.props = p = dict(re.findall(r"^\[([^\]]+)\]: \[(.*)\]\s*$", o, re.M))
                ph.manufacturer = p.get("ro.product.manufacturer") or p.get("ro.product.vendor.manufacturer") or ph.manufacturer
                ph.model = p.get("ro.product.model") or p.get("ro.product.vendor.model") or ph.model
                ph.device = p.get("ro.product.device") or p.get("ro.product.vendor.device") or ph.device
                ph.android = p.get("ro.build.version.release", "")
                ph.slot = p.get("ro.boot.slot_suffix", "").lstrip("_")
                ph.locked, ph.lock_how = lock_from_props(p)
                a = p.get("sys.oem_unlock_allowed", "")
                ph.unlock_allowed = True if a == "1" else False if a == "0" else None
                if p.get("ro.boot.warranty_bit") == "1":
                    ph.notes.append("Samsung Knox warranty bit: tripped (0x1)")
                if ph.mode == "recovery":
                    rc, o = r.call("adb", "shell", *FMRRE_CLI, "help", timeout=25)
                    ph.fmrre = "FMRRE" in o
                    if not ph.fmrre:  # a build from before the rename only knows "twrp" (with "fmrre" it would start its UI)
                        rc, o = r.call("adb", "shell", "/system/bin/recovery", "twrp", "help", timeout=25)
                        ph.fmrre = "TWRP v2" in o
                        if ph.fmrre:
                            ph.cli = ["/system/bin/recovery", "twrp"]
            save_cache(ph)
    return ph


def lock_verdict(ph):
    """(word, tone, explanation); tone is ok / bad / warn."""
    if ph.mode == "none":
        return "-", "warn", ""
    if ph.locked is False:
        return "UNLOCKED", "ok", "Repairs from this computer are possible."
    if ph.locked is True:
        text = "Nothing can be flashed while the bootloader is locked. To unlock (it ERASES the phone): " + \
               UNLOCK_HELP.get(ph.maker, UNLOCK_DEFAULT)
        if ph.unlock_allowed is True:
            text = "OEM unlocking is already allowed in Developer options. " + text
        elif ph.unlock_allowed is False and ph.mode == "android":
            text = "OEM unlocking is OFF in Developer options. " + text
        return "LOCKED", "bad", text + " Without unlocking: " + OFFICIAL_RESTORE.get(ph.maker, OFFICIAL_DEFAULT) + \
            " This tool can do the sideload: sideload <official full OTA>.zip"
    if ph.mode == "download":
        return "CHECK THE SCREEN", "warn", (
            "Samsung Download mode doesn't tell the computer. On the phone's screen: 'OEM LOCK: OFF (U)' (or just OFF) means "
            "unlocked, 'ON (L)' means locked. 'KG STATE: Prenormal' (or 'RMM STATE: Prenormal') refuses custom images until the "
            "phone has been online for a while. Checked? Tick the box (or pass --unlocked).")
    if ph.mode in ("unauthorized", "offline", "sideload", "rescue"):
        return "UNKNOWN", "warn", "Can't ask the phone in this mode."
    return "UNKNOWN", "warn", "The phone doesn't report it (%s). If you know it is unlocked, pass --unlocked." % ph.lock_how


def describe(ph):
    rows = [("Phone", ph.name if ph.mode != "none" else "-"), ("Mode", MODE_TEXT.get(ph.mode, ph.mode))]
    if ph.serial:
        rows.append(("Serial", ph.serial))
    if ph.android:
        rows.append(("Android", ph.android))
    if ph.slot:
        rows.append(("Slot", ph.slot.upper() + slot_health(ph)))
    if ph.mode == "recovery":
        rows.append(("Recovery", "FMRRE" if ph.fmrre else "not FMRRE (stock or another recovery)"))
    word, _, _ = lock_verdict(ph)
    if ph.mode != "none":
        rows.append(("Bootloader", word + ("  (%s)" % ph.lock_how if ph.lock_how and ph.locked is not None else "")))
    for n in ph.notes:
        rows.append(("Note", n))
    return rows


def slot_health(ph):
    v = ph.vars
    if not v or not ph.slot:
        return ""
    out = []
    for s in ("a", "b"):
        st = []
        if v.get("slot-unbootable:" + s) == "yes":
            st.append(T("unbootable"))
        if v.get("slot-successful:" + s) == "yes":
            st.append(T("booted fine"))
        elif v.get("slot-successful:" + s) == "no":
            st.append(T("never booted fully"))
        rc = v.get("slot-retry-count:" + s)
        if rc not in (None, ""):
            st.append(T("%s %s left" % (rc, "try" if rc == "1" else "tries")))
        if st:
            out.append("%s: %s" % (s.upper(), ", ".join(st)))
    return ("  -  " + "; ".join(out)) if out else ""


# ---------------------------------------------------------------------------------------------------
# What is wrong

class Finding:
    def __init__(self, level, title, detail, action=None):
        self.level, self.title, self.detail, self.action = level, title, detail, action  # level: 0 info, 1 warning, 2 problem


def other_slot(s):
    return "b" if s == "a" else "a"


def slot_problem(ph):
    """(this slot, other slot, why) when this slot can't boot and the other one did; else None."""
    v, cur = ph.vars, ph.slot
    if not cur or not v:
        return None
    oth = other_slot(cur)
    cu, cs, cr = v.get("slot-unbootable:" + cur), v.get("slot-successful:" + cur), v.get("slot-retry-count:" + cur)
    ou, osu = v.get("slot-unbootable:" + oth), v.get("slot-successful:" + oth)
    if osu == "yes" and ou != "yes" and (cu == "yes" or cr == "0" or cs == "no"):
        why = "is marked unbootable" if cu == "yes" else "has no boot attempts left" if cr == "0" else "never finished booting"
        return cur, oth, why
    return None


def image_kind(path):
    try:
        with open(path, "rb") as f:
            m = f.read(8)
    except OSError:
        return ""
    return "boot" if m == b"ANDROID!" else "vendor_boot" if m == b"VNDRBOOT" else ""


def diagnose(r, ph, image=None):
    f = []
    m = ph.mode
    if m == "none":
        missing = [t for t in ("adb", "fastboot") if not r.have(t)]
        detail = CONNECT_STEPS + ("\nMissing on this computer: %s - %s" % (", ".join(missing), INSTALL_HINT["adb"]) if missing else "")
        return [Finding(2, "No phone found", detail)]
    if m == "unauthorized":
        return [Finding(2, "USB debugging isn't allowed for this computer",
                        "Unlock the phone's screen and tap Allow on 'Allow USB debugging?' (tick 'Always allow'). No prompt? Developer "
                        "options > Revoke USB debugging authorizations, then plug in again.")]
    if m == "offline":
        return [Finding(2, "adb sees the phone as offline", "Unplug and plug in again (another cable or port helps), or restart adb: "
                        "'adb kill-server'.")]
    if ph.locked:
        f.append(Finding(2, "The bootloader is locked", lock_verdict(ph)[2]))
    if m in ("fastboot", "fastbootd"):
        v = ph.vars
        sp = slot_problem(ph)
        if sp:
            f.append(Finding(2, "Slot %s %s; slot %s booted fine before" % (sp[0].upper(), sp[2], sp[1].upper()),
                             "Switching to slot %s goes back to the system that worked - often the one before an update." % sp[1].upper(),
                             "switch_slot"))
        elif ph.slot and v.get("slot-unbootable:a") == "yes" and v.get("slot-unbootable:b") == "yes":
            f.append(Finding(2, "Both slots are marked unbootable",
                             "Start FMRRE from this computer to look closer, or reinstall the firmware.", "boot_fmrre"))
        snap = v.get("snapshot-update-status", "")
        if snap == "merging":
            f.append(Finding(1, "A system update is still being merged",
                             "Don't flash or wipe anything until Android finishes it. If it can't start to finish it, the maker's official "
                             "firmware tool is the safe way."))
        elif snap == "snapshotted":
            f.append(Finding(1, "A system update is installed but not finished",
                             "If the bootloop began with this update, cancelling it (and starting the other slot) goes back to the "
                             "previous version. fastbootd also refuses to flash system partitions until it is cancelled.",
                             "cancel_update"))
        try:
            mv = int(v.get("battery-voltage", "0").split()[0])
        except ValueError:
            mv = 0
        if v.get("battery-soc-ok") == "no" or 0 < mv < 3500:
            f.append(Finding(1, "The battery is low", "Charge it for half an hour before flashing anything."))
        if image and image_kind(image) == "boot":
            f.append(Finding(0, "FMRRE can be started from this computer",
                             "'fastboot boot' runs it from RAM - nothing is written - and its Auto repair checks the logs, boot images, "
                             "vbmeta, both slots and /data from the inside.", "boot_fmrre"))
        elif image:
            f.append(Finding(1, "This image can't be started with fastboot boot",
                             "%s isn't a boot/recovery image (vendor_boot images can only be flashed)." % os.path.basename(image)))
        else:
            f.append(Finding(0, "For the most thorough check: an FMRRE image for this phone",
                             "With --recovery fmrre-recovery.img (built from this phone's own recovery or boot image) it is started from "
                             "RAM and Auto repair looks at the phone from the inside. Without one: the recovery installed on the phone "
                             "is tried.", "reboot_recovery"))
    elif m == "download":
        f.append(Finding(0, "Samsung Download mode",
                         "Install FMRRE (recovery + vbmeta with verification disabled) and let its Auto repair fix the phone, or flash "
                         "the stock boot / vbmeta / recovery from your firmware's AP file to undo a bad change.", "samsung_fmrre"))
    elif m == "android":
        f.append(Finding(0, "Android starts", "When it misbehaves: the tool can reboot to recovery and run FMRRE's Auto repair "
                         "(when FMRRE is the installed recovery).", "reboot_recovery"))
    elif m == "recovery":
        if ph.fmrre:
            rc, out = r.call("adb", "shell", *ph.cli, "autofix", "--dry-run", timeout=180)
            lines = [l.rstrip() for l in out.splitlines() if l.strip() and not l.startswith(">")]
            planned = [l[4:].strip() for l in lines if l.startswith("[x]")]
            bad = [l[5:].strip() for l in lines if l.startswith("[!!]")]
            head = lines[0] if lines else "FMRRE Auto repair"
            detail = []
            if bad:
                detail.append("Problems: " + "; ".join(bad))
            if planned:
                detail.append("Planned fixes: " + "; ".join(planned))
            detail += [l[len("Advice: "):] for l in lines if l.startswith("Advice: ")]
            f.append(Finding(2 if planned or bad else 0, head, "\n".join(detail) or "Nothing to repair.", "autofix" if planned else None))
        else:
            f.append(Finding(1, "A recovery other than FMRRE is running",
                             "A stock recovery can reinstall the official system without unlocking: 'Apply update from ADB', then 'adb "
                             "sideload <official OTA>.zip'. To use FMRRE: reboot to the bootloader and start it from here.",
                             "reboot_bootloader"))
    elif m == "sideload":
        f.append(Finding(0, "Waiting for adb sideload", "'adb sideload <rom-or-ota>.zip' installs it; cancel sideload on the phone to get back."))
    return f


# ---------------------------------------------------------------------------------------------------
# Images: vbmeta with verification disabled, and LZ4 frames (Samsung firmware)

def vbmeta_disabled(size=4096):
    """Unsigned vbmeta with no descriptors and flags=3 (avbtool make_vbmeta_image --flags 3)."""
    h = bytearray(256)
    h[0:4] = b"AVB0"
    struct.pack_into(">II", h, 4, 1, 0)
    struct.pack_into(">I", h, 120, 3)
    rel = b"avbtool 1.3.0 (fmrre)"
    h[128:128 + len(rel)] = rel
    return bytes(h) + bytes(size - len(h))


def vbmeta_patch(data):
    if data[:4] != b"AVB0":
        raise Refused("not a vbmeta image (no AVB0 header)")
    out = bytearray(data)
    struct.pack_into(">I", out, 120, struct.unpack_from(">I", out, 120)[0] | 3)
    return bytes(out)


def lz4_block(src, prefix=b""):
    """Decompress one LZ4 block; `prefix` is the previous output (blocks that depend on earlier ones)."""
    dst = bytearray(prefix)
    base, i, n = len(prefix), 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]
        i += lit
        if i >= n:
            break  # the last sequence has literals only
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if off == 0 or off > len(dst):
            raise ValueError("corrupt LZ4 data")
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:  # overlapping copy: the last `off` bytes repeat
            dst += (dst[start:] * (ml // off + 1))[:ml]
    return bytes(dst[base:])


def lz4_decode(fin, fout):
    """LZ4 frame format (what the lz4 tool and Samsung firmware use) from fin to fout; returns the bytes written."""
    total = 0
    while True:
        magic = fin.read(4)
        if not magic:
            return total
        if len(magic) < 4:
            raise ValueError("truncated LZ4 frame")
        (mv,) = struct.unpack("<I", magic)
        if 0x184D2A50 <= mv <= 0x184D2A5F:  # skippable frame
            (sz,) = struct.unpack("<I", fin.read(4))
            fin.read(sz)
            continue
        if mv != 0x184D2204:
            if total:
                return total  # trailing bytes after the last frame
            raise ValueError("not an LZ4 frame")
        flg, _bd = fin.read(2)
        if flg >> 6 != 1:
            raise ValueError("unsupported LZ4 frame version")
        independent, block_sum, has_size, content_sum, has_dict = flg & 0x20, flg & 0x10, flg & 0x08, flg & 0x04, flg & 0x01
        fin.read((8 if has_size else 0) + (4 if has_dict else 0) + 1)  # content size, dictionary id, header checksum
        history = b""
        while True:
            (bs,) = struct.unpack("<I", fin.read(4))
            if bs == 0:
                break
            raw, bs = bs & 0x80000000, bs & 0x7FFFFFFF
            data = fin.read(bs)
            if len(data) != bs:
                raise ValueError("truncated LZ4 block")
            if block_sum:
                fin.read(4)
            out = data if raw else lz4_block(data, b"" if independent else history)
            fout.write(out)
            total += len(out)
            if not independent:
                history = (history + out)[-65536:]
        if content_sum:
            fin.read(4)


def check_tar_md5(path, log):
    """A Samsung .tar.md5 ends with '<md5>  <name>.tar' after the tar: check it like Odin does (False = damaged)."""
    import hashlib
    size = os.path.getsize(path)
    tail = size % 512  # a tar is whole 512-byte blocks; the md5 line comes after them
    if tail == 0:
        return True  # a plain .tar: nothing to check
    with open(path, "rb") as f:
        f.seek(size - tail)
        line = f.read().decode("ascii", "replace").strip()
        want = line.split()[0].lower() if line else ""
        if len(want) != 32:
            return True
        log("   checking the MD5 of %s (%d MB)..." % (os.path.basename(path), size >> 20))
        h, left = hashlib.md5(), size - tail
        f.seek(0)
        while left:
            b = f.read(min(left, 4 << 20))
            if not b:
                break
            h.update(b)
            left -= len(b)
    return h.hexdigest() == want


def ap_images(ap, parts, workdir, log):
    """Unpack <part>.img(.lz4) for each wanted partition from a Samsung AP .tar / .tar.md5 into workdir."""
    out = []
    try:
        t = tarfile.open(ap)
    except (OSError, tarfile.TarError) as e:
        raise Refused("%s is not a firmware tar: %s" % (ap, e))
    with t:
        for mem in t.getmembers():
            if not mem.isfile():
                continue
            base = os.path.basename(mem.name)
            stem = base[:-4] if base.endswith(".lz4") else base
            if not stem.endswith(".img") or stem[:-4] not in parts:
                continue
            dst = os.path.join(workdir, stem)
            log("   unpacking %s (%d MB)" % (base, mem.size >> 20))
            src = t.extractfile(mem)
            try:
                with open(dst, "wb") as fo:
                    if base.endswith(".lz4"):
                        lz4_decode(src, fo)
                    else:
                        shutil.copyfileobj(src, fo, 1 << 20)
            except (ValueError, IndexError, struct.error, tarfile.TarError) as e:
                raise Refused("%s in %s is damaged (%s) - download the firmware again." % (base, os.path.basename(ap), e))
            out.append((stem[:-4], dst))
    order = {p: i for i, p in enumerate(parts)}
    return sorted(out, key=lambda x: order.get(x[0], 99))


def factory_info(path):
    """A factory image zip (Pixel and other fastboot phones): its bootloader / radio images, the image-*.zip
    and the boards it is for (android-info.txt: 'require board=husky')."""
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        raise Refused("%s is not a factory image zip: %s" % (os.path.basename(path), e))
    with z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        pick = lambda pre, ext: sorted(n for n in names if os.path.basename(n).startswith(pre) and n.endswith(ext))
        boot, radio, image = pick("bootloader-", ".img"), pick("radio-", ".img"), pick("image-", ".zip")
    if not image:
        raise Refused("%s has no image-*.zip inside: not a factory image (flash an OTA zip with adb sideload instead)."
                      % os.path.basename(path))
    return {"bootloader": boot[0] if boot else "", "radio": radio[0] if radio else "", "image": image[0]}


def ota_info(path):
    """What an OTA / ROM zip says about itself (META-INF/com/android/metadata): {"pre-device": ..., "ota-type": ...}."""
    if not zipfile.is_zipfile(path):
        raise Refused("%s is not a zip - an OTA or ROM for sideload is a .zip file." % os.path.basename(path))
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        meta = {}
        if "META-INF/com/android/metadata" in names:
            for line in z.read("META-INF/com/android/metadata").decode("utf-8", "replace").splitlines():
                k, _, v = line.partition("=")
                if k.strip():
                    meta[k.strip()] = v.strip()
        meta["_installable"] = "payload.bin" in names or "META-INF/com/google/android/update-binary" in names
    return meta


def boards_in(image_zip):
    try:
        with zipfile.ZipFile(image_zip) as z:
            text = z.read("android-info.txt").decode("utf-8", "replace")
    except (OSError, KeyError, zipfile.BadZipFile):
        return []
    for line in text.splitlines():
        m = re.match(r"\s*require\s+(?:board|product)\s*=\s*(\S+)", line)
        if m:
            return [b.strip().lower() for b in m.group(1).split("|") if b.strip()]
    return []


def check_magic(part, path):
    with open(path, "rb") as f:
        m = f.read(8)
    want = {"vbmeta": b"AVB0", "vendor_boot": b"VNDRBOOT", "boot": b"ANDROID!", "init_boot": b"ANDROID!", "recovery": b"ANDROID!"}.get(part)
    if want and not m.startswith(want):
        raise Refused("%s doesn't look like a %s image (header %r) - not flashing it" % (os.path.basename(path), part, m[:8]))


def pit_names(r):
    """{'boot.img': 'BOOT', ...} from the phone's partition table; the Download mode session stays open (--resume)."""
    rc, out = r.call("heimdall", "print-pit", "--no-reboot", timeout=60)
    names, cur = {}, None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("Partition Name:"):
            cur = line.split(":", 1)[1].strip()
        elif line.startswith("Flash Filename:") and cur:
            fn = line.split(":", 1)[1].strip()
            if fn:
                names[fn.lower()] = cur
            cur = None
    return names if rc == 0 else {}


# ---------------------------------------------------------------------------------------------------
# Repairs

class Options:
    def __init__(self, **kw):
        self.yes = False
        self.unlocked = False     # the user checked: bootloader unlocked (when the phone doesn't say)
        self.recovery = None      # FMRRE image
        self.ap = None            # Samsung AP tar
        self.ota = None           # an OTA / ROM zip for adb sideload
        self.vbmeta = None        # stock vbmeta.img
        self.both_slots = False
        self.allow_wipe = False
        self.reboot = True
        self.parts = list(SAMSUNG_STOCK_PARTS)
        self.workdir = None
        self.__dict__.update(kw)


class Rescue:
    """The repairs, shared by the command line and the window. ask(question) -> bool; typed(word, question) -> bool."""

    def __init__(self, runner, opts, ask=None, typed=None):
        self.r, self.o = runner, opts
        self.log = runner.log
        self.ask_fn = ask or self._ask_tty
        self.typed_fn = typed or self._typed_tty
        self.phone = Phone()

    # ---- confirmations
    def _ask_tty(self, q):
        try:
            return input(q + (" [d/N] " if LANG == "ro" else " [y/N] ")).strip().lower() in ("y", "yes", "d", "da")
        except EOFError:
            return False

    def _typed_tty(self, word, q):
        try:
            return input("%s\n%s " % (T(q), T("Type %s to continue:" % word))).strip() == word
        except EOFError:
            return False

    def ask(self, q):
        q = T(q)
        if self.o.yes:
            self.log("   " + q + " yes (--yes)")
            return True
        return self.ask_fn(q)

    # ---- state
    def refresh(self):
        self.phone = detect(self.r)
        return self.phone

    def workdir(self):
        if not self.o.workdir:
            self.o.workdir = tempfile.mkdtemp(prefix="fmrre-rescue-")
        os.makedirs(self.o.workdir, exist_ok=True)
        return self.o.workdir

    def need_unlocked(self, what):
        ph = self.phone
        if ph.locked is True:
            raise Refused("Not %s: the bootloader is LOCKED.\n%s" % (what, lock_verdict(ph)[2]))
        if ph.locked is None and not self.o.unlocked:
            raise Refused("Not %s: the phone doesn't say whether its bootloader is unlocked.\n%s" % (what, lock_verdict(ph)[2]))

    def need_mode(self, *modes):
        if self.phone.mode not in modes:
            raise Refused("This needs the phone in %s mode - it is in: %s." %
                          ((" %s " % T("or")).join(T(MODE_TEXT.get(m, m)) for m in modes), T(MODE_TEXT.get(self.phone.mode, self.phone.mode))))

    def need_not_merging(self):
        if self.phone.vars.get("snapshot-update-status") == "merging":
            raise Refused("A system update is still being merged: flashing or wiping now could leave the phone unable to start.")

    def to_bootloader(self):
        m = self.phone.mode
        if m == "fastboot":
            return
        if m == "fastbootd":
            ok = self.r.write("fastboot", "reboot", "bootloader")
        elif m in ("android", "recovery"):
            ok = self.r.write("adb", "reboot", "bootloader")
        else:
            raise Refused("Can't get to the bootloader from %s - power off, then hold Volume Down + Power." % MODE_TEXT.get(m, m))
        if not ok or not self.r.wait_for("fastboot"):
            raise Refused("The phone didn't come back in the bootloader. Hold Volume Down + Power, then try again.")
        self.refresh()

    # ---- single repairs
    def switch_slot(self):
        self.need_mode("fastboot", "fastbootd")
        self.need_unlocked("switching slots")
        if not self.phone.slot:
            raise Refused("This phone has no A/B slots.")
        oth = other_slot(self.phone.slot)
        if not self.ask("Make slot %s active (the other system, can be switched back)?" % oth.upper()):
            return False
        return self.r.write("fastboot", "set_active", oth)

    def cancel_update(self):
        self.need_mode("fastboot", "fastbootd")
        self.need_unlocked("cancelling the update")
        if not self.ask("Cancel the pending system update?"):
            return False
        return self.r.write("fastboot", "snapshot-update", "cancel")

    def disable_vbmeta(self):
        self.need_mode("fastboot", "fastbootd")
        self.need_unlocked("changing vbmeta")
        self.need_not_merging()
        if self.o.vbmeta:
            src = self.o.vbmeta
            with open(src, "rb") as f:
                vbmeta_patch(f.read())  # refuses a file that isn't vbmeta
        else:
            src = os.path.join(self.workdir(), "vbmeta_disabled.img")
            with open(src, "wb") as f:
                f.write(vbmeta_disabled())
        where = "both slots" if self.o.both_slots else ("slot " + self.phone.slot.upper() if self.phone.slot else "vbmeta")
        if not self.ask("Flash vbmeta with verification disabled (%s)?" % where):
            return False
        args = ["--disable-verity", "--disable-verification", "flash"] + (["--slot=all"] if self.o.both_slots and self.phone.slot else [])
        return self.r.write("fastboot", *args, "vbmeta", src)

    def flash(self, part, image):
        if not os.path.isfile(image):
            raise Refused("No such file: " + image)
        self.need_unlocked("flashing")
        if self.phone.mode == "download":
            check_magic(part, image)
            if not self.ask("Flash %s to %s with Heimdall?" % (os.path.basename(image), part.upper())):
                return False
            return self.r.write("heimdall", "flash", "--" + part.upper(), image, "--no-reboot")
        self.need_mode("fastboot", "fastbootd")
        self.need_not_merging()
        check_magic(part, image)
        if not self.ask("Flash %s to %s?" % (os.path.basename(image), part)):
            return False
        return self.r.write("fastboot", "flash", part, image)

    def sideload(self, path):
        """Install an OTA / ROM zip with adb sideload. Allowed on a LOCKED phone: the stock recovery checks that it is
        signed by the maker (an official full OTA reinstalls the system and keeps apps and files)."""
        if not os.path.isfile(path):
            raise Refused("No such file: " + path)
        meta = ota_info(path)
        if not meta["_installable"]:
            raise Refused("%s has neither payload.bin nor an update-binary: it isn't an OTA or a flashable zip." % os.path.basename(path))
        devs = [d.strip().lower() for d in meta.get("pre-device", "").split(",") if d.strip()]
        product = (self.phone.device or "").lower()
        if devs and product and product not in devs:
            raise Refused("This OTA is for %s, but the phone is %s - the recovery would refuse it." % (", ".join(devs), product))
        m = self.phone.mode
        if m == "none":
            raise Refused(HOW_TO_CONNECT)
        if m in ("unauthorized", "offline"):
            raise Refused(diagnose(self.r, self.phone)[0].detail)
        if m != "sideload":
            if m in ("android", "recovery"):
                # FMRRE has its own sideload mode; Android and most stock recoveries start it with `adb reboot sideload`.
                ok = self.r.write("adb", "shell", *self.phone.cli, "sideload") if m == "recovery" and self.phone.fmrre \
                    else self.r.write("adb", "reboot", "sideload")
            elif m in ("fastboot", "fastbootd"):
                self.log("On the phone: choose 'Apply update from ADB' in the recovery that starts now (volume keys to move, "
                         "power to select).")
                ok = self.r.write("fastboot", "reboot", "recovery")
            else:
                raise Refused("Can't start sideload from %s. Start the recovery, then choose 'Apply update from ADB'." % T(MODE_TEXT.get(m, m)))
            if not ok:  # starting sideload switches USB over, so this can look like a failure - wait and see
                self.log("   (if nothing happens: on the phone, in the recovery, choose 'Apply update from ADB')")
            if not self.r.wait_for("sideload", max(self.r.wait_s, 180)):
                raise Refused("The phone isn't waiting for adb sideload. In its recovery choose 'Apply update from ADB' "
                              "(FMRRE: Install > Sideload), then run this again.")
            self.refresh()
        q = "Install %s with adb sideload?" if self.phone.locked is False else \
            "Install %s with adb sideload? (On a locked phone only the maker's signed OTA is accepted.)"
        if not self.ask(q % os.path.basename(path)):
            return False
        ok = self.r.write("adb", "sideload", path)
        return ok or "Total xfer:" in self.r.last_output  # some recoveries end the connection before adb reports success

    def factory(self, path, wipe=False):
        """Reinstall the official firmware from a factory image zip - like its flash-all script, without -w data stays."""
        if not os.path.isfile(path):
            raise Refused("No such file: " + path)
        self.need_mode("fastboot", "fastbootd")
        self.need_unlocked("reinstalling the firmware")
        info = factory_info(path)
        self.to_bootloader()
        wd = self.workdir()
        files = {}
        with zipfile.ZipFile(path) as z:
            for k in ("bootloader", "radio", "image"):
                if info[k]:
                    self.log("   unpacking %s" % os.path.basename(info[k]))
                    files[k] = os.path.join(wd, os.path.basename(info[k]))
                    with z.open(info[k]) as src, open(files[k], "wb") as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
        boards, product = boards_in(files["image"]), (self.phone.device or "").lower()
        if boards and product and product not in boards:
            raise Refused("This factory image is for %s, but the phone is %s: not flashing it (the wrong bootloader can make a "
                          "phone unusable)." % (", ".join(boards), product))
        if not boards:
            self.log("   (the image doesn't name its phone - check it is the one for %s)" % (product or "this phone"))
        if wipe and not self.typed_fn("ERASE", "Reinstall and factory reset: ERASES apps, settings, photos and every file on the "
                                               "phone's internal storage."):
            raise Refused("Not wiped (ERASE wasn't typed).")
        q = "Reinstall the official firmware from %s and ERASE the phone?" if wipe else \
            "Reinstall the official firmware from %s (apps and files are kept)?"
        if not self.ask(q % os.path.basename(path)):
            return False
        for k in ("bootloader", "radio"):
            if k in files:
                if not self.r.write("fastboot", "flash", k, files[k]):
                    raise Refused("Flashing the %s failed - see above. Don't restart the phone: try again, or use the maker's "
                                  "official tool." % k)
                self.r.write("fastboot", "reboot-bootloader")
                if not self.r.wait_for("fastboot"):
                    raise Refused("The phone didn't come back in the bootloader after flashing the %s." % k)
        return self.r.write("fastboot", *(["-w"] if wipe else []), "update", files["image"])

    def wipe_data(self):
        self.need_mode("fastboot", "fastbootd")
        self.need_unlocked("wiping")
        self.need_not_merging()
        if not self.typed_fn("ERASE", "Factory reset: ERASES apps, settings, photos and every file on the phone's internal storage."):
            raise Refused("Not wiped (ERASE wasn't typed).")
        return self.r.write("fastboot", "-w")

    def reboot(self, target="system"):
        m = self.phone.mode
        if m in ("fastboot", "fastbootd"):
            args = {"system": ["reboot"], "recovery": ["reboot", "recovery"], "bootloader": ["reboot", "bootloader"],
                    "fastboot": ["reboot", "fastboot"]}.get(target)
            if not args:
                raise Refused("fastboot can't reboot to %s" % target)
            return self.r.write("fastboot", *args)
        if m in ("android", "recovery", "sideload", "rescue"):
            return self.r.write("adb", "reboot", *([] if target == "system" else [target]))
        if m == "download":
            raise Refused("Download mode: hold Volume Down + Power for 7 seconds to restart (add Volume Up right after the screen goes "
                          "black for recovery).")
        raise Refused(HOW_TO_CONNECT)

    def boot_fmrre(self, image):
        kind = image_kind(image)
        if kind != "boot":
            raise Refused("%s can't be started with 'fastboot boot' (%s)." %
                          (os.path.basename(image), "a vendor_boot image" if kind else "not a boot/recovery image"))
        self.need_unlocked("starting FMRRE")
        self.to_bootloader()
        if not self.ask("Start FMRRE from this computer (fastboot boot - nothing is written)?"):
            return False
        if not self.r.write("fastboot", "boot", image):
            raise Refused("The bootloader refused to start the image. On some phones 'fastboot boot' is disabled - flash it instead "
                          "(scripts/install.sh --permanent).")
        return self.wait_fmrre()

    def wait_fmrre(self, timeout=None):
        if not self.r.wait_for("recovery", timeout):
            raise Refused("FMRRE didn't show up on adb. If its screen is on the phone, check the cable / USB driver.")
        self.refresh()
        if not self.phone.fmrre and not self.r.dry:
            raise Refused("A recovery started, but it isn't FMRRE.")
        self.log("   FMRRE is running.")
        return True

    def autofix(self):
        if self.r.dry and self.phone.mode != "recovery":
            self.log("   (dry run: FMRRE would now check the phone and repair it)")
            return True
        self.need_mode("recovery")
        if not self.phone.fmrre:
            raise Refused("The running recovery isn't FMRRE.")
        self.log("FMRRE Auto repair - what it found:")
        rc, out = self.r.call("adb", "shell", *self.phone.cli, "autofix", "--dry-run", timeout=180)
        for line in out.splitlines():
            if line.strip() and not line.startswith(">"):
                self.log("   " + line.rstrip())
        planned = [l for l in out.splitlines() if l.startswith("[x]")]
        args = ["autofix", "--allow-slot-switch"]
        if self.o.allow_wipe:
            if not self.typed_fn("ERASE", "Auto repair may Format Data if it finds that it is needed - that ERASES the phone."):
                raise Refused("Not allowing Format Data (ERASE wasn't typed).")
            args.append("--allow-format-data")
        if not planned and not self.o.allow_wipe:
            self.log("   Nothing for Auto repair to fix. Details: 'fmrre_rescue.py logs' saves the phone's logs.")
            return False
        if self.o.reboot:
            args.append("--reboot")
        if not self.ask("Run these repairs on the phone and restart it?" if self.o.reboot else "Run these repairs on the phone?"):
            return False
        ok = self.r.write("adb", "shell", *self.phone.cli, *args)
        return ok or "Repairs done" in self.r.last_output  # the reboot at the end drops the connection

    def samsung_fmrre(self, image):
        self.need_mode("download")
        self.need_unlocked("installing FMRRE")
        if image_kind(image) != "boot":
            raise Refused("Samsung needs a recovery.img (build it from the stock recovery.img in your firmware's AP file: "
                          "scripts/install.sh --build-from recovery.img).")
        vb = os.path.join(self.workdir(), "vbmeta.img")
        with open(vb, "wb") as f:
            if self.o.vbmeta:
                with open(self.o.vbmeta, "rb") as s:
                    f.write(vbmeta_patch(s.read()))
            else:
                f.write(vbmeta_disabled())
        if not self.ask("Flash FMRRE to RECOVERY and a vbmeta with verification disabled to VBMETA?"):
            return False
        if not self.r.write("heimdall", "flash", "--RECOVERY", image, "--VBMETA", vb, "--no-reboot"):
            raise Refused("Heimdall failed - see above. (Odin on Windows: scripts/install.sh --odin-only builds the AP package.)")
        self.log("Flashed. Now start STRAIGHT into recovery, or stock Android replaces FMRRE on its first boot:\n"
                 "   1. hold Volume Down + Power until the screen goes black\n"
                 "   2. at once switch to Volume Up + Power (+ Bixby on older models) and hold until FMRRE appears")
        return True

    def samsung_stock(self, ap):
        self.need_mode("download")
        self.need_unlocked("flashing")
        if not os.path.isfile(ap):
            raise Refused("No such file: " + ap)
        if not check_tar_md5(ap, self.log):
            raise Refused("%s is damaged: its MD5 doesn't match - download the firmware again." % os.path.basename(ap))
        imgs = ap_images(ap, self.o.parts, self.workdir(), self.log)
        if not imgs:
            raise Refused("%s has none of: %s" % (os.path.basename(ap), ", ".join(self.o.parts)))
        for part, path in imgs:
            check_magic(part, path)
        names = pit_names(self.r)
        args = ["flash"]
        for part, path in imgs:
            args += ["--" + names.get(part + ".img", part.upper()), path]
        if names:
            args.append("--resume")  # print-pit kept the session open
        if not self.ask("Flash the stock %s from %s?" % (", ".join(p for p, _ in imgs), os.path.basename(ap))):
            return False
        return self.r.write("heimdall", *args)

    def save_logs(self, dest):
        os.makedirs(dest, exist_ok=True)
        ph = self.phone
        saved = []
        if ph.mode in ("fastboot", "fastbootd"):
            p = os.path.join(dest, "getvar-all.txt")
            with open(p, "w") as f:
                f.write(self.r.call("fastboot", "getvar", "all", timeout=45)[1])
            saved.append(p)
        elif ph.mode == "recovery" and ph.fmrre:
            rc, out = self.r.call("adb", "shell", *ph.cli, "rescue", "save", timeout=120)
            m = re.search(r"Logs saved to (\S+)", out)
            with open(os.path.join(dest, "rescue.txt"), "w") as f:
                f.write(out)
            saved.append(os.path.join(dest, "rescue.txt"))
            if m:
                phone_dir = m.group(1).replace("/sdcard/", "/data/media/0/") if m.group(1).startswith("/sdcard/") else m.group(1)
                if self.r.call("adb", "pull", phone_dir, dest, timeout=300)[0] == 0:
                    saved.append(os.path.join(dest, os.path.basename(phone_dir)))
            self.r.call("adb", "pull", "/tmp/recovery.log", dest, timeout=60)
        elif ph.mode == "android":
            p = os.path.join(dest, "logcat.txt")
            with open(p, "w") as f:
                f.write(self.r.call("adb", "logcat", "-d", "-b", "all", timeout=120)[1])
            saved.append(p)
        else:
            raise Refused("No logs to save in this mode (%s)." % MODE_TEXT.get(ph.mode, ph.mode))
        with open(os.path.join(dest, "phone.json"), "w") as f:
            json.dump(ph.to_dict(), f, indent=1)
        self.log("Saved: " + ", ".join(saved))
        return True

    # ---- "fix it for me"
    def fix(self):
        ph = self.refresh()
        self.log("%s - %s" % (ph.name, T(MODE_TEXT.get(ph.mode, ph.mode))))
        m = ph.mode
        if m == "none":
            raise Refused(HOW_TO_CONNECT)
        if self.o.ota and m not in ("unauthorized", "offline"):
            return self.sideload(self.o.ota)  # works on a locked phone too: the recovery checks the signature
        if m == "sideload":
            raise Refused("The phone is waiting for adb sideload: give the OTA or ROM zip - fix --ota <file>.zip (or: sideload <file>.zip).")
        if m in ("unauthorized", "offline", "rescue"):
            raise Refused(diagnose(self.r, ph)[0].detail if m in ("unauthorized", "offline") else
                          "The phone is waiting in %s - cancel it on the phone first." % MODE_TEXT[m])
        self.need_unlocked("repairing")
        if m == "recovery" and ph.fmrre:
            return self.autofix()
        if m == "android":
            if not self.ask("Restart to recovery and run FMRRE's Auto repair there?"):
                return False
            if not self.r.write("adb", "reboot", "recovery"):
                raise Refused("adb couldn't restart the phone.")
            self.wait_fmrre()
            return self.autofix()
        if m == "recovery":  # some other recovery
            if not self.o.recovery:
                raise Refused("This recovery isn't FMRRE. Give an FMRRE image (--recovery fmrre-recovery.img) to start it from "
                              "this computer, or use this recovery's 'Apply update from ADB' with the official firmware.")
            self.to_bootloader()
            m = self.phone.mode
        if m == "download":
            if self.o.recovery:
                if not self.samsung_fmrre(self.o.recovery):
                    return False
                self.log("Waiting for FMRRE (start it with the key combination above)...")
                self.wait_fmrre(max(self.r.wait_s, 180))
                return self.autofix()
            if self.o.ap:
                return self.samsung_stock(self.o.ap)
            raise Refused("In Download mode the tool can:\n"
                          "  - install FMRRE and let it repair the phone:   fix --recovery fmrre-recovery.img\n"
                          "    (build the image from the stock recovery.img in your firmware: scripts/install.sh --build-from recovery.img)\n"
                          "  - flash the stock boot / vbmeta / recovery from your firmware, undoing root or a bad flash:\n"
                          "                                                    fix --ap AP_<model>_....tar.md5\n"
                          "  The whole firmware (BL, AP, CP, CSC) is best flashed with Odin or Smart Switch.")
        # bootloader / fastbootd
        sp = slot_problem(self.phone)
        if sp:
            self.log("Slot %s %s, slot %s booted fine before: going back to slot %s." % (sp[0].upper(), sp[2], sp[1].upper(), sp[1].upper()))
            if self.phone.vars.get("snapshot-update-status") == "snapshotted":
                self.cancel_update()
            if not self.switch_slot():
                return False
            if self.o.reboot:
                self.r.write("fastboot", "reboot")
            self.log("Done. If it still doesn't start, run fix again: the next step is starting FMRRE to repair from the inside.")
            return True
        if self.o.recovery:
            self.boot_fmrre(self.o.recovery)
            return self.autofix()
        # No image: maybe the installed recovery is FMRRE.
        if self.phone.mode == "fastboot" and self.ask("Try starting the recovery installed on the phone (FMRRE, if it is installed)?"):
            if self.r.write("fastboot", "reboot", "recovery") and self.r.wait_for("recovery", min(self.r.wait_s, 60)):
                self.refresh()
                if self.phone.fmrre:
                    return self.autofix()
                raise Refused("The installed recovery isn't FMRRE. Give an FMRRE image (--recovery fmrre-recovery.img) to start it "
                              "from this computer, or use the stock recovery's 'Apply update from ADB' with the official firmware.")
            raise Refused("The phone's recovery didn't start (or this bootloader can't 'reboot recovery'). Give an FMRRE image: "
                          "fix --recovery fmrre-recovery.img")
        raise Refused("Nothing is clearly broken as far as the bootloader can tell. Give an FMRRE image (--recovery) so it can look "
                      "inside, or choose a repair: switch-slot, disable-vbmeta, cancel-update, flash, factory, wipe-data.")


# ---------------------------------------------------------------------------------------------------
# Command line

def color(text, tone):
    if not sys.stdout.isatty() or os.name == "nt" or os.environ.get("NO_COLOR"):
        return text
    return {"ok": "\033[1;32m", "bad": "\033[1;31m", "warn": "\033[1;33m", "head": "\033[1;36m"}.get(tone, "") + text + "\033[0m"


def print_status(ph):
    print(color("FMRRE PC rescue " + VERSION, "head"))
    word, tone, why = lock_verdict(ph)
    for k, v in describe(ph):
        if k == "Bootloader":
            v = color(T(word), tone) + v[len(word):]
        elif k != "Serial":
            v = T(v)
        print("  %-11s %s" % (T(k), v))
    if ph.mode == "none":
        print(T(HOW_TO_CONNECT))
    elif why:
        print("  " + T(why))


def print_findings(fs):
    for f in fs:
        tag = {2: color("[!!]", "bad"), 1: color("[! ]", "warn")}.get(f.level, "[i ]")
        print("%s %s" % (tag, T(f.title)))
        for line in T(f.detail).splitlines():
            print("     " + line)


def main(argv=None):
    def common(p, sub):
        # Accepted before and after the command; the copies on the commands don't override what was given before.
        d = {"default": argparse.SUPPRESS} if sub else {}
        p.add_argument("--serial", **d)
        p.add_argument("--dry-run", action="store_true", help="show the commands that would change the phone, run none of them", **d)
        p.add_argument("-y", "--yes", action="store_true", help="don't ask before each change (ERASE must still be typed)", **d)
        p.add_argument("--unlocked", action="store_true", help="I checked: the bootloader is unlocked (when the phone doesn't say)", **d)
        p.add_argument("--workdir", help="where generated / unpacked images go (default: a temporary folder)", **d)
        p.add_argument("--lang", choices=["en", "ro"], help="language (default: the computer's; ro = Romanian)", **d)
        return p

    ap = common(argparse.ArgumentParser(prog="fmrre_rescue.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter),
                False)
    ap.add_argument("--version", action="version", version=VERSION)
    sub = ap.add_subparsers(dest="cmd")
    _add = sub.add_parser
    sub.add_parser = lambda *x, **k: common(_add(*x, **k), True)
    p = sub.add_parser("status")
    p.add_argument("--json", action="store_true")
    sub.add_parser("diagnose").add_argument("--recovery")
    p = sub.add_parser("fix")
    p.add_argument("--recovery", help="FMRRE image for this phone (started from RAM, or installed in Download mode)")
    p.add_argument("--ap", help="Samsung: the firmware's AP .tar.md5 (stock boot / vbmeta / recovery are flashed from it)")
    p.add_argument("--ota", help="an official full OTA (or a ROM zip) to install with adb sideload - works on a locked phone")
    p.add_argument("--allow-wipe", action="store_true", help="let Auto repair Format Data when it finds that it is needed")
    p.add_argument("--no-reboot", action="store_true")
    sub.add_parser("boot-fmrre").add_argument("image")
    p = sub.add_parser("autofix")
    p.add_argument("--allow-wipe", action="store_true")
    p.add_argument("--no-reboot", action="store_true")
    sub.add_parser("switch-slot")
    sub.add_parser("cancel-update")
    p = sub.add_parser("disable-vbmeta")
    p.add_argument("--vbmeta", help="the stock vbmeta.img (flags set in it); default: a generated empty one")
    p.add_argument("--both-slots", action="store_true")
    p = sub.add_parser("flash")
    p.add_argument("partition")
    p.add_argument("image")
    p = sub.add_parser("samsung-fmrre")
    p.add_argument("image")
    p.add_argument("--vbmeta")
    p = sub.add_parser("samsung-stock")
    p.add_argument("ap")
    p.add_argument("--parts", default=",".join(SAMSUNG_STOCK_PARTS))
    sub.add_parser("sideload", help="install an OTA / ROM zip with adb sideload (official OTAs work on locked phones)").add_argument("zip")
    p = sub.add_parser("factory", help="reinstall the official firmware from a factory image zip (keeps data)")
    p.add_argument("zip")
    p.add_argument("--wipe", action="store_true", help="also factory reset (type ERASE to confirm)")
    sub.add_parser("wipe-data")
    sub.add_parser("reboot").add_argument("target", nargs="?", default="system",
                                          choices=["system", "recovery", "bootloader", "fastboot", "download"])
    sub.add_parser("logs").add_argument("dir", nargs="?", default="")
    sub.add_parser("gui")
    a = ap.parse_args(argv)
    set_language(a.lang or detect_language())
    try:
        sys.stdout.reconfigure(errors="replace")  # diacritics on a console that can't show them
    except (AttributeError, ValueError):
        pass

    if a.cmd in (None, "gui"):
        try:
            import tkinter  # noqa: F401
        except ImportError:
            if a.cmd == "gui":
                print("No tkinter on this computer (Linux: sudo apt install python3-tk) - use the commands, e.g. 'fix'.")
                return 2
            a.cmd = "status"
        else:
            return run_gui(a)

    r = Runner(dry=a.dry_run, serial=a.serial)
    o = Options(yes=a.yes, unlocked=a.unlocked, workdir=a.workdir)
    for k in ("recovery", "ap", "ota", "vbmeta", "both_slots", "allow_wipe"):
        if getattr(a, k, None):
            setattr(o, k, getattr(a, k))
    if getattr(a, "no_reboot", False):
        o.reboot = False
    if getattr(a, "parts", None):
        o.parts = [x.strip() for x in a.parts.split(",") if x.strip()]
    rs = Rescue(r, o)
    ph = rs.refresh()

    if a.cmd == "status":
        if a.json:
            print(json.dumps(ph.to_dict(), indent=1))
        else:
            print_status(ph)
        return 0 if ph.mode != "none" else 3
    if a.cmd == "diagnose":
        print_status(ph)
        print()
        print_findings(diagnose(r, ph, getattr(a, "recovery", None)))
        return 0 if ph.mode != "none" else 3
    try:
        if a.cmd == "fix":
            ok = rs.fix()
        elif a.cmd == "boot-fmrre":
            ok = rs.boot_fmrre(a.image)
        elif a.cmd == "autofix":
            ok = rs.autofix()
        elif a.cmd == "switch-slot":
            ok = rs.switch_slot()
        elif a.cmd == "cancel-update":
            ok = rs.cancel_update()
        elif a.cmd == "disable-vbmeta":
            ok = rs.disable_vbmeta()
        elif a.cmd == "flash":
            ok = rs.flash(a.partition, a.image)
        elif a.cmd == "samsung-fmrre":
            ok = rs.samsung_fmrre(a.image)
        elif a.cmd == "samsung-stock":
            ok = rs.samsung_stock(a.ap)
        elif a.cmd == "sideload":
            ok = rs.sideload(a.zip)
        elif a.cmd == "factory":
            ok = rs.factory(a.zip, a.wipe)
        elif a.cmd == "wipe-data":
            ok = rs.wipe_data()
        elif a.cmd == "reboot":
            ok = rs.reboot(a.target)
        elif a.cmd == "logs":
            ok = rs.save_logs(a.dir or os.path.join(os.getcwd(), "fmrre-logs-" + time.strftime("%Y%m%d-%H%M%S")))
        else:
            ap.print_help()
            return 2
    except Refused as e:
        print(color("xx  ", "bad") + T(str(e)))
        return 3 if ph.mode == "none" else 2
    except KeyboardInterrupt:
        print("\n" + T("cancelled"))
        return 130
    except Exception:  # a bug: say so plainly, keep the details for the report
        import traceback
        print(color("xx  ", "bad") + T("Unexpected error - nothing more was done. Please report it with the details below:"))
        traceback.print_exc(file=sys.stdout)
        return 4
    print(color(T("==> done" if ok else "==> not done"), "ok" if ok else "warn"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------------
# Window

def run_gui(a):
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext, simpledialog, ttk

    root = tk.Tk()
    root.title("FMRRE PC rescue")
    root.geometry("860x760")
    root.minsize(720, 600)
    logq = queue.Queue()
    busy = threading.Event()

    def log(s):
        logq.put(T(str(s)))

    def ask(q):
        ev, box = threading.Event(), {}

        def show():
            box["a"] = messagebox.askyesno("FMRRE PC rescue", q, parent=root)
            ev.set()
        root.after(0, show)
        ev.wait()
        return box.get("a", False)

    def typed(word, q):
        ev, box = threading.Event(), {}

        def show():
            box["a"] = simpledialog.askstring("FMRRE PC rescue", "%s\n\n%s" % (T(q), T("Type %s to continue:" % word)),
                                              parent=root) == word
            ev.set()
        root.after(0, show)
        ev.wait()
        return box.get("a", False)

    r = Runner(log=log, dry=a.dry_run, serial=a.serial)
    o = Options(yes=False, unlocked=a.unlocked, workdir=a.workdir)
    rs = Rescue(r, o, ask=ask, typed=typed)

    style = ttk.Style()
    for t in ("clam", "vista", "aqua"):
        if t in style.theme_names():
            style.theme_use(t)
            break
    style.configure("Head.TLabel", font=("TkDefaultFont", 16, "bold"))
    style.configure("Big.TButton", font=("TkDefaultFont", 12, "bold"), padding=8)

    top = ttk.Frame(root, padding=12)
    top.pack(fill="x")
    name_var, mode_var, lock_var, why_var = tk.StringVar(value=T("Looking for a phone...")), tk.StringVar(), tk.StringVar(), tk.StringVar()
    ttk.Label(top, textvariable=name_var, style="Head.TLabel").grid(row=0, column=0, columnspan=3, sticky="w")
    ttk.Label(top, textvariable=mode_var, wraplength=820, justify="left").grid(row=1, column=0, columnspan=3, sticky="w", pady=(2, 6))
    lock_lbl = tk.Label(top, textvariable=lock_var, font=("TkDefaultFont", 12, "bold"), padx=10, pady=3, fg="white", bg="#64748b")
    lock_lbl.grid(row=2, column=0, sticky="w")
    ttk.Label(top, textvariable=why_var, wraplength=640, justify="left").grid(row=2, column=1, columnspan=2, sticky="w", padx=10)
    unl_var = tk.BooleanVar(value=a.unlocked)
    unl = ttk.Checkbutton(top, text=T("I checked on the phone: the bootloader is unlocked (Samsung: OEM LOCK: OFF)"), variable=unl_var,
                          command=lambda: setattr(o, "unlocked", unl_var.get()))
    top.columnconfigure(2, weight=1)

    mid = ttk.LabelFrame(root, text=T("What was found"), padding=8)
    mid.pack(fill="x", padx=12)
    found = tk.Text(mid, height=8, wrap="word", relief="flat", font=("TkDefaultFont", 10))
    found.pack(fill="x")
    found.tag_configure("bad", foreground="#dc2626", font=("TkDefaultFont", 10, "bold"), lmargin1=6, lmargin2=24)
    found.tag_configure("warn", foreground="#d97706", font=("TkDefaultFont", 10, "bold"), lmargin1=6, lmargin2=24)
    found.tag_configure("info", foreground="#2563eb", font=("TkDefaultFont", 10, "bold"), lmargin1=6, lmargin2=24)
    found.tag_configure("detail", lmargin1=24, lmargin2=24, spacing3=4)

    act = ttk.Frame(root, padding=(12, 8))
    act.pack(fill="x")
    img_var, ap_var = tk.StringVar(), tk.StringVar()
    row0 = ttk.Frame(act)
    row0.pack(fill="x")
    ttk.Label(row0, text=T("FMRRE image:")).pack(side="left")
    ttk.Entry(row0, textvariable=img_var).pack(side="left", fill="x", expand=True, padx=6)
    ttk.Button(row0, text=T("Browse..."), command=lambda: img_var.set(filedialog.askopenfilename(
        title=T("FMRRE image for this phone"), filetypes=[("Images", "*.img"), ("All files", "*")]) or img_var.get())).pack(side="left")
    row0b = ttk.Frame(act)
    row0b.pack(fill="x", pady=(4, 0))
    ttk.Label(row0b, text=T("Samsung AP file:")).pack(side="left")
    ttk.Entry(row0b, textvariable=ap_var).pack(side="left", fill="x", expand=True, padx=6)
    ttk.Button(row0b, text=T("Browse..."), command=lambda: ap_var.set(filedialog.askopenfilename(
        title=T("Samsung firmware AP file"), filetypes=[("Odin AP", "*.tar.md5 *.tar"), ("All files", "*")]) or ap_var.get())).pack(side="left")

    buttons = []

    def job(fn, *args):
        if busy.is_set():
            return

        def work():
            busy.set()
            root.after(0, lambda: [b.state(["disabled"]) for b in buttons])
            try:
                o.recovery = img_var.get().strip() or None
                o.ap = ap_var.get().strip() or None
                rs.refresh()
                ok = fn(*args)
                log("==> done" if ok else "==> not done")
            except Refused as e:
                log("xx  " + T(str(e)))
            except Exception as e:  # keep the window alive, show what happened
                log("xx  %s: %s" % (type(e).__name__, e))
            finally:
                busy.clear()
                root.after(0, lambda: [b.state(["!disabled"]) for b in buttons])
                root.after(0, refresh)
        threading.Thread(target=work, daemon=True).start()

    def btn(parent, text, fn, *args, big=False):
        b = ttk.Button(parent, text=T(text), command=lambda: job(fn, *args), style="Big.TButton" if big else "TButton")
        buttons.append(b)
        return b

    row1 = ttk.Frame(act)
    row1.pack(fill="x", pady=(10, 4))
    btn(row1, "Fix it for me", rs.fix, big=True).pack(side="left")
    btn(row1, "Start FMRRE", lambda: rs.boot_fmrre(img_var.get().strip()) and rs.autofix()).pack(side="left", padx=(10, 0))
    btn(row1, "Auto repair (FMRRE)", rs.autofix).pack(side="left", padx=4)

    def sideload_dialog():
        f = filedialog.askopenfilename(title=T("OTA or ROM zip"), filetypes=[("Zip", "*.zip"), ("All files", "*")])
        if f:
            job(rs.sideload, f)
    sb = ttk.Button(row1, text=T("Sideload OTA..."), command=sideload_dialog)
    sb.pack(side="left", padx=4)
    buttons.append(sb)
    row2 = ttk.Frame(act)
    row2.pack(fill="x", pady=4)
    btn(row2, "Other slot", rs.switch_slot).pack(side="left")
    btn(row2, "Disable vbmeta", rs.disable_vbmeta).pack(side="left", padx=4)
    btn(row2, "Cancel update", rs.cancel_update).pack(side="left")

    def flash_dialog():
        part = simpledialog.askstring(T("Flash an image"), T("Partition (boot, init_boot, vendor_boot, recovery, vbmeta, dtbo...):"),
                                      parent=root)
        if not part:
            return
        f = filedialog.askopenfilename(title=T("Image for " + part), filetypes=[("Images", "*.img"), ("All files", "*")])
        if f:
            job(rs.flash, part.strip(), f)
    fb = ttk.Button(row2, text=T("Flash image..."), command=flash_dialog)
    fb.pack(side="left", padx=4)
    buttons.append(fb)
    btn(row2, "Wipe data...", rs.wipe_data).pack(side="left")

    def factory_dialog():
        f = filedialog.askopenfilename(title=T("Factory image zip for this phone"), filetypes=[("Factory image", "*.zip"), ("All files", "*")])
        if f:
            job(rs.factory, f)
    fa = ttk.Button(row2, text=T("Reinstall firmware..."), command=factory_dialog)
    fa.pack(side="left", padx=4)
    buttons.append(fa)
    row2b = ttk.Frame(act)
    row2b.pack(fill="x", pady=4)
    ttk.Label(row2b, text=T("Samsung:")).pack(side="left")
    btn(row2b, "Install FMRRE", lambda: rs.samsung_fmrre(img_var.get().strip())).pack(side="left", padx=2)
    btn(row2b, "Stock boot / vbmeta / recovery from AP", lambda: rs.samsung_stock(ap_var.get().strip())).pack(side="left", padx=2)
    row3 = ttk.Frame(act)
    row3.pack(fill="x", pady=4)
    ttk.Label(row3, text=T("Restart to:")).pack(side="left")
    for t in ("system", "recovery", "bootloader", "fastboot", "download"):
        btn(row3, t.capitalize(), rs.reboot, t).pack(side="left", padx=2)  # btn() translates the label

    def logs_dialog():
        d = filedialog.askdirectory(title=T("Save the phone's logs to"))
        if d:
            job(rs.save_logs, os.path.join(d, "fmrre-logs-" + time.strftime("%Y%m%d-%H%M%S")))
    lb = ttk.Button(row3, text=T("Save logs..."), command=logs_dialog)
    lb.pack(side="right")
    buttons.append(lb)

    out = scrolledtext.ScrolledText(root, height=14, font=("Courier", 10), background="#0f172a", foreground="#e2e8f0",
                                    insertbackground="#e2e8f0")
    out.pack(fill="both", expand=True, padx=12, pady=(4, 12))

    state = {"sig": None}

    def show(ph, fs):
        name_var.set(ph.name if ph.mode != "none" else T("No phone found"))
        bits = [T(MODE_TEXT.get(ph.mode, ph.mode))]
        if ph.serial:
            bits.append(T("serial") + " " + ph.serial)
        if ph.android:
            bits.append("Android " + ph.android)
        if ph.slot:
            bits.append(T("slot") + " " + ph.slot.upper() + slot_health(ph))
        mode_var.set("   ".join(bits))
        word, tone, why = lock_verdict(ph)
        lock_var.set(T("Bootloader: " + word) if ph.mode != "none" else T("Plug the phone in"))
        lock_lbl.configure(bg={"ok": "#16a34a", "bad": "#dc2626"}.get(tone, "#d97706"))
        why_var.set(T(why) if ph.mode != "none" else "")
        if ph.locked is None and ph.mode not in ("none", "unauthorized", "offline"):
            unl.grid(row=3, column=0, columnspan=3, sticky="w", pady=(6, 0))
        else:
            unl.grid_forget()
        found.configure(state="normal")
        found.delete("1.0", "end")
        for f in fs:
            tag = {2: "bad", 1: "warn"}.get(f.level, "info")
            found.insert("end", {2: "\u25cf ", 1: "\u25b2 "}.get(f.level, "\u2139 ") + T(f.title) + "\n", tag)
            found.insert("end", T(f.detail) + "\n", "detail")
        found.configure(state="disabled")

    def refresh():
        if busy.is_set():
            return

        def work():
            ph = detect(Runner(log=lambda s: None, serial=a.serial))
            sig = json.dumps(ph.to_dict(), sort_keys=True)
            if sig != state["sig"]:
                state["sig"] = sig
                fs = diagnose(Runner(log=lambda s: None, serial=a.serial), ph, img_var.get().strip() or None) \
                    if ph.mode != "recovery" else diagnose(r, ph)
                root.after(0, lambda: show(ph, fs))
        threading.Thread(target=work, daemon=True).start()

    def tick():
        while True:
            try:
                line = logq.get_nowait()
            except queue.Empty:
                break
            out.insert("end", line + "\n")
            out.see("end")
        root.after(100, tick)

    def poll():
        refresh()
        root.after(4000, poll)

    ttk.Button(top, text=T("Refresh"), command=lambda: (state.update(sig=None), refresh())).grid(row=0, column=2, sticky="e")
    log("FMRRE PC rescue %s - tools: %s" % (VERSION, ", ".join("%s %s" % (k, "ok" if v else "MISSING") for k, v in r.tools.items())))
    for k, v in r.tools.items():
        if not v:
            log("   %s: %s" % (k, INSTALL_HINT[k]))
    tick()
    poll()
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
