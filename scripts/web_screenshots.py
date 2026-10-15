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


ROOT = Path(__file__).resolve().parents[1]
ASSET = ROOT / "src/variants/web/assets/index.html"
ORIGIN = "http://bridge.test"
PAIRED = {
    "name": "Training bike", "address": "aa:bb:cc:dd:ee:01",
    "addressType": "public", "adapter": "ftms",
}
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
                   "name": "SmartFitness", "advertising": True, "client": True, "control": True},
    "ant": False, "unsaved": False,
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
    "type": "config", "version": "1.0.0", "ftmsName": "SmartFitness", "ftmsOutput": True,
    "ftmsOutputProfile": "auto", "ftmsInput": True, "ant": False,
    "antRequiresHardware": True, "simulatedTrainer": True, "wifiSsid": "Training room",
    "wifiPasswordSet": True, "apPasswordDefault": True, "unsaved": False,
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


def render(browser, output, name, width, height):
    context = browser.new_context(viewport={"width": width, "height": height},
                                  device_scale_factor=1, reduced_motion="reduce")
    page = context.new_page()
    errors, external, commands, sockets, results, wifi_scans = [], [], [], [], {}, []
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
            request_route.fulfill(body=ASSET.read_text(), content_type="text/html")
            return
        if request.method == "POST" and url.path == "/api/wifi-scan":
            wifi_scans.append(request.post_data_json)
            request_route.fulfill(status=202, json={"scanning": True})
            return
        if request.method == "POST":
            endpoint = url.path.removeprefix("/api/")
            commands.append((endpoint, request.post_data_json))
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
        fixtures = {"/api/status": STATUS, "/api/scan/results": SCAN, "/api/config": CONFIG,
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

    panel_names = {"dash": "dashboard", "dev": "devices", "ctl": "control", "set": "settings"}
    for number, panel in enumerate(panel_names, start=1 if name == "desktop" else 5):
        page.locator(f"#tab-{panel}").click()
        check_view(page, panel, width < 650)
        if panel == "dev":
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
        elif panel == "set":
            expect(page.locator("#c-ftmsName")).to_have_value("SmartFitness")
            expect(page.locator("#c-wifiPassword")).to_have_value("")
            command(page.locator("#btn-save"), "config")
            expect(page.locator("#toast")).to_have_text("applied")
            assert commands[-1] == ("config", {
                "ftmsName": "SmartFitness", "ftmsOutput": True, "ftmsOutputProfile": "auto",
                "ant": False, "simulatedTrainer": True, "wifiSsid": "Training room", "save": True})
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

    with page.expect_response("**/api/config"):
        page.locator("#tab-set").click()
    expect(page.locator("#c-ftmsName")).to_have_value("SmartFitness")
    page.locator("#c-ftmsOutputProfile").select_option("treadmill")
    page.locator("#c-ftmsOutput").uncheck()
    page.locator("#c-simulatedTrainer").uncheck()
    page.locator("#c-ftmsName").fill("Test treadmill")
    # Config WebSocket updates must not overwrite a field currently being edited.
    sockets[0].send(json.dumps(CONFIG))
    expect(page.locator("#c-ftmsName")).to_have_value("Test treadmill")
    page.locator("#c-wifiPassword").fill("test-fixture-only")
    command(page.locator("#btn-save"), "config")
    assert commands[-1] == ("config", {
        "ftmsName": "Test treadmill", "ftmsOutput": False, "ftmsOutputProfile": "treadmill",
        "ant": False, "simulatedTrainer": False, "wifiSsid": "Training room", "save": True,
        "wifiPassword": "test-fixture-only"}), commands[-1]
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
    expect(ssid).to_have_attribute("aria-expanded", "true")
    expect(wifi_list.get_by_role("option")).to_have_count(2)
    expect(wifi_list.get_by_role("option").first).to_contain_text("-45 dBm · secured")
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
        browser.close()
    content = (args.before_asset or ASSET).read_bytes()
    label = "Baseline asset" if args.before_asset else "Web asset"
    print(f"{label}: {len(content)} bytes; gzip: {len(gzip.compress(content, mtime=0))} bytes")


if __name__ == "__main__":
    main()
