#!/usr/bin/env python3
"""Render offline web UI fixtures: pip install -r scripts/requirements-screenshots.txt

Install Chromium with `python -m playwright install --with-deps chromium`, then run
`python3 scripts/web_screenshots.py --output docs/hmi/web`.
Fixtures follow src/variants/web/web_json.cpp and web_api.cpp; no firmware is needed.
"""

import argparse
import base64
import gzip
import json
import re
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from playwright.sync_api import expect, sync_playwright
from embed_web_assets import web_content


ROOT = Path(__file__).resolve().parents[1]
ASSET = ROOT / "src/variants/web/assets/index.html"
ORIGIN = "http://bridge.test"
PAIRED = {
    "name": "Training bike", "address": "aa:bb:cc:dd:ee:01",
    "addressType": "public", "adapter": "ftms",
    "advertisedServices": [
        "1826", "180d", "12345678-1234-5678-9abc-def012345678",
    ],
    "advertisedServiceNames": {"1826": "FTMS", "180d": "HR",
                              "12345678-1234-5678-9abc-def012345678": "12345678..."},
    "subscriptions": [
        {"service": "00001826-0000-1000-8000-00805f9b34fb",
         "characteristic": "00002ad2-0000-1000-8000-00805f9b34fb",
         "characteristicName": "Indoor Bike Data", "name": "FTMS",
         "state": "ok", "indicate": False, "reason": ""},
        {"service": "00001826-0000-1000-8000-00805f9b34fb",
         "characteristic": "00002ad9-0000-1000-8000-00805f9b34fb",
         "characteristicName": "Fitness Machine Control Point", "name": "FTMS",
         "state": "pending", "indicate": True, "reason": ""},
        {"service": "0000180d-0000-1000-8000-00805f9b34fb",
         "characteristic": "00002a37-0000-1000-8000-00805f9b34fb",
         "characteristicName": "Heart Rate Measurement", "name": "HR",
         "state": "failed", "indicate": False, "reason": "not-found"},
    ],
}
# Mirrors outputCharacteristics() in include/core/bridge/output_gatt.hpp (bike profile).
GATT = {
    "ftms": ("1826", [("2acc", "Fitness Machine Feature", "read"), ("2ad2", "Indoor Bike Data", "notify"),
                      ("2ad3", "Training Status", "read/notify"),
                      ("2ad5", "Supported Inclination Range", "read"),
                      ("2ad4", "Supported Speed Range", "read"),
                      ("2ad6", "Supported Resistance Level Range", "read"),
                      ("2ad8", "Supported Power Range", "read"),
                      ("2ad9", "Fitness Machine Control Point", "write/indicate"),
                      ("2ada", "Fitness Machine Status", "notify")]),
    "cps": ("1818", [("2a63", "Cycling Power Measurement", "notify"), ("2a65", "Cycling Power Feature", "read"),
                     ("2a5d", "Sensor Location", "read")]),
    "hr": ("180d", [("2a37", "Heart Rate Measurement", "notify")]),
    "csc": ("1816", [("2a5b", "CSC Measurement", "notify"), ("2a5c", "CSC Feature", "read"),
                     ("2a5d", "Sensor Location", "read"), ("2a55", "SC Control Point", "write/indicate")]),
    "rsc": ("1814", [("2a53", "RSC Measurement", "notify"), ("2a54", "RSC Feature", "read")]),
}
BLE_SERVICES = [
    {"id": ident, "name": name, "enabled": enabled, "available": available, "active": active,
     "uuid": GATT[ident][0],
     "characteristics": [{"uuid": uuid, "name": char_name, "properties": props}
                         for uuid, char_name, props in GATT[ident][1]]}
    for ident, name, enabled, available, active in [
        ("ftms", "Fitness Machine (FTMS)", True, True, True), ("cps", "Cycling Power (CPS)", True, True, False),
        ("hr", "Heart Rate", True, True, True), ("csc", "Cycling Speed/Cadence (CSC)", False, True, False),
        ("rsc", "Running Speed/Cadence (RSC)", False, False, False)]
]
ANT_CHANNELS = [
    {"id": ident, "name": name, "deviceType": device_type, "enabled": enabled, "open": False,
     "available": ident != "sdm"}
    for ident, name, device_type, enabled in [
        ("fec", "FE-C", 17, True), ("power", "Power", 11, False), ("hr", "HR", 120, True),
        ("speed-cadence", "Speed / cadence", 121, False), ("sdm", "Footpod", 124, False)]
]
STATUS = {
    "type": "status", "version": "1.0.0", "state": "control-ready",
    "telemetry": {"power": 214, "cadence": 86, "speed": 32.4,
                  "heartRate": 142, "incline": 2.5, "distance": 12400, "elapsed": 1380},
    "deviceName": "Training bike", "trainer": True, "canControl": True,
    "controls": ["power", "resistance", "incline", "speed"],
    "target": {"control": "power", "value": 225},
    "input": {"source": "ble", "profile": "bike", "bleAvailable": True,
              "link": "ready", "protocol": "ftms", "stale": False, "scanning": False,
              "error": "", "reconnectAttempts": 0, "nextAttemptMs": 0,
              "autoConnect": True, "paired": PAIRED,
              "dropped": {"scans": 0, "events": 0, "notifications": 0, "commands": 0}},
    "ftmsOutput": {"enabled": True, "profile": "bike", "override": "auto",
                   "name": "SmartFitness", "advertising": True, "client": True, "control": True,
                   "clientAddress": "5a:12:34:56:78:9a", "clientAddressType": "random",
                   "restartRequired": False, "services": BLE_SERVICES},
    "outputProtocol": "ble-ftms",
    "antOutput": {"enabled": False, "available": True, "link": "disabled", "stick": False, "open": False,
                  "deviceNumber": 4660, "profile": "bike", "display": False, "control": False,
                  "hardware": "USB 0fcf:1008", "channels": ANT_CHANNELS, "error": "", "channelLimit": 0},
    "unsaved": False,
    "network": {"mode": "sta", "ssid": "Training room", "ip": "192.0.2.42",
                "hostname": "smartfitness", "stations": 0},
}
SCAN = {
    "type": "scan", "scanning": False, "bleAvailable": True, "dropped": 0,
    "devices": [
        {**PAIRED, "rssi": -48, "connectable": True, "services": ["0x1826"]},
        {"name": "Unknown peripheral", "address": "aa:bb:cc:dd:ee:02",
         "addressType": "random", "adapter": "", "rssi": -67,
         "connectable": True, "services": ["0xff00"]},
    ],
    "adapters": [
        {"id": "ftms", "label": "FTMS fitness machine", "controllable": True, "advertisementOnly": False},
        {"id": "hrs", "label": "Heart rate monitor", "controllable": False, "advertisementOnly": False},
    ],
}
CONFIG = {
    "type": "config", "version": "1.0.0", "ftmsName": "SmartFitness", "outputProtocol": "ble-ftms",
    "ftmsOutputProfile": "auto", "ftmsInput": True, "antAvailable": True,
    "ftmsOutputServices": 7, "antOutputServices": 5,
    "simulatedTrainer": True, "wifiSsid": "Training room",
    "wifiPasswordSet": True, "apPasswordDefault": True, "https": True, "unsaved": False,
}
WIFI_SCAN = {
    "type": "wifi-scan", "scanning": False,
    "networks": [{"ssid": "Training room", "rssi": -45, "secure": True},
                 {"ssid": "Guest", "rssi": -71, "secure": False}],
}


