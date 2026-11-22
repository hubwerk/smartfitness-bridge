#!/usr/bin/env python3
"""Build the English web walkthrough from offline UI fixtures and original audio.

Install scripts/requirements-video.txt and `python -m playwright install chromium`.
Run `python scripts/web_howto.py`; no bridge, network credentials or music downloads
are used. The procedural soundtrack is dedicated under CC0-1.0.
"""

import argparse
from copy import deepcopy
import io
import json
from pathlib import Path
import subprocess
import tempfile
from urllib.parse import parse_qs, urlparse
import wave

import imageio_ffmpeg
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from playwright.sync_api import expect, sync_playwright

from embed_web_assets import web_content
from web_screenshots import CONFIG, PAIRED, SCAN, STATUS, WIFI_SCAN


ROOT = Path(__file__).resolve().parents[1]
SIZE = (1920, 1080)
FPS = 25
SECONDS = 7.5  # Four bars at 128 BPM.
BG, WHITE, MUTED, BLUE = "#07111d", "#f1f6fc", "#9bb5ca", "#53b9ff"
FONT = ROOT / "assets/fonts/Antonio-Variable.ttf"

# Each scene has a chapter, title, instruction, visual, and short takeaway.
SCENES = [
    ("START", "Your equipment.\nYour training app.",
     "Set up SmartFitness Bridge, pair your fitness device, and start training.",
     "flow", "WEB EDITION  /  ENGLISH WALKTHROUGH"),
    ("PREPARE", "Get ready",
     "Use an ESP32-S3 DevKitC-1 N16R8, a USB data cable, and supported BLE equipment.",
     "kit", "No extra radio is needed for Bluetooth FTMS output."),
    ("PREPARE", "Install the firmware",
     "Install Git and PlatformIO Core. Connect the board by USB, then run these commands.",
     "flash", "Already flashed? Continue with the Wi-Fi setup."),
    ("CONNECT", "Join the bridge Wi-Fi",
     "Power the bridge by USB. On your phone or computer, join its access point.",
     "wifi", "Keep this connection even if it has no internet access."),
    ("CONNECT", "Open the web interface",
     "Open https://192.168.4.1/ while connected to the bridge's access point.",
     "https", "A self-signed certificate causes a browser warning."),
    ("SECURE", "Set your own password",
     "Open System. Replace the default access point password before normal use.",
     "password", "Use a private, trusted network. The web interface has no login."),
    ("NETWORK", "Home Wi-Fi is optional",
     "To join your home network, click Scan and select your Wi-Fi network.",
     "wifi-scan", "You can also leave the SSID empty and use access-point mode."),
    ("NETWORK", "Save and restart",
     "Enter your Wi-Fi password. Click Save & restart and confirm the restart.",
     "wifi-save", "Restarting interrupts the browser, device and app connections."),
    ("NETWORK", "Reconnect your browser",
     "Join the same home Wi-Fi. Open https://smartfitness.local/ or the bridge's router-assigned IP address.",
     "rejoin", "Access-point mode: reconnect to the bridge and use 192.168.4.1."),
    ("PAIR", "Wake your equipment",
     "Wake the fitness device. Disconnect any other app that is using its Bluetooth connection.",
     "wake", "Keep the bridge close to your equipment while pairing."),
    ("PAIR", "Find your device",
     "Open Connections, choose Add device, then click Scan.",
     "scan", "This scan finds Bluetooth fitness equipment, not Wi-Fi networks."),
    ("PAIR", "Choose and pair",
     "Find your device in the results. Check the adapter, then click Pair.",
     "pair", "If no adapter is detected, choose the supported adapter manually."),
    ("PAIR", "Check the input link",
     "Wait for the fitness device to show a ready connection. Then check Training for measurements.",
     "paired", "Demo device and measurements are simulated throughout this video."),
    ("OUTPUT", "Choose Bluetooth FTMS",
     "Under Bridge output, choose Bluetooth FTMS. Keep the device profile on Automatic unless you need an override.",
     "output", "Input: your fitness device. Output: what your training app receives."),
    ("OUTPUT", "Save your output",
     "Check Name for apps, then click Save output. Look for this name when pairing in your training app.",
     "output-save", "The example bridge name is SmartFitness."),
    ("TRAIN", "Pair the app to the bridge",
     "In your app's device or sensor pairing screen, select SmartFitness rather than the original equipment.",
     "app", "Pairing screens vary by app. This is a connection diagram, not an app recording."),
    ("TRAIN", "Confirm the receiver",
     "Back in Connections, check App / display. A connection and control ownership are separate states.",
     "receiver", "Start the workout from your training app."),
    ("TRAIN", "Watch your measurements",
     "Open Training. Check power, cadence and the other measurements supported by your device.",
     "training", "Missing measurements depend on the equipment and its protocol."),
    ("TRAIN", "Adjust supported targets",
     "Usually, your app controls training. For manual control, choose a target, enter a value and click Apply.",
     "control", "Only supported controls with fresh device data are available."),
    ("TRAIN", "Wait for confirmation",
     "Check the confirmed target and command result. A value you enter is not proof that the device accepted it.",
     "confirmed", "Example only: use targets appropriate for your equipment and workout."),
    ("OPTIONAL", "Use ANT+ FE-C instead",
     "Connect a supported USB ANT+ stick to the USB host port. Choose ANT+ FE-C and Save output.",
     "ant", "Web variant only. Bluetooth FTMS and ANT+ FE-C are alternative outputs."),
    ("RECONNECT", "Ready for next time",
     "The bridge remembers its paired input and reconnects after reboot. Wake the equipment and reconnect your app.",
     "reconnect", "Disconnect ends the link. Forget pairing removes the saved input."),
    ("SAFETY", "Stay in control",
     "The bridge never starts treadmill belts. Treadmill controls require an already-running device and fresh data.",
     "safety", "Use the equipment's physical safety controls. Never expose the web UI publicly."),
    ("FINISH", "Connect. Train. Repeat.",
     "No data? Check equipment power, distance and competing apps. Confirm the input link and selected output.",
     "finish", "Compatibility and setup: github.com/hubwerk/smartfitness-bridge"),
]


