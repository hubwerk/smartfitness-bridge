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


def render_before(browser, asset, output, name, width, height):
    context = browser.new_context(viewport={"width": width, "height": height},
                                  device_scale_factor=1, reduced_motion="reduce")
    page = context.new_page()
    external, errors = [], []
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.on("websocket", lambda ws: external.append(ws.url)
            if ws.url != "ws://bridge.test/ws" else None)

    def websocket(ws):
        ws.send(json.dumps(failure_status()))
        ws.send(json.dumps(SCAN))

    def route(request_route):
        url = urlparse(request_route.request.url)
        if f"{url.scheme}://{url.netloc}" != ORIGIN:
            external.append(request_route.request.url)
            request_route.abort()
        elif url.path == "/":
            request_route.fulfill(body=asset.read_text(), content_type="text/html")
        else:
            fixtures = {"/api/status": failure_status(), "/api/scan/results": SCAN,
                        "/api/config": CONFIG}
            assert url.path in fixtures, f"Unexpected baseline request: {url.path}"
            request_route.fulfill(json=fixtures[url.path])

    context.route_web_socket("**/ws", websocket)
    context.route("**/*", route)
    page.goto(ORIGIN)
    expect(page.locator("#m-power")).to_have_text("214")
    assert page.locator("#metrics .metric").count() == len(STATUS["telemetry"])
    assert page.locator("#m-target").count() == 0
    expect(page.locator("#s-input .bad")).to_have_text("connection lost")
    expect(page.locator("#dash")).to_be_visible()
    assert page.evaluate("document.documentElement.scrollWidth <= window.innerWidth")
    page.screenshot(path=str(output / f"web-dashboard-{name}.png"), full_page=True)
    assert not external, f"External baseline requests: {external}"
    assert not errors, f"Baseline browser errors: {errors}"
    context.close()
    print(f"before {name}: same dashboard fixtures; offline assets and no overflow passed")


def check_view(page, panel, mobile):
    expect(page.locator(f"#{panel}")).to_be_visible()
    expect(page.locator('[role="tab"][aria-selected="true"]')).to_have_attribute(
        "aria-controls", panel)
    assert page.locator('[role="tabpanel"]:visible').count() == 1
    assert page.evaluate(
        "document.documentElement.scrollWidth <= window.innerWidth"
    ), f"Horizontal overflow in {'mobile' if mobile else 'desktop'} {panel}"
    legends = {
        "Web connection": ["Web interface online", "Web interface offline"],
        "Bridge connection": [
            "Connected / ready / active", "Disconnected / error",
            "Retry backoff / stale data / missing ANT+ stick",
            "Connecting / scanning / advertising / inactive / info"],
        "Bluetooth service": ["Advertised service", "Subscribed / OK",
                              "Pending / none reported", "Subscription failed"],
        "Bluetooth scan": ["Unrelated device / unnamed device"],
        "Bridge output": ["Service / ANT+ channel", "Sending / subscribed / channel open",
                          "Pending / not applied"],
        "Receiving device": ["Connected / has control", "Waiting / no control / output off"],
        "Control result": ["Command accepted", "Command failed / disconnected / rejected"],
        "Output service": ["Service active / channel open / channel capacity",
                           "Pending / off", "Failed / error / duplicate power warning"],
    }
    names = ["Web connection"] + {
        "dash": ["Bridge connection"], "dev": ["Bluetooth service", "Bridge output", "Receiving device", "Bluetooth scan"],
        "ctl": ["Control result"], "set": ["Output service"], "debug": [],
    }[panel]
    expect(page.locator(".status-legend:visible")).to_have_count(len(names))
    for name in names:
        legend = page.get_by_role("list", name=f"{name} color legend", exact=True)
        expect(legend).to_be_visible()
        expect(legend.locator("li")).to_have_text(legends[name])
        expect(legend.locator('.legend-swatch[aria-hidden="true"]')).to_have_count(
            len(legends[name]))