def failure_status():
    failed = json.loads(json.dumps(STATUS))
    failed["state"] = "disconnected"
    failed["canControl"] = False
    failed["input"].update(link="backoff", stale=True, error="connection lost", nextAttemptMs=3000)
    return failed


def check_view(page, panel, mobile):
    expect(page.locator(f"#{panel}")).to_be_visible()
    expect(page.locator('[role="tab"][aria-selected="true"]')).to_have_attribute("aria-controls", panel)
    assert page.locator('[role="tabpanel"]:visible').count() == 1
    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), f"Overflow: {panel}"
    if panel == "dash":
        assert page.locator("#device-icon path, #device-icon rect, #device-icon circle").count() > 1
    if not mobile:
        # The first content tile starts level with the first navigation item.
        first = page.evaluate(f"""() => {{
            const nav=document.querySelector('nav button').getBoundingClientRect().top+scrollY;
            const card=document.querySelector('#{panel} .card, #{panel} .chain').getBoundingClientRect().top+scrollY;
            return [nav, card];}}""")
        assert abs(first[0] - first[1]) <= 1, (panel, first)
    if panel == "dash" and page.evaluate("innerWidth > 1150"):
        heights = page.evaluate("""() => ['.live-card','.control-card'].map(s =>
            document.querySelector(s).getBoundingClientRect().height)""")
        assert abs(heights[0] - heights[1]) <= 1, heights