def capture_ui(folder):
    """Exercise real controls; mock only the transport, never the UI's DOM state."""
    status, config = deepcopy(STATUS), deepcopy(CONFIG)
    status.update(state="disconnected", deviceName="", canControl=False, telemetry={})
    status["input"].update(paired=None, link="idle", autoConnect=False)
    status["ftmsOutput"].update(client=False, control=False, clientAddress="")
    for service in status["ftmsOutput"]["services"]:
        service["active"] = False
    config.update(wifiSsid="", wifiPasswordSet=False)
    scan = {**deepcopy(SCAN), "devices": []}
    sockets, errors, commands, shots = [], [], [], {}
    results = {}

    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context(viewport={"width": 1100, "height": 1000},
                                      device_scale_factor=2, reduced_motion="reduce")
        page = context.new_page()
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.on("dialog", lambda dialog: dialog.accept())

        def publish():
            for socket in sockets:
                socket.send(json.dumps(status))
                socket.send(json.dumps(scan))

        def websocket(socket):
            sockets.append(socket)
            publish()

        def route(r):
            url, request = urlparse(r.request.url), r.request
            if url.netloc != "bridge.test":
                raise AssertionError(f"Unexpected external request: {request.url}")
            if url.path == "/":
                r.fulfill(body=web_content(), content_type="text/html")
            elif request.method == "POST":
                body = request.post_data_json or {}
                commands.append(url.path)
                if url.path == "/api/wifi-scan":
                    r.fulfill(status=202, json={"scanning": True})
                    return
                if url.path == "/api/config":
                    config.update({k: v for k, v in body.items()
                                   if k not in ("save", "apPassword", "wifiPassword")})
                    if "apPassword" in body:
                        config["apPasswordDefault"] = False
                    if "wifiPassword" in body:
                        config["wifiPasswordSet"] = True
                    status["outputProtocol"] = config["outputProtocol"]
                    ant = config["outputProtocol"] == "ant-fec"
                    status["ftmsOutput"]["enabled"] = not ant
                    status["antOutput"].update(enabled=ant, stick=ant, open=ant,
                                               link="open" if ant else "disabled")
                    for channel in status["antOutput"]["channels"]:
                        channel["open"] = ant and channel["enabled"] and channel["available"]
                    for service in status["ftmsOutput"]["services"]:
                        service["active"] = not ant and service["enabled"] and service["available"]
                elif url.path == "/api/scan":
                    scan["devices"] = deepcopy(SCAN["devices"][:1])
                elif url.path == "/api/pair":
                    assert body["address"] == PAIRED["address"]
                    status.update(state="control-ready", deviceName="Training bike",
                                  canControl=True, telemetry=deepcopy(STATUS["telemetry"]))
                    paired = deepcopy(PAIRED)
                    paired["subscriptions"] = paired["subscriptions"][:2]
                    for subscription in paired["subscriptions"]:
                        subscription["state"] = "ok"
                    status["input"].update(paired=paired, link="ready", autoConnect=True)
                    for service in status["ftmsOutput"]["services"]:
                        service["active"] = service["enabled"] and service["available"]
                elif url.path == "/api/control":
                    status["target"] = body
                elif url.path == "/api/restart":
                    status["network"].update(mode="sta", ssid=config["wifiSsid"])
                else:
                    raise AssertionError(f"Unexpected command: {url.path}")
                ident = len(results) + 1
                results[ident] = {"type": "result", "id": ident, "done": True,
                                  "ok": True, "message": "applied"}
                publish()
                r.fulfill(status=202, json={"id": ident})
            else:
                fixtures = {"/api/status": status, "/api/config": config,
                            "/api/scan/results": scan, "/api/wifi-scan/results": WIFI_SCAN}
                if url.path == "/api/result":
                    r.fulfill(json=results[int(parse_qs(url.query)["id"][0])])
                else:
                    assert url.path in fixtures, request.url
                    r.fulfill(json=fixtures[url.path])

        context.route("**/*", route)
        context.route_web_socket("**/ws", websocket)
        page.goto("https://bridge.test")
        expect(page.locator("#conn")).to_contain_text("online")

        def shot(name, selector, focus=None):
            expect(page.locator("#toast")).to_be_hidden()
            publish()
            page.wait_for_timeout(100)
            element = page.locator(selector)
            element.scroll_into_view_if_needed()
            box = element.bounding_box()
            highlight = page.locator(focus).bounding_box() if focus else None
            image = Image.open(io.BytesIO(element.screenshot())).convert("RGB")
            path = folder / f"{name}.png"
            image.save(path)
            rect = None
            if highlight:
                rect = tuple(2 * value for value in (
                    highlight["x"] - box["x"], highlight["y"] - box["y"],
                    highlight["width"], highlight["height"]))
            shots[name] = (path, rect)

        page.locator("#tab-set").click()
        page.locator("#c-apPassword").fill("Demo-only-passphrase")
        shot("password", "#network-settings", "#c-apPassword")
        page.locator("#btn-wifi-scan").click()
        expect(page.locator("#wifi-list")).to_be_visible()
        shot("wifi-scan", "#network-settings", "#wifi-list")
        page.get_by_role("option", name="Training room", exact=False).click()
        page.locator("#c-wifiPassword").fill("Demo-only-network")
        expect(page.locator("#btn-save")).to_have_text("Save & restart")
        shot("wifi-save", "#network-settings", "#btn-save")
        page.locator("#btn-save").click()
        expect(page.locator("#system-dirty")).to_contain_text("restarts and joins")

        page.locator("#tab-dev").click()
        page.locator("#btn-change-device").click()
        shot("scan", "#card-input", "#btn-scan")
        page.locator("#btn-scan").click()
        expect(page.get_by_role("button", name="Pair Training bike", exact=True)).to_be_visible()
        shot("pair", "#card-input", "#scan-body tr:first-child")
        page.get_by_role("button", name="Pair Training bike", exact=True).click()
        expect(page.locator("#input-state")).to_have_text("ready")
        page.locator("#device-search summary").click()
        shot("paired", "#card-input", "#input-state")
        shot("output", "#card-bridge", "#c-outputProtocol-button")
        shot("output-save", "#card-bridge", "#btn-output")
        page.locator("#btn-output").click()
        expect(page.locator("#output-dirty")).to_have_text("No unsaved edits")

        status["ftmsOutput"].update(client=True, control=True,
                                    clientAddress="5a:12:34:56:78:9a")
        publish()
        shot("receiver", "#card-receiver", "#r-state")
        page.locator("#tab-dash").click()
        expect(page.locator("#m-power")).to_have_text("214")
        shot("training", ".live-card")
        page.locator("#ctl-value").fill("230")
        shot("control", "#ctl", "#btn-apply")
        page.locator("#btn-apply").click()
        expect(page.locator("#confirmed-target")).to_contain_text("230")
        expect(page.locator("#ctl-result")).to_have_text("applied")
        shot("confirmed", "#ctl", "#confirmed-target")

        page.locator("#tab-dev").click()
        page.locator("#c-outputProtocol-button").click()
        page.get_by_role("option", name="ANT+ FE-C", exact=True).click()
        page.locator("#btn-output").click()
        expect(page.locator("#output-dirty")).to_have_text("No unsaved edits")
        shot("ant", "#card-bridge", "#ant-services")
        page.locator("#input-details summary").click()
        shot("reconnect", "#card-input", "#btn-unpair")
        assert not errors, errors
        assert {"/api/pair", "/api/control", "/api/restart", "/api/config"} <= set(commands)
        browser.close()
    return shots