def check_legend_colors(page):
    references = [
        (".pill.ok", "#conn", "backgroundColor"),
        (".pill.bad", "#s-input .bad", "backgroundColor"),
        (".pill.warn", "#s-input .warn", "backgroundColor"),
        (".pill.pending", "#paired-subscriptions .pending", "backgroundColor"),
        (".pill.sensor", "#paired-advertised .sensor", "backgroundColor"),
        (".gray", "#scan-body .gray", "color"),
        (".pill:not(.ok):not(.bad):not(.warn):not(.pending):not(.sensor)",
         "#s-input .pill", "backgroundColor"),
    ]
    # A theme change must affect real indicators and legend samples together.
    for theme in ({}, {"--ok": "#123456", "--bad": "#654321", "--warn": "#abcdef",
                       "--tan": "#fedcba", "--violet": "#456789"}):
        page.evaluate("""theme => {
            for (const [key, value] of Object.entries(theme))
                document.documentElement.style.setProperty(key, value);
        }""", theme)
        for selector, reference, prop in references:
            color = page.locator(reference).first.evaluate(
                "(e, prop) => getComputedStyle(e)[prop]", prop)
            assert page.locator(".status-legend .legend-swatch" + selector).evaluate_all(
                "(elements, color) => elements.length > 0 && elements.every("
                "e => getComputedStyle(e).backgroundColor === color)", color
            ), f"Legend color mismatch for {selector}"
    page.evaluate("""() => {
        for (const key of ['--ok', '--bad', '--warn', '--tan', '--violet'])
            document.documentElement.style.removeProperty(key);
    }""")