def render(browser, output, name, width, height):
    context = browser.new_context(viewport={"width": width, "height": height},
                                  device_scale_factor=1, reduced_motion="reduce")
    page = context.new_page()
    errors, commands, sockets, results, external = [], [], [], {}, []
    config = dict(CONFIG)
    reject_control = True
    page.on("pageerror", lambda error: errors.append(str(error)))

    def websocket(ws):
        sockets.append(ws)
        ws.send(json.dumps(STATUS))
        ws.send(json.dumps(SCAN))

    def route(request_route):
        request = request_route.request
        url = urlparse(request.url)
        if f"{url.scheme}://{url.netloc}" != ORIGIN:
            external.append(request.url)
            request_route.abort()
        elif url.path == "/":
            request_route.fulfill(body=web_content(), content_type="text/html")
        elif url.path == "/api/wifi-scan":
            request_route.fulfill(status=202, json={"scanning": True})
        elif url.path == "/api/wifi-scan/results":
            request_route.fulfill(json=WIFI_SCAN)
        elif request.method == "POST":
            endpoint = url.path.removeprefix("/api/")
            body = request.post_data_json
            commands.append((endpoint, body))
            if endpoint == "config":
                config.update({k: v for k, v in body.items() if k != "save"})
            ident = len(commands)
            result = {"type": "result", "id": ident, "done": True,
                      "ok": not (endpoint == "control" and reject_control),
                      "message": "control rejected" if endpoint == "control" and reject_control else "applied"}
            results[ident] = result
            request_route.fulfill(status=202, json={"id": ident})
        elif url.path == "/api/result":
            request_route.fulfill(json=results[int(parse_qs(url.query)["id"][0])])
        else:
            fixtures = {"/api/status": STATUS, "/api/config": config, "/api/scan/results": SCAN}
            assert url.path in fixtures, request.url
            request_route.fulfill(json=fixtures[url.path])

    context.route_web_socket("**/ws", websocket)
    context.route("**/*", route)
    page.goto(ORIGIN)
    expect(page.locator("#m-power")).to_have_text("214")
    expect(page.locator("#c-ftmsName")).to_have_value("SmartFitness")
    assert "/debug/log" not in page.content()
    assert page.locator('[role="tab"]').count() == 3
    assert page.evaluate("""() => {const ids=[...document.querySelectorAll('[id]')].map(e=>e.id);
        return ids.length===new Set(ids).size}"""), "Duplicate IDs"
    page.locator("#tab-dash").focus()
    for key, panel in [("ArrowRight", "dev"), ("End", "set"), ("Home", "dash"),
                       ("ArrowLeft", "set"), ("ArrowRight", "dash")]:
        page.keyboard.press(key)
        check_view(page, panel, width < 700)
        expect(page.locator("#tab-" + panel)).to_be_focused()
    assert page.locator('[role="tab"][tabindex="0"]').count() == 1
    page.keyboard.press("Tab")
    expect(page.locator("#dash")).to_be_focused()

    def push(value):
        sockets[0].send(json.dumps(value))
        page.wait_for_function("s => status.deviceName === s.deviceName && status.input.profile === s.input.profile", arg=value)

    mappings = {
        "bike": ("power", "cadence", "distance"),
        "treadmill": ("speed", "incline", "distance"),
        "rower": ("cadence", "power", "distance"),
        "cross-trainer": ("resistance", "cadence", "distance"),
        "step-climber": ("cadence", "positiveElevationGain", "stepCount"),
        "stair-climber": ("cadence", "floors", "stepCount"),
    }
    for profile, keys in mappings.items():
        sample = json.loads(json.dumps(STATUS))
        sample["deviceName"] = profile
        sample["input"]["profile"] = sample["ftmsOutput"]["profile"] = profile
        sample["telemetry"].update(resistance=18, totalEnergy=243, pace=122, stepCount=720,
                                   floors=24, positiveElevationGain=192, averageCadence=46)
        if profile == "rower":
            sample["telemetry"]["cadence"] = 24
        if "climber" in profile:
            sample["telemetry"]["cadence"] = 64
        push(sample)
        expect(page.locator("#primary-reading .v")).to_have_attribute("id", "m-" + keys[0])
        expect(page.locator("#secondary-left .v")).to_have_attribute("id", "m-" + keys[1])
        expect(page.locator("#secondary-right .v")).to_have_attribute("id", "m-" + keys[2])
        for key in keys:
            assert page.locator("#m-" + key).count() == 1
            assert page.locator("#metrics #m-" + key).count() == 0
        assert page.locator("#metrics .metric").count() == 4
        assert page.evaluate("history.length <= 1"), "History leaked across devices"
        page.evaluate("""sample => {
            const realNow=Date.now, end=realNow(), key=activeProfile.main;
            try {
                history=[]; lastHistory=0;
                for(let i=360;i>=0;i--) {
                    Date.now=()=>end-i*1000;
                    const next=structuredClone(sample);
                    next.telemetry[key]=sample.telemetry[key]*(1+Math.sin(i/9)*.08);
                    renderStatus(next);
                }
            } finally { Date.now=realNow; }
            if(history.length!==301) throw new Error("History must be bounded to five minutes");
            renderHistory();
            document.activeElement.blur(); window.scrollTo(0,0);
        }""", sample)
        check_view(page, "dash", width < 700)
        page.screenshot(path=str(output / f"training-{profile}-{name}.png"), full_page=True)
    # Unknown profiles must not silently become an indoor bike.
    unknown = json.loads(json.dumps(STATUS))
    unknown["input"]["profile"] = unknown["ftmsOutput"]["profile"] = "future-device"
    unknown["deviceName"] = "unknown"
    push(unknown)
    expect(page.locator("#training-alert")).to_contain_text("generic")
    sim = json.loads(json.dumps(STATUS))
    sim["input"]["source"] = "simulation"
    push(sim)
    expect(page.locator("#training-alert")).to_contain_text("Simulation")
    push(STATUS)

    missing = json.loads(json.dumps(STATUS))
    missing["telemetry"] = {"cadence": 0}
    sockets[0].send(json.dumps(missing))
    expect(page.locator("#m-power")).to_have_text("–")
    expect(page.locator("#m-cadence")).to_have_text("0")
    assert page.locator("#primary-arc").evaluate("e => getComputedStyle(e).visibility") == "hidden"
    sockets[0].send(json.dumps(failure_status()))
    expect(page.locator("#m-cadence")).to_have_text("–")
    expect(page.locator("#btn-apply")).to_be_disabled()
    expect(page.locator("#training-alert")).to_contain_text("connection lost")
    push(STATUS)
    expect(page.locator("#btn-apply")).to_be_enabled()
    page.evaluate("lastStatus = Date.now() - 7000; renderTraining()")
    expect(page.locator("#m-power")).to_have_text("–")
    expect(page.locator("#btn-apply")).to_be_disabled()
    page.evaluate("renderStatus(" + json.dumps(STATUS) + ")")
    page.evaluate("""() => {
        const now=Date.now();
        history=[
            {time:now-7000,value:210,target:225},
            {time:now-6000,value:211,target:225},
            {time:now-5000,value:null,target:null},
            {time:now-2000,value:212,target:225},
            {time:now-1000,value:214,target:225}
        ];
        renderHistory();
    }""")
    assert page.locator("#history-chart .series").get_attribute("d").count("M") == 2
    assert page.locator("#history-chart .target").get_attribute("d").count("M") == 2
    # A percentage resistance target is not comparable to a device's raw level.
    cross = json.loads(json.dumps(STATUS))
    cross["input"]["profile"] = cross["ftmsOutput"]["profile"] = "cross-trainer"
    cross["target"] = {"control": "resistance", "value": 25}
    push(cross)
    expect(page.locator("#history-target-legend")).to_be_hidden()
    push(STATUS)
    expect(page.locator("#ctl-value")).to_have_value("225")

    def command(selector, endpoint):
        before = len(commands)
        with page.expect_response(lambda r: r.request.method == "POST" and
                                  urlparse(r.url).path == "/api/" + endpoint):
            page.locator(selector).click()
        page.wait_for_function("Object.keys(pending).length === 0 && !configSaving && !controlBusy")
        assert len(commands) == before + 1, (selector, commands)
        assert commands[-1][0] == endpoint

    page.locator("#ctl-value").fill("")
    page.locator("#btn-apply").click()
    expect(page.locator("#toast")).to_have_text("enter a number")
    assert not commands
    page.locator("#ctl-value").fill("220")
    command("#btn-apply", "control")
    expect(page.locator("#ctl-result")).to_have_text("control rejected")
    expect(page.locator("#confirmed-target")).to_contain_text("225 W")
    assert commands[-1] == ("control", {"control": "power", "value": 220})
    reject_control = False
    command("#btn-apply", "control")
    expect(page.locator("#ctl-result")).to_have_text("applied")
    # The confirmed target is sourced from device status, never the unsent input.
    expect(page.locator("#confirmed-target")).to_contain_text("225 W")
    page.locator("#ctl-kind").select_option("speed")
    expect(page.locator("#ctl-value")).to_have_value("")
    page.locator("#ctl-value").fill("12")
    before = len(commands)
    page.once("dialog", lambda dialog: dialog.dismiss())
    page.locator("#btn-apply").click()
    assert len(commands) == before
    page.once("dialog", lambda dialog: dialog.accept())
    command("#btn-apply", "control")
    assert commands[-1] == ("control", {"control": "speed", "value": 12})
    # Capability gating, without inventing pause/stop endpoints.
    limited = json.loads(json.dumps(STATUS))
    limited["controls"] = ["resistance"]
    sockets[0].send(json.dumps(limited))
    expect(page.locator("#ctl-kind")).to_have_value("resistance")
    expect(page.locator('#ctl-kind option[value="speed"]')).to_have_attribute("disabled", "")
    sockets[0].send(json.dumps(STATUS))
    page.locator("#tab-dev").click()
    check_view(page, "dev", width < 700)
    expect(page.locator("#device-search")).not_to_have_attribute("open", "")
    page.locator("#btn-change-device").click()
    expect(page.locator("#device-search")).to_have_attribute("open", "")
    command("#btn-scan", "scan")
    assert commands[-1] == ("scan", {"seconds": 30})
    command("#btn-scan-stop", "scan")
    assert commands[-1] == ("scan", {"action": "stop"})
    page.get_by_label("Pair Unknown peripheral", exact=True).click()
    expect(page.locator("#toast")).to_have_text("choose an adapter first")
    page.get_by_label("Pair Training bike", exact=True).click()
    page.wait_for_function("Object.keys(pending).length === 0")
    assert commands[-1] == ("pair", {"address": PAIRED["address"], "addressType": "public", "adapter": "ftms"})
    page.locator("#input-details summary").click()
    expect(page.locator("#paired-subscriptions .bad")).to_contain_text("not-found")
    before = len(commands)
    page.once("dialog", lambda dialog: dialog.dismiss())
    page.locator("#btn-unpair").click()
    assert len(commands) == before
    page.once("dialog", lambda dialog: dialog.accept())
    command("#btn-unpair", "unpair")
    page.locator("#device-search summary").click()
    page.locator("#input-details summary").click()

    # Output changes survive tab switches, focus loss, and WebSocket config pushes.
    page.locator("#c-ftmsName").fill("My bike")
    page.locator("#c-ftmsOutputProfile").select_option("rower")
    page.locator('#c-ftmsServices [data-service="cps"]').click()
    expect(page.locator("#c-ftmsRestart")).to_contain_text("Save + restart to apply")
    page.locator("#tab-set").click()
    page.locator("#c-wifiSsid").fill("New network")
    page.locator("#tab-dev").click()
    page.evaluate("document.activeElement.blur()")
    sockets[0].send(json.dumps(CONFIG))
    expect(page.locator("#c-ftmsName")).to_have_value("My bike")
    expect(page.locator("#c-wifiSsid")).to_have_value("New network")
    command("#btn-output", "config")
    assert commands[-1] == ("config", {"outputProtocol": "ble-ftms", "ftmsName": "My bike",
                                      "ftmsOutputProfile": "rower", "ftmsOutputServices": 5,
                                      "antOutputServices": 5, "save": True})
    expect(page.locator("#c-wifiSsid")).to_have_value("New network")
    page.locator("#c-outputProtocol").select_option("ant-fec")
    expect(page.locator("#ble-services")).to_be_hidden()
    expect(page.locator("#ant-services")).to_be_visible()
    page.locator('#c-antServices [data-service="power"]').click()
    expect(page.locator("#c-antWarning")).to_be_visible()
    page.locator("#btn-discard-output").click()
    expect(page.locator("#c-outputProtocol")).to_have_value("ble-ftms")
    page.locator("#toast").evaluate("e => e.style.display='none'")
    page.evaluate("document.activeElement.blur(); window.scrollTo(0,0)")
    page.screenshot(path=str(output / f"connections-{name}.png"), full_page=True)
    page.locator("#tab-set").click()
    check_view(page, "set", width < 700)
    expect(page.locator("#advanced")).not_to_have_attribute("open", "")
    page.locator("#c-https").uncheck()
    command("#btn-save", "config")
    assert commands[-1] == ("config", {"simulatedTrainer": True, "wifiSsid": "New network",
                                      "https": False, "save": True})
    page.locator("#btn-wifi-scan").click()
    expect(page.locator("#wifi-list")).to_be_visible(timeout=10000)
    page.locator("#c-wifiSsid").press("ArrowDown")
    page.locator("#c-wifiSsid").press("Enter")
    expect(page.locator("#c-wifiSsid")).to_have_value("Training room")
    expect(page.locator("#system-dirty")).to_have_text("Unsaved edits")
    command("#btn-save", "config")
    before = len(commands)
    page.once("dialog", lambda dialog: dialog.dismiss())
    page.locator("#btn-restart").click()
    assert len(commands) == before
    page.once("dialog", lambda dialog: dialog.accept())
    command("#btn-restart", "restart")
    dialogs = []
    def cancel_second(dialog):
        dialogs.append(dialog.message)
        dialog.accept() if len(dialogs) == 1 else dialog.dismiss()
    page.on("dialog", cancel_second)
    before = len(commands)
    page.locator("#btn-reset").click()
    assert len(dialogs) == 2 and len(commands) == before
    page.remove_listener("dialog", cancel_second)
    page.locator("#toast").evaluate("e => e.style.display='none'")
    page.evaluate("document.activeElement.blur(); window.scrollTo(0,0)")
    page.screenshot(path=str(output / f"system-{name}.png"), full_page=True)
    page.locator("#advanced summary").click()
    page.once("dialog", lambda dialog: dialog.accept())
    command("#btn-sim", "simulate")
    assert not errors, errors
    assert not external, external
    context.close()
    print(f"{name}: six profiles, responsive layout, truthful telemetry, capabilities, confirmations,"
          " config edit preservation, services, pairing and network actions passed")