def font(size):
    return ImageFont.truetype(str(FONT), size)


def text(draw, xy, value, size, color=WHITE, width=490, gap=12):
    """Wrap by measured glyph width, failing if a word cannot fit."""
    x, y = xy
    face = font(size)
    for paragraph in value.split("\n"):
        line = ""
        for word in paragraph.split():
            assert draw.textlength(word, font=face) <= width, word
            candidate = f"{line} {word}".strip()
            if draw.textlength(candidate, font=face) > width:
                draw.text((x, y), line, font=face, fill=color)
                y += size + gap
                line = word
            else:
                line = candidate
        draw.text((x, y), line, font=face, fill=color)
        y += size + gap
    return y


CARDS = {
    "flow": [("01  FITNESS DEVICE", "Bluetooth Low Energy input"),
             ("02  SMARTFITNESS BRIDGE", "Translate supported measurements"),
             ("03  APP / DISPLAY", "Bluetooth FTMS or optional ANT+ FE-C")],
    "kit": [("ESP32-S3 DEVKITC-1", "N16R8: 16 MB flash / 8 MB PSRAM"),
            ("USB DATA CABLE", "Used for flashing and power"),
            ("SUPPORTED EQUIPMENT", "Check the repository's device guide first")],
    "flash": [("INSTALL PLATFORMIO CORE", "python -m pip install -U platformio"),
              ("GET THE SOURCE", "git clone https://github.com/hubwerk/smartfitness-bridge.git\ncd smartfitness-bridge"),
              ("FLASH THE WEB VARIANT", "pio run -e esp32s3dev -t upload")],
    "wifi": [("WI-FI NETWORK", "SmartFitness-XXXX"),
             ("INITIAL PASSWORD", "smartfitness"),
             ("IMPORTANT", "Change this default password before normal use")],
    "https": [("LOCAL WEB ADDRESS", "https://192.168.4.1/"),
              ("BROWSER WARNING", "Self-signed certificate: expected on your own bridge"),
              ("PROCEED ONLY AFTER CHECKING", "You are on your bridge's Wi-Fi and using its local address")],
    "rejoin": [("HOME WI-FI", "https://smartfitness.local/"),
               ("IF THE NAME DOES NOT RESOLVE", "Find the bridge's IP in your router's device list"),
               ("IF WI-FI SETUP FAILS", "The bridge access point returns after about 20 seconds")],
    "wake": [("WAKE THE FITNESS DEVICE", "Power on / wake it according to its manual"),
             ("RELEASE ITS BLUETOOTH LINK", "Disconnect other apps from the original equipment"),
             ("BRING THE BRIDGE CLOSE", "Then scan from Connections in the web interface")],
    "app": [("FITNESS DEVICE", "Paired to the bridge, not directly to the training app"),
            ("SMARTFITNESS", "Choose this advertised name in your app"),
            ("TRAINING APP / DISPLAY", "Follow the app's instructions to pair and start a workout")],
    "safety": [("NO REMOTE BELT START", "Start treadmills only with their own controls"),
               ("FRESH DATA REQUIRED", "Do not rely on stale or disconnected measurements"),
               ("PRIVATE NETWORK ONLY", "Anyone on the bridge's Wi-Fi network can control it")],
    "finish": [("FULL GUIDE & COMPATIBILITY", "github.com/hubwerk/smartfitness-bridge"),
               ("ILLUSTRATED WALKTHROUGH", "Real web UI / offline fixtures / no hardware demonstration"),
               ("ORIGINAL 128 BPM SOUNDTRACK", "Procedural synthesis / no third-party samples / CC0-1.0")],
}


