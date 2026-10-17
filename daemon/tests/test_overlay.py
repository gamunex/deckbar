"""Overlay: Layout (Validierung, Teil-Updates), Seite und Live-Daten auf Port+1."""
import asyncio
import json

import pytest
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer

from deckbar_daemon.overlay import DEFAULT_EVENTS, Overlay, OverlayLayout, merge


def test_layout_defaults_and_validation():
    L = OverlayLayout()
    assert L.start_mode == "click" and L.widgets.meter.enabled and not L.widgets.timers.enabled
    assert [e.name for e in L.widgets.timers.events] == [e.name for e in DEFAULT_EVENTS]
    with pytest.raises(ValueError):
        OverlayLayout.model_validate({"widgets": {"meter": {"scale": 9}}})
    with pytest.raises(ValueError):
        OverlayLayout.model_validate({"widgets": {"name": {}}})  # gibt es nicht (mehr)


def test_merge_is_deep_but_replaces_lists():
    a = {"widgets": {"meter": {"x": 1, "y": 2}, "timers": {"events": [1, 2]}}}
    assert merge(a, {"widgets": {"meter": {"x": 5}, "timers": {"events": [3]}}}) == \
        {"widgets": {"meter": {"x": 5, "y": 2}, "timers": {"events": [3]}}}


async def test_update_saves_and_publishes(tmp_path):
    o = Overlay(tmp_path, manage_unit=False)
    q: asyncio.Queue = asyncio.Queue(8)
    o.clients.add(q)
    await o.update({"fx": "frost", "widgets": {"boss": {"x": 100}}})
    assert json.loads((tmp_path / "overlay" / "layout.json").read_text())["fx"] == "frost"
    msg = q.get_nowait()
    assert msg.startswith("event: layout\n") and '"frost"' in msg
    await o.on_state({"dps": {"target": "B", "hp": 50.0, "rows": []}, "out_volume": 3})
    msg = q.get_nowait()
    assert msg.startswith("event: dps\n") and "out_volume" not in msg
    assert Overlay(tmp_path).layout.widgets.boss.x == 100


async def test_autohide_geht_sofort_ans_laufende_programm(tmp_path):
    """Nur ein Wechsel von hide_unfocused schickt einen Befehl über den Socket (hier ein Fake im
    Test-Laufzeitordner aus conftest.py); ohne laufendes Programm wird nur gespeichert."""
    from deckbar_daemon.overlay import socket_path
    o = Overlay(tmp_path, manage_unit=False)
    assert o.layout.hide_unfocused
    await o.update({"hide_unfocused": False})  # kein Socket: kein Fehler
    got: list[bytes] = []

    async def conn(reader, writer):
        got.append(await reader.read())
        writer.close()

    socket_path().parent.mkdir(parents=True, exist_ok=True)
    server = await asyncio.start_unix_server(conn, str(socket_path()))
    async with server:
        await o.update({"fx": "gold"})
        await o.update({"hide_unfocused": True})
        for _ in range(50):
            if got:
                break
            await asyncio.sleep(0.01)
    assert got == [b"autohide-on\n"]


async def test_unit_untouched_for_test_instances(tmp_path):
    o = Overlay(tmp_path, manage_unit=False)
    code, out = await o._systemctl("enable", "--now", "deckbar-overlay.service")
    assert code == 1 and "Testinstanz" in out


class FakeUnit:
    """Stellt systemctl --user nach, ohne systemd anzufassen."""

    def __init__(self, autostart=False):
        self.calls: list[str] = []
        self.active, self.autostart = False, autostart

    async def __call__(self, *args):
        self.calls.append(args[0])
        match args[0]:
            case "start":
                self.active = True
            case "stop":
                self.active = False
            case "disable":
                self.autostart = False
            case "show":
                return 0, (f"LoadState=loaded\nActiveState={'active' if self.active else 'inactive'}\n"
                           f"UnitFileState={'enabled' if self.autostart else 'disabled'}\n")
        return 0, ""


async def test_overlay_kommt_und_geht_mit_dem_spiel(tmp_path, monkeypatch):
    import deckbar_daemon.overlay as ov
    monkeypatch.setattr(ov, "GAME_POLL_S", 0.01)
    monkeypatch.setattr(ov, "GAME_GRACE_S", 0.06)
    o = Overlay(tmp_path, manage_unit=True)
    unit = FakeUnit(autostart=True)
    o._systemctl = unit
    o.refresh_screens = lambda: asyncio.sleep(0)
    game = {"on": False}

    async def check():
        return game["on"]
    o.game_check = check

    await o.on_module(True)
    assert not unit.autostart and not unit.active  # früherer Login-Autostart ist weg, nichts gestartet
    task = asyncio.ensure_future(o.watch())
    await asyncio.sleep(0.05)
    assert not unit.active and "start" not in unit.calls

    game["on"] = True
    await asyncio.sleep(0.05)
    assert unit.active and o.game and unit.calls.count("start") == 1

    unit.active = False  # Overlay mitten im Spiel von Hand beendet: bleibt aus
    await asyncio.sleep(0.05)
    assert unit.calls.count("start") == 1

    unit.active = True
    game["on"] = False
    await asyncio.sleep(0.01)
    assert unit.active  # Gnadenfrist (Ladebildschirm, Absturz mit Neustart)
    await asyncio.sleep(0.1)
    assert not unit.active and not o.game

    await o.on_module(False)
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)


async def test_page_and_api(tmp_path):
    o = Overlay(tmp_path, manage_unit=False)
    app = web.Application()
    o.add_routes(app)
    async with TestClient(TestServer(app)) as c:
        r = await c.get("/__deckbar__/overlay/")
        assert r.status == 200 and "NexBoard Overlay" in await r.text()
        assert (await c.get("/__deckbar__/overlay/overlay.js")).status == 200
        assert (await c.get("/__deckbar__/overlay/../a2ui.py")).status == 404
        r = await c.put("/api/overlay/layout", json={"widgets": {"meter": {"x": 7, "y": 8, "scale": 1.5}}})
        assert r.status == 200 and (await r.json())["widgets"]["meter"]["scale"] == 1.5
        assert (await c.put("/api/overlay/layout", json={"start_mode": "edit"})).status == 422
        r = await c.get("/api/overlay/events")
        first = await r.content.readuntil(b"\n\n")
        assert first.startswith(b"event: layout")
        r.close()


async def test_close_clients_beendet_offene_streams(tmp_path):
    """Beim Beenden des Daemons darf aiohttp nicht auf offene Event-Streams warten."""
    o = Overlay(tmp_path, manage_unit=False)
    app = web.Application()
    o.add_routes(app)
    async with TestClient(TestServer(app)) as c:
        r = await c.get("/api/overlay/events")
        await r.content.readuntil(b"\n\n")
        await r.content.readuntil(b"\n\n")
        assert len(o.clients) == 1
        o.close_clients()
        assert await asyncio.wait_for(r.content.read(), 2) == b""  # Server hat beendet
        assert not o.clients