def check_debug(browser, output, name, width, height):
    context = browser.new_context(viewport={"width": width, "height": height},
                                  device_scale_factor=1, reduced_motion="reduce",
                                  accept_downloads=True)
    page = context.new_page()
    sockets, requests, errors = [], [], []
    page.on("pageerror", lambda error: errors.append(str(error)))
    history = b"[Boot] ready\n"
    end = len(history)
    during_load = None
    logging_enabled = True
    logging_posts = []

    def push(data, start, reset=False):
        sockets[0].send(json.dumps({"type": "debug", "start": start,
                                   "end": start + len(data), "reset": reset,
                                   "data": base64.b64encode(data).decode()}))

    def websocket(ws):
        sockets.append(ws)
        ws.send(json.dumps(STATUS))

    def route(request_route):
        nonlocal during_load, logging_enabled
        path = urlparse(request_route.request.url).path
        requests.append(path)
        if path == "/debug/logging":
            assert request_route.request.method == "POST"
            assert request_route.request.headers["content-type"] == "application/json"
            body = json.loads(request_route.request.post_data)
            logging_posts.append(body)
            logging_enabled = body["enabled"]
            request_route.fulfill(json={"enabled": logging_enabled})
        elif path == "/":
            request_route.fulfill(body=web_content(True), content_type="text/html")
        elif path == "/debug/log":
            if during_load:
                push(during_load, end)
                during_load = None
            request_route.fulfill(body=history, content_type="text/plain; charset=utf-8",
                                  headers={"X-Debug-End": str(end),
                                           "X-Debug-Logging": "on" if logging_enabled else "off",
                                           "Content-Disposition": 'attachment; filename="debug.log"'})
        else:
            fixtures = {"/api/status": STATUS, "/api/scan/results": SCAN, "/api/config": CONFIG}
            assert path in fixtures, f"Unexpected debug request: {path}"
            request_route.fulfill(json=fixtures[path])

    context.route_web_socket("**/ws", websocket)
    context.route("**/*", route)
    page.goto(ORIGIN)
    expect(page.locator("#m-power")).to_have_text("214")
    assert "/debug/log" not in requests, "History must load only when opening the tab"
    during_load = b"[Live] during history request\n"
    page.locator("#tab-set").click()
    page.locator("#advanced").evaluate("e => e.open = true")
    box = page.locator("#debug-output")
    expected = history.decode() + "[Live] during history request\n"
    expect(box).to_have_value(expected)
    end = len(expected.encode())
    page.wait_for_timeout(350)
    assert requests.count("/debug/log") == 1, "Debug history must not be polled"
    check_view(page, "set", width < 700)
    payload = '<script>window.__unsafe=true</script> ä\n'.encode()
    push(payload[:-2], end)
    push(payload[-2:], end + len(payload) - 2)
    expected += payload.decode()
    expect(box).to_have_value(expected)
    assert page.evaluate("window.__unsafe") is None, "Log output must be plain text"
    push(payload, end)  # duplicate delivery must not append twice
    expect(box).to_have_value(expected)
    end += len(payload)
    # A partial overlap (history completed while a frame was in flight).
    push(payload + b"overlap\n", end - len(payload))
    expected += "overlap\n"
    expect(box).to_have_value(expected)
    end += len(b"overlap\n")
    push(b"[Log] rotated\n", end + 100, reset=True)
    expect(box).to_have_value("[Log] rotated\n")
    end += 100 + len(b"[Log] rotated\n")
    # The browser may already have loaded part of the rotated file as history.
    push(b"[Log] rotated\ncontinued\n", end - len(b"[Log] rotated\n"), reset=True)
    expect(box).to_have_value("[Log] rotated\ncontinued\n")
    end += len(b"continued\n")
    history = b"[Log] recovered after slow client\n"
    end += len(history) + 100
    push(b"gap\n", end - 4)
    expect(box).to_have_value(history.decode())
    assert requests.count("/debug/log") == 2, "A missed frame must resync once"
    with page.expect_download() as download:
        page.locator("#btn-debug-download").click()
    assert download.value.suggested_filename == "debug.log"
    assert Path(download.value.path()).read_bytes() == history
    push(b"line\n" * 150, end)
    expect(box).to_have_value(history.decode() + "line\n" * 150)
    assert box.evaluate("e => e.scrollTop + e.clientHeight >= e.scrollHeight - 2")
    page.screenshot(path=str(output / f"05-debug-{name}.png"), full_page=True)
    page.locator("#btn-debug-clear").click()
    expect(box).to_have_value("")
    push(b"after clear\n", end + 750)
    expect(box).to_have_value("after clear\n")
    page.locator("#tab-dash").click()
    page.locator("#tab-set").click()
    page.locator("#advanced").evaluate("e => e.open = true")
    expect(box).to_have_value(history.decode())
    toggle, state = page.locator("#btn-debug-logging"), page.locator("#debug-state")
    expect(toggle).to_have_text("Logging stoppen")
    expect(toggle).to_have_attribute("aria-pressed", "false")
    toggle.click()
    expect(toggle).to_have_text("Logging fortsetzen")
    expect(toggle).to_have_attribute("aria-pressed", "true")
    expect(state).to_contain_text("Logging gestoppt")
    assert logging_posts == [{"enabled": False}]
    page.locator("#tab-dash").click()
    page.locator("#tab-set").click()
    page.locator("#advanced").evaluate("e => e.open = true")  # history reload keeps the stopped state
    expect(box).to_have_value(history.decode())
    expect(toggle).to_have_text("Logging fortsetzen")
    expect(state).to_contain_text("Logging gestoppt")
    toggle.click()
    expect(toggle).to_have_text("Logging stoppen")
    expect(state).to_have_text("")
    assert logging_posts == [{"enabled": False}, {"enabled": True}]
    assert not errors, f"Debug browser errors: {errors}"
    context.close()
    print(f"{name}: debug gating, history race, push/UTF-8/plain text, deduplication,"
          " rotation, gap recovery, download, stop/resume logging, clear, scroll and no polling passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", "--output-dir", dest="output_dir", type=Path,
                        default=ROOT / "docs/hmi/web",
                        help="Screenshot destination (default: docs/hmi/web)")
    args = parser.parse_args()
    normal, debug = web_content(), web_content(True)
    assert "DEBUG_WEB" not in normal and "DEBUG_WEB" not in debug
    for marker in ("/debug/log", "debug-output", "renderDebug", "btn-debug-logging"):
        assert marker not in normal and marker in debug, f"Conditional asset leak: {marker}"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        for name, width, height in [("desktop", 1280, 800), ("mobile", 390, 844)]:
            render(browser, args.output_dir, name, width, height)
            check_debug(browser, args.output_dir, name, width, height)
        browser.close()
    content = ASSET.read_bytes()
    label = "Web asset"
    print(f"{label}: {len(content)} bytes; gzip: {len(gzip.compress(content, mtime=0))} bytes")


if __name__ == "__main__":
    main()