def render(browser, output, name, width, height):
    context = browser.new_context(viewport={"width": width, "height": height},
                                  device_scale_factor=1, reduced_motion="reduce")
    page = context.new_page()
    errors, external, commands, sockets, results, wifi_scans, wifi_polls = [], [], [], [], {}, [], []
    config = dict(CONFIG)  # saved settings change what /api/config returns
    page.on("pageerror", lambda error: errors.append(str(error)))
    page.on("websocket", lambda ws: external.append(ws.url)
            if ws.url != "ws://bridge.test/ws" else None)

    def websocket(ws):
        sockets.append(ws)
        ws.send(json.dumps(STATUS))
        ws.send(json.dumps(SCAN))

    context.route_web_socket("**/ws", websocket)

    def route(request_route):
        request = request_route.request
        url = urlparse(request.url)
        if f"{url.scheme}://{url.netloc}" != ORIGIN:
            external.append(request.url)
            request_route.abort()
            return
        if url.path == "/":
            request_route.fulfill(body=web_content(), content_type="text/html")
            return
        if request.method == "POST" and url.path == "/api/wifi-scan":
            wifi_scans.append(request.post_data_json)
            request_route.fulfill(status=202, json={"scanning": True})
            return
        if url.path == "/api/wifi-scan/results" and len(wifi_polls) == 0:
            # The scan takes the radio off-channel; a dropped poll must not abort it.
            wifi_polls.append("aborted")
            request_route.abort()
            return
        if request.method == "POST":
            endpoint = url.path.removeprefix("/api/")
            commands.append((endpoint, request.post_data_json))
            if endpoint == "config" and "https" in request.post_data_json:
                config["https"] = request.post_data_json["https"]
            if endpoint == "config":
                for key in ("ftmsOutputServices", "antOutputServices"):
                    if key in request.post_data_json:
                        config[key] = request.post_data_json[key]
            ident = len(commands)
            result = {"type": "result", "id": ident, "done": True,
                      "ok": endpoint != "control",
                      "message": "control: disconnected" if endpoint == "control" else "applied"}
            results[ident] = result
            request_route.fulfill(status=202, json={"id": ident})
            # Exercise the existing WebSocket completion path as well as polling.
            if endpoint == "connect":
                page.evaluate(
                    "(result) => setTimeout(() => window.__fixtureResult(result), 50)", result)
            return
        fixtures = {"/api/status": STATUS, "/api/scan/results": SCAN, "/api/config": config,
                    "/api/wifi-scan/results": WIFI_SCAN}
        if url.path == "/api/result":
            request_route.fulfill(json=results[int(parse_qs(url.query)["id"][0])])
        elif url.path in fixtures:
            request_route.fulfill(json=fixtures[url.path])
        else:
            raise AssertionError(f"Unexpected API request: {request.method} {request.url}")

    context.route("**/*", route)
    # A server-side WebSocket send is scheduled from Python via this binding.
    page.expose_function("__fixtureResult", lambda result: sockets[0].send(json.dumps(result)))
    page.goto(ORIGIN)
    page.evaluate("document.fonts.ready")
    expect(page.locator("#m-power")).to_have_text("214")
    assert page.locator("#tab-debug").count() == 0
    assert "/debug/log" not in page.content()
    expect(page.locator("#power-gauge")).to_have_attribute("value", "214")
    expect(page.locator("#s-input .ok")).to_have_text("link: ready")
    expect(page.locator("#s-bridge")).to_contain_text("control ready")
    expect(page.locator("#ctl-state")).to_contain_text("supported: power, resistance, incline, speed")
    assert page.locator("#s-input").get_by_text("stale data", exact=True).count() == 0
    assert sockets, "WebSocket fixture did not connect"
    assert page.evaluate("document.fonts.size") == 1, "Missing embedded font face"
    assert page.evaluate("document.fonts.check('18px Antonio')"), "Embedded font did not load"
    assert page.evaluate("[...document.fonts][0].status") == "loaded"
    expect(page.locator(".lcars-rail")).to_have_attribute("aria-hidden", "true")
    assert page.evaluate("""() => [...document.querySelectorAll('*')].every(element =>
        ['', '::before', '::after'].every(pseudo =>
            getComputedStyle(element, pseudo || null).backgroundImage === 'none'))"""), \
        "Decorative backgrounds must use flat solid shapes"
    page.evaluate("""() => {
        window.__completed = [];
        const originalFinish = finish;
        finish = result => { originalFinish(result); window.__completed.push(result.id); };
    }""")

    def command(button, endpoint):
        with page.expect_response(
            lambda response: response.request.method == "POST"
            and urlparse(response.url).path == "/api/" + endpoint
        ) as response:
            button.click()
        ident = response.value.json()["id"]
        page.wait_for_function("id => window.__completed.includes(id)", arg=ident)

    # Roving tab focus, wrapping arrows and Home/End automatic activation.
    tab = page.locator("#tab-dash")
    tab.focus()
    assert tab.evaluate("e => getComputedStyle(e).outlineColor") == "rgb(255, 255, 255)"
    for key, panel in [("ArrowRight", "dev"), ("End", "set"), ("Home", "dash"),
                       ("ArrowLeft", "set"), ("ArrowRight", "dash")]:
        page.keyboard.press(key)
        check_view(page, panel, width < 650)
        expect(page.locator(f"#tab-{panel}")).to_be_focused()
    assert page.locator('[role="tab"][tabindex="0"]').count() == 1
    page.keyboard.press("Tab")
    expect(page.locator("#dash")).to_be_focused()

    searching = json.loads(json.dumps(STATUS))
    searching["input"]["link"] = "searching"
    sockets[0].send(json.dumps(searching))
    expect(page.locator("#s-input .pending")).to_have_text("link: searching")
    check_view(page, "dash", width < 650)

    reduced = json.loads(json.dumps(STATUS))
    reduced["telemetry"] = {"cadence": 86}
    sockets[0].send(json.dumps(reduced))
    expect(page.locator("#metrics .metric")).to_have_count(1)
    expect(page.locator("#m-cadence")).to_have_text("86")
    assert page.locator("#m-power, #m-heartRate").count() == 0
    empty = json.loads(json.dumps(STATUS))
    empty["telemetry"] = {}
    sockets[0].send(json.dumps(empty))
    expect(page.locator("#metrics .metric")).to_have_count(0)
    expect(page.locator("#metrics-empty")).to_be_visible()
    sockets[0].send(json.dumps(STATUS))
    expect(page.locator("#metrics .metric")).to_have_count(len(STATUS["telemetry"]))

    # Status failure rendering is driven by the real WebSocket status contract.
    sockets[0].send(json.dumps(failure_status()))
    expect(page.locator("#s-input .bad")).to_have_text("connection lost")
    expect(page.locator("#s-bridge .bad")).to_have_text("disconnected")
    expect(page.locator("#s-input .warn")).to_have_text(["link: backoff", "stale data"])
    expect(page.locator("#s-input")).to_contain_text("retry in 3 s")
    expect(page.locator("#s-bridge")).to_contain_text("no control")
    expect(page.locator("#ctl-state")).to_contain_text("control not available")
    assert page.locator("#s-input .bad").evaluate(
        "e => getComputedStyle(e).backgroundColor") == "rgb(204, 68, 68)"
    check_legend_colors(page)

    panel_names = {"dash": "dashboard", "dev": "devices", "ctl": "control", "set": "settings"}
    for number, panel in enumerate(panel_names, start=1 if name == "desktop" else 5):
        page.locator(f"#tab-{panel}").click()
        check_view(page, panel, width < 650)
        if panel == "dash":
            expect(page.locator("#s-out .pill").first).to_have_text("BLE FTMS")
            expect(page.locator("#s-out")).to_contain_text("app connected")
        if panel == "dev":
            expect(page.locator("#paired-advertised .sensor")).to_have_count(3)
            expect(page.locator("#paired-advertised .sensor").first).to_have_attribute(
                "title", "00001826-0000-1000-8000-00805f9b34fb")
            expect(page.locator("#paired-subscriptions .ok")).to_have_text(
                "FTMS · Indoor Bike Data (2ad2) · NOTIFY · ok")
            expect(page.locator("#paired-subscriptions .pending")).to_have_text(
                "FTMS · Fitness Machine Control Point (2ad9) · INDICATE · pending")
            expect(page.locator("#paired-subscriptions .bad")).to_have_text(
                "HR · Heart Rate Measurement (2a37) · NOTIFY · failed (not-found)")
            expect(page.locator("#paired-subscriptions .bad")).to_have_attribute(
                "title", PAIRED["subscriptions"][2]["service"] + " / " +
                PAIRED["subscriptions"][2]["characteristic"])
            assert page.locator("#paired-advertised .sensor").first.evaluate(
                "e => getComputedStyle(e).backgroundColor") == "rgb(153, 153, 255)"
            assert page.locator("#paired-subscriptions .pending").evaluate(
                "e => getComputedStyle(e).backgroundColor") == "rgb(204, 153, 102)"
            assert page.locator("#paired-subscriptions .ok").evaluate(
                "e => getComputedStyle(e).backgroundColor") == "rgb(153, 204, 255)"
            assert page.locator("#paired-subscriptions .bad").evaluate(
                "e => getComputedStyle(e).textTransform") == "uppercase"
            # Input, bridge and receiving device share one card layout.
            cards = page.locator("#dev .device-card")
            expect(cards).to_have_count(3)
            expect(cards.locator(".facts")).to_have_count(3)
            expect(page.locator("#p-name")).to_have_text("Training bike")
            expect(page.locator("#p-address")).to_have_text("aa:bb:cc:dd:ee:01 (public)")
            expect(page.locator("#b-name")).to_have_text("SmartFitness")
            expect(page.locator("#b-ant")).to_have_text("#4660")
            expect(page.locator("#c-outputProtocol")).to_have_value("ble-ftms")
            expect(page.locator("#b-sent .service-group")).to_have_count(3)
            expect(page.locator("#b-sent .sensor")).to_have_text(
                ["Fitness Machine (FTMS) · 1826", "Cycling Power (CPS) · 1818", "Heart Rate · 180d"])
            expect(page.locator("#b-sent .service-group").first.locator(".ok")).to_have_count(9)
            expect(page.locator("#b-sent .service-group").nth(1).locator(".pending")).to_have_count(3)
            expect(page.locator("#b-sent")).to_contain_text("Indoor Bike Data (2ad2) · NOTIFY")
            expect(page.locator("#b-sent")).to_contain_text("Fitness Machine Control Point (2ad9) · WRITE/INDICATE")
            expect(page.locator("#r-id")).to_have_text("5a:12:34:56:78:9a (random)")
            expect(page.locator("#r-protocol")).to_have_text("BLE FTMS")
            expect(page.locator("#r-state .ok")).to_have_text(["app connected", "app has control"])
            # The sent services follow the selected (not yet applied) output format.
            page.locator("#c-outputProtocol").select_option("ant-fec")
            expect(page.locator("#b-sent-label")).to_have_text("Sent ANT+ channels")
            expect(page.locator("#b-sent .sensor")).to_have_text(["FE-C", "HR"])
            expect(page.locator("#b-sent .pending")).to_have_text(
                ["device type 17 · device #4660 · PENDING", "device type 120 · device #4660 · PENDING"])
            expect(page.locator("#b-note")).to_contain_text("Not applied yet")
            page.locator("#c-outputProtocol").select_option("off")
            expect(page.locator("#b-sent .pending")).to_have_text("output off – nothing is sent")
            page.locator("#c-outputProtocol").select_option("ble-ftms")
            expect(page.locator("#b-sent .sensor")).to_have_count(3)
            expect(page.get_by_label("Adapter for Training bike")).to_have_value("ftms")
            page.get_by_label("Pair Unknown peripheral", exact=True).click()
            expect(page.locator("#toast")).to_have_text("choose an adapter first")
            assert not commands, "Pairing without an adapter sent a command"
            command(page.get_by_label("Pair Training bike", exact=True), "pair")
            expect(page.locator("#toast")).to_have_text("applied")
            assert commands[-1] == ("pair", {
                "address": PAIRED["address"], "addressType": "public", "adapter": "ftms"})
            command(page.locator("#btn-connect"), "connect")
            assert commands[-1] == ("connect", {})
            command(page.locator("#btn-scan"), "scan")
            assert commands[-1] == ("scan", {"seconds": 30}), commands[-1]
            page.locator("#toast").evaluate("e => e.style.display = 'none'")
        elif panel == "ctl":
            page.locator("#ctl-value").fill("999")
            command(page.locator("#btn-apply"), "control")
            expect(page.locator("#ctl-result")).to_have_text("control: disconnected")
            assert commands[-1] == ("control", {"control": "power", "value": 999})
            assert page.locator("#ctl-result").evaluate(
                "e => getComputedStyle(e).backgroundColor") == "rgb(204, 68, 68)"
            assert page.locator("#ctl-result").evaluate(
                "e => getComputedStyle(e).backgroundColor") == page.locator(
                    "#ctl .legend-swatch.bad").evaluate(
                        "e => getComputedStyle(e).backgroundColor")
        elif panel == "set":
            expect(page.locator("#c-ftmsName")).to_have_value("SmartFitness")
            expect(page.locator("#c-wifiPassword")).to_have_value("")
            command(page.locator("#btn-save"), "config")
            expect(page.locator("#toast")).to_have_text("applied")
            assert commands[-1] == ("config", {
                "ftmsName": "SmartFitness", "ftmsOutputProfile": "auto",
                "ftmsOutputServices": 7, "antOutputServices": 5,
                "simulatedTrainer": True, "wifiSsid": "Training room", "https": True, "save": True})
            page.locator("#toast").evaluate("e => e.style.display = 'none'")
        # Remove interaction-only focus so screenshots have a stable resting state.
        page.evaluate("document.activeElement.blur()")
        check_view(page, panel, width < 650)
        page.screenshot(path=str(output / f"{number:02d}-{panel_names[panel]}-{name}.png"),
                        full_page=True)

    # All existing action endpoints and confirmation/cancellation remain intact.
    page.locator("#tab-dev").click()
    for button, endpoint, body in [
        ("btn-disconnect", "disconnect", {}), ("btn-sim", "simulate", {}),
        ("btn-scan-stop", "scan", {"action": "stop"}),
    ]:
        command(page.locator(f"#{button}"), endpoint)
        assert commands[-1] == (endpoint, body)
    before = len(commands)
    page.once("dialog", lambda dialog: dialog.dismiss())
    page.locator("#btn-unpair").click()
    assert len(commands) == before, "Cancelled unpair sent a command"
    page.once("dialog", lambda dialog: dialog.accept())
    command(page.locator("#btn-unpair"), "unpair")
    expect(page.locator("#toast")).to_have_text("applied")
    assert commands[-1] == ("unpair", {})

    page.locator("#tab-ctl").click()
    page.locator("#ctl-value").fill("")
    page.locator("#btn-apply").click()
    expect(page.locator("#toast")).to_have_text("enter a number")
    assert len(commands) == before + 1, "Invalid number sent a control command"
    for control, value in [("resistance", "25"), ("incline", "2"), ("speed", "12")]:
        page.locator("#ctl-kind").select_option(control)
        page.locator("#ctl-value").fill(value)
        command(page.locator("#btn-apply"), "control")
        expect(page.locator("#ctl-result")).to_have_class("small bad")
        assert commands[-1] == ("control", {"control": control, "value": int(value)})

    # The output format is applied from the Devices → Bridge card on its own.
    with page.expect_response("**/api/config"):
        page.locator("#tab-dev").click()
    expect(page.locator("#c-outputProtocol")).to_have_value("ble-ftms")
    expect(page.locator('#c-outputProtocol option[value="ant-fec"]')).to_be_enabled()
    page.locator("#c-outputProtocol").select_option("ant-fec")
    command(page.locator("#btn-output"), "config")
    assert commands[-1] == ("config", {"outputProtocol": "ant-fec", "save": True}), commands[-1]
    with page.expect_response("**/api/config"):
        page.locator("#tab-set").click()
    expect(page.locator("#c-ftmsName")).to_have_value("SmartFitness")
    page.locator("#c-ftmsOutputProfile").select_option("treadmill")
    ble_group, ant_group = page.locator("#c-ftmsServices"), page.locator("#c-antServices")
    expect(ble_group.locator('[data-service="ftms"]')).to_be_disabled()
    expect(ble_group.locator('[data-service="rsc"]')).to_be_disabled()
    expect(ble_group.locator('[data-service="rsc"]')).to_contain_text("NO DATA")
    assert ble_group.locator('[data-service="cps"]').evaluate(
        "e => getComputedStyle(e).backgroundColor") == "rgb(255, 170, 0)"
    ble_group.locator('[data-service="cps"]').click()
    expect(ble_group.locator('[data-service="cps"]')).to_have_attribute("aria-pressed", "false")
    expect(page.locator("#c-ftmsRestart")).to_contain_text("Save + restart to apply this BLE service selection")
    assert ble_group.locator('[data-service="cps"]').evaluate(
        "e => getComputedStyle(e).backgroundColor") == "rgb(204, 153, 102)"
    ant_group.locator('[data-service="power"]').focus()
    page.keyboard.press("Space")
    expect(page.locator("#c-antWarning")).to_be_visible()
    expect(ant_group.locator('[data-service="power"]')).to_be_focused()
    page.locator("#c-simulatedTrainer").uncheck()
    page.locator("#c-ftmsName").fill("Test treadmill")
    # Config WebSocket updates must not overwrite a field currently being edited.
    sockets[0].send(json.dumps(CONFIG))
    expect(page.locator("#c-ftmsName")).to_have_value("Test treadmill")
    page.locator("#c-wifiPassword").fill("test-fixture-only")
    # This fixture page is served over http://, so HTTPS off keeps the restart on this origin.
    expect(page.locator("#c-https")).to_be_checked()
    page.locator("#c-https").uncheck()
    command(page.locator("#btn-save"), "config")
    assert commands[-1] == ("config", {
        "ftmsName": "Test treadmill", "ftmsOutputProfile": "treadmill",
        "ftmsOutputServices": 5, "antOutputServices": 7,
        "simulatedTrainer": False, "wifiSsid": "Training room", "save": True,
        "wifiPassword": "test-fixture-only", "https": False}), commands[-1]
    expect(page.locator("#c-https")).not_to_be_checked()
    # Auto is explicit 0; unavailable ANT hardware removes controls without losing BLE settings.
    ble_group.locator('[data-service="auto"]').click()
    ant_group.locator('[data-service="auto"]').click()
    expect(ble_group.locator('[data-service="auto"]')).to_have_attribute("aria-pressed", "true")
    expect(ble_group.locator('[data-service="hr"]')).to_have_attribute("aria-pressed", "false")
    expect(page.locator("#c-antWarning")).to_be_hidden()
    ant_group.locator('[data-service="fec"]').click()
    expect(page.locator("#toast")).to_have_text("select at least one ANT+ channel or choose Auto")
    expect(ant_group.locator('[data-service="fec"]')).to_have_attribute("aria-pressed", "true")
    page.evaluate("document.activeElement.blur()")
    sockets[0].send(json.dumps({**CONFIG, "antAvailable": False}))
    expect(page.locator("#ant-services")).to_be_hidden()
    expect(page.locator('#c-outputProtocol option[value="ant-fec"]')).to_have_attribute("hidden", "")
    command(page.locator("#btn-save"), "config")
    assert "antOutputServices" not in commands[-1][1]
    sockets[0].send(json.dumps({**CONFIG, "https": False}))
    expect(page.locator("#ant-services")).to_be_visible()
    changed = json.loads(json.dumps(STATUS))
    changed["ftmsOutput"]["restartRequired"] = True
    sockets[0].send(json.dumps(changed))
    expect(page.locator("#c-ftmsRestart")).to_contain_text("pending: restart")
    changed["antOutput"].update(enabled=True, error="insufficient-channels", channelLimit=1)
    sockets[0].send(json.dumps(changed))
    expect(page.locator("#ant-service-state")).to_contain_text("insufficient-channels")
    expect(page.locator("#ant-service-state")).to_contain_text("channel capacity: 1")
    expect(page.locator("#ant-service-state .bad")).to_have_count(3)
    # SDM can use valid treadmill speed without running cadence; BLE RSC cannot.
    speed_only = json.loads(json.dumps(STATUS))
    speed_only["ftmsOutput"]["profile"] = "treadmill"
    speed_only["telemetry"]["cadence"] = None
    for service in speed_only["ftmsOutput"]["services"]:
        if service["id"] == "rsc":
            service["available"] = False
    for channel in speed_only["antOutput"]["channels"]:
        if channel["id"] == "sdm":
            channel["available"] = True
    sockets[0].send(json.dumps(speed_only))
    expect(ble_group.locator('[data-service="rsc"]')).to_be_disabled()
    expect(ant_group.locator('[data-service="sdm"]')).to_be_enabled()
    expect(ant_group.locator('[data-service="sdm"]')).not_to_contain_text("NO DATA")
    # Wi-Fi scan: spinner while busy, then a listbox under the SSID field.
    ssid, wifi_list, wifi_button = (page.locator("#c-wifiSsid"), page.locator("#wifi-list"),
                                    page.locator("#btn-wifi-scan"))
    expect(wifi_list).to_be_hidden()
    wifi_button.click()
    expect(wifi_button).to_have_attribute("aria-busy", "true")
    expect(wifi_button.locator(".spin")).to_be_visible()
    expect(wifi_list).to_be_visible()
    expect(wifi_button).to_have_attribute("aria-busy", "false")
    expect(wifi_button.locator(".spin")).to_be_hidden()
    assert wifi_scans == [{}], wifi_scans
    assert wifi_polls == ["aborted"], wifi_polls
    expect(ssid).to_have_attribute("aria-expanded", "true")
    expect(wifi_list.get_by_role("option")).to_have_count(2)
    expect(wifi_list.get_by_role("option").first).to_have_text("Training roomsecured")
    expect(wifi_list.get_by_role("option").first).to_have_attribute(
        "aria-label", "Training room, signal 4 of 4, secured")
    expect(wifi_list).not_to_contain_text("dBm")
    # SSID field is 1.5x its former intrinsic width (203px -> 305px), capped on narrow screens.
    ssid_width = ssid.evaluate("i => [i.offsetWidth, Math.min(305, i.closest('.row').clientWidth)]")
    assert ssid_width[0] == ssid_width[1], ssid_width
    # Signal pictogram: same thresholds as hmi::signalLevel (-45 dBm -> 4, -71 dBm -> 2).
    assert wifi_list.locator(".bars").evaluate_all(
        "bars => bars.map(b => [b.dataset.level, b.querySelectorAll('i.on').length, "
        "b.getAttribute('aria-hidden')])") == [["4", 4, "true"], ["2", 2, "true"]]
    check_view(page, "set", width < 650)
    page.keyboard.press("ArrowDown")
    page.keyboard.press("ArrowDown")
    expect(ssid).to_have_attribute("aria-activedescendant", "wifi-opt-1")
    page.keyboard.press("Enter")
    expect(ssid).to_have_value("Guest")
    expect(wifi_list).to_be_hidden()
    expect(page.locator("#c-wifiPassword")).to_be_focused()
    ssid.click()
    expect(wifi_list).to_be_visible()
    page.keyboard.press("Escape")
    expect(wifi_list).to_be_hidden()
    ssid.click()
    wifi_list.get_by_role("option").first.click()
    expect(ssid).to_have_value("Training room")
    expect(wifi_list).to_be_hidden()
    for button, endpoint in [("btn-restart", "restart"), ("btn-reset", "factory-reset")]:
        page.once("dialog", lambda dialog: dialog.accept())
        command(page.locator(f"#{button}"), endpoint)
        assert commands[-1] == (endpoint, {})

    unavailable = json.loads(json.dumps(STATUS))
    unavailable["input"]["bleAvailable"] = False
    sockets[0].send(json.dumps(unavailable))
    expect(page.locator("#btn-scan")).to_be_disabled()
    assert page.locator("#btn-scan").evaluate("e => getComputedStyle(e).opacity") == "0.4"
    if width < 650:
        for responsive_width in (320, 650, 651, 800):
            page.set_viewport_size({"width": responsive_width, "height": height})
            for panel in ("dash", "dev", "ctl", "set"):
                page.locator(f"#tab-{panel}").click()
                check_view(page, panel, True)
    sockets[0].close()
    expect(page.locator("#conn")).to_have_text("offline")
    expect(page.locator("#conn")).to_have_class("pill bad")
    assert not external, f"External requests: {external}"
    assert not errors, f"Browser errors: {errors}"
    context.close()
    print(f"{name}: four screenshots; tabs, keyboard, API/WS, ready/backoff/stale/control,"
          " errors, font, offline assets, no overflow passed")


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
    page.locator("#tab-debug").click()
    box = page.locator("#debug-output")
    expected = history.decode() + "[Live] during history request\n"
    expect(box).to_have_value(expected)
    end = len(expected.encode())
    page.wait_for_timeout(350)
    assert requests.count("/debug/log") == 1, "Debug history must not be polled"
    check_view(page, "debug", width < 650)
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
    page.locator("#tab-debug").click()
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
    page.locator("#tab-debug").click()  # history reload keeps the stopped state
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
    parser.add_argument("--embed-font", action="store_true",
                        help="Refresh the inline Antonio face from assets/fonts before rendering")
    parser.add_argument("--html", "--before-asset", dest="before_asset", type=Path,
                        help="Render only baseline dashboard screenshots from a saved original HTML")
    args = parser.parse_args()
    normal, debug = web_content(), web_content(True)
    assert "DEBUG_WEB" not in normal and "DEBUG_WEB" not in debug
    for marker in ("/debug/log", "debug-output", "renderDebug", "tab-debug", "btn-debug-logging"):
        assert marker not in normal and marker in debug, f"Conditional asset leak: {marker}"
    if not args.before_asset:
        assert not re.search(r"(?:linear|radial|conic)-gradient\(", ASSET.read_text(), re.I), \
            "Gradient syntax is not allowed in the web UI"
        font = base64.b64encode((ROOT / "assets/fonts/Antonio-Regular.woff2").read_bytes()).decode()
        if args.embed_font:
            content, count = re.subn(r"""url\(["']?data:font/woff2;base64,[^)]*\)""",
                                    'url("data:font/woff2;base64,' + font + '")', ASSET.read_text())
            assert count == 1, "Expected exactly one inline font face"
            ASSET.write_text(content)
        assert f'url("data:font/woff2;base64,{font}")' in ASSET.read_text(), \
            "Embedded font differs from source or is not quoted"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True)
        for name, width, height in [("desktop", 1280, 800), ("mobile", 390, 844)]:
            if args.before_asset:
                render_before(browser, args.before_asset, args.output_dir, name, width, height)
            else:
                render(browser, args.output_dir, name, width, height)
                check_debug(browser, args.output_dir, name, width, height)
        browser.close()
    content = (args.before_asset or ASSET).read_bytes()
    label = "Baseline asset" if args.before_asset else "Web asset"
    print(f"{label}: {len(content)} bytes; gzip: {len(gzip.compress(content, mtime=0))} bytes")


if __name__ == "__main__":
    main()