def slide(index, scene, shots):
    chapter, title, body, visual, takeaway = scene
    image = Image.new("RGB", SIZE, BG)
    draw = ImageDraw.Draw(image)
    for y in range(1080):
        glow = max(0, 1 - y / 1000)
        draw.line((0, y, 1919, y), fill=(7 + int(4 * glow), 17 + int(13 * glow),
                                                   29 + int(21 * glow)))
    text(draw, (64, 34), "SMARTFITNESS / BRIDGE", 28, BLUE, width=700)
    text(draw, (1330, 38), "WEB HOW-TO  |  SIMULATED DEMO", 23, MUTED, width=520)
    draw.line((64, 95, 1856, 95), fill="#2b465d", width=2)
    text(draw, (64, 140), f"{index + 1:02d} / {len(SCENES):02d}   {chapter}", 27, BLUE)
    y = text(draw, (64, 215), title, 64, width=500, gap=10)
    y = text(draw, (64, y + 40), body, 35, width=480, gap=15)
    assert y < 850, (index, "Caption overflow")
    draw.rounded_rectangle((610, 137, 1856, 938), radius=26, fill="#0d1e2e",
                           outline="#2b465d", width=2)
    if visual in shots:
        path, rect = shots[visual]
        with Image.open(path) as source:
            scale = min(1170 / source.width, 734 / source.height)
            resized = source.resize((round(source.width * scale), round(source.height * scale)),
                                    Image.Resampling.LANCZOS)
        x, y = 1233 - resized.width // 2, 537 - resized.height // 2
        image.paste(resized, (x, y))
        if rect:
            rx, ry, rw, rh = rect
            draw.rounded_rectangle((x + rx * scale - 5, y + ry * scale - 5,
                                    x + (rx + rw) * scale + 5, y + (ry + rh) * scale + 5),
                                   radius=10, outline=BLUE, width=4)
    else:
        for row, (heading, detail) in enumerate(CARDS[visual]):
            y = 193 + row * 244
            text(draw, (656, y), heading, 31, BLUE, width=1140)
            end = text(draw, (656, y + 58), detail, 37, width=1140, gap=16)
            assert end < y + 220, (visual, "Card overflow")
            if row < 2:
                draw.line((656, y + 211, 1810, y + 211), fill="#2b465d", width=2)
    text(draw, (64, 975), takeaway, 27, MUTED, width=1760)
    return image


def soundtrack(path, duration):
    """Original four-on-the-floor arrangement; all instruments are oscillators/noise."""
    rate, beat = 44100, 60 / 128
    count = round(duration * rate)
    mix = np.zeros((count, 2), dtype=np.float32)
    rng = np.random.default_rng(128)

    def add(sound, start, gain=1, pan=0):
        offset = round(start * rate)
        n = min(len(sound), count - offset)
        if n > 0:
            mix[offset:offset + n, 0] += sound[:n] * gain * (1 - pan * .35)
            mix[offset:offset + n, 1] += sound[:n] * gain * (1 + pan * .35)

    def note(midi, length, decay):
        t = np.arange(round(length * rate)) / rate
        frequency = 440 * 2 ** ((midi - 69) / 12)
        signal = sum(np.sin(2 * np.pi * frequency * harmonic * t) / harmonic ** 1.8
                     for harmonic in range(1, 6))
        return signal * np.minimum(t / .008, 1) * np.exp(-t / decay)

    roots = [40, 36, 43, 38]
    beats = round(duration / beat)
    for b in range(beats):
        start, bar = b * beat, b // 4
        root = roots[(bar // 2) % 4]
        full = 8 <= bar < 88
        breakdown = 48 <= bar < 56
        t = np.arange(round(.38 * rate)) / rate
        kick = np.sin(2 * np.pi * (48 * t + 105 * .022 * (1 - np.exp(-t / .022))))
        kick *= np.exp(-t / .09)
        if not breakdown or b % 4 == 0:
            add(kick, start, .7 if full else .45)
        if b % 2 == 1:
            t = np.arange(round(.18 * rate)) / rate
            noise = rng.normal(0, 1, len(t))
            clap = np.diff(noise, prepend=0) * np.exp(-t / .035)
            add(clap, start, .095 if full else .04)
        for sub in range(4 if full else 2):
            t = np.arange(round(.065 * rate)) / rate
            noise = rng.normal(0, 1, len(t))
            hat = np.diff(noise, prepend=0) * np.exp(-t / .012)
            add(hat, start + sub * beat / (4 if full else 2), .032, (-1) ** sub * .6)
        for sub in range(2):
            add(note(root, beat * .46, .13), start + sub * beat / 2 + .035,
                .22 if full else .12)
        chord = [root + 24, root + (27 if root in (40, 36) else 28), root + 31]
        if b % 4 == 0:
            for pitch in chord:
                pad = note(pitch, beat * 4, 1.1)
                time = np.arange(len(pad)) / rate
                pump = .25 + .75 * np.minimum((time % beat) / .18, 1)
                add(pad * pump, start, .075, (pitch % 3 - 1) * .5)
        if full and not breakdown:
            melody = [0, 7, 12, 7, 3, 7, 10, 7]
            pitch = root + 36 + melody[b % 8]
            pluck = note(pitch, .32, .075)
            add(pluck, start + beat / 2, .085, .4)
            add(pluck, start + beat * 1.25, .025, -.6)
    fade = min(round(2 * rate), count // 2)
    mix[:fade] *= np.linspace(0, 1, fade)[:, None]
    mix[-fade:] *= np.linspace(1, 0, fade)[:, None]
    mix *= .82 / np.max(np.abs(mix))
    assert np.isfinite(mix).all() and np.max(np.abs(mix)) < 1
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes((mix * 32767).astype("<i2").tobytes())


def build(output, previews=None):
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    with tempfile.TemporaryDirectory(prefix="smartfitness-video-") as temporary:
        folder = Path(temporary)
        shots = capture_ui(folder)
        frames = [slide(i, scene, shots) for i, scene in enumerate(SCENES)]
        if previews:
            previews.mkdir(parents=True, exist_ok=True)
            for i, frame in enumerate(frames):
                frame.save(previews / f"{i + 1:02d}.jpg")
        duration = len(SCENES) * SECONDS
        soundtrack(folder / "music.wav", duration)
        metadata = [";FFMETADATA1", "title=SmartFitness Bridge - Web How-To",
                    "comment=Offline UI demonstration. Original procedural music: CC0-1.0."]
        for i, scene in enumerate(SCENES):
            metadata += ["[CHAPTER]", "TIMEBASE=1/1000",
                         f"START={round(i * SECONDS * 1000)}",
                         f"END={round((i + 1) * SECONDS * 1000)}",
                         f"title={scene[1].replace(chr(10), ' ')}"]
        (folder / "chapters.txt").write_text("\n".join(metadata), encoding="utf-8")
        writer = imageio_ffmpeg.write_frames(
            str(folder / "picture.mp4"), SIZE, fps=FPS, codec="libx264",
            pix_fmt_out="yuv420p", macro_block_size=1, ffmpeg_log_level="error",
            output_params=["-crf", "21", "-preset", "fast", "-threads", "2"])
        writer.send(None)
        try:
            total = round(duration * FPS)
            for n in range(total):
                scene_index = min(int(n / FPS / SECONDS), len(frames) - 1)
                frame = frames[scene_index].copy()
                draw = ImageDraw.Draw(frame)
                draw.rectangle((0, 1068, round(SIZE[0] * (n + 1) / total), 1079), fill=BLUE)
                writer.send(frame.tobytes())
        finally:
            writer.close()
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            ffmpeg, "-y", "-v", "error", "-i", str(folder / "picture.mp4"),
            "-i", str(folder / "music.wav"), "-i", str(folder / "chapters.txt"),
            "-map", "0:v:0", "-map", "1:a:0", "-map_metadata", "2", "-map_chapters", "2",
            "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
            "-af", "loudnorm=I=-16:TP=-1.5:LRA=9", "-ar", "48000",
            "-metadata:s:a:0", "language=eng", "-movflags", "+faststart", str(output),
        ], check=True)
        subprocess.run([ffmpeg, "-v", "error", "-xerror", "-i", str(output),
                        "-f", "null", "-"], check=True)
        print(f"Created {output}: {duration:.0f}s, 1920x1080, {FPS} fps, stereo AAC")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/web-howto.mp4")
    parser.add_argument("--previews", type=Path, help="Optional folder for review images")
    args = parser.parse_args()
    build(args.output.resolve(), args.previews)
