"""A snapshot reports what THIS run measured, never what an earlier one did.

`MEASURED_THIS_RUN` and `MEASURED_DATES` are module-level, so a second `snapshot()` in the same
process would inherit the first's claims unless they are cleared: a target measured once and then
unavailable would keep reporting `Built: yes` and carry the first run's date, which is exactly the
false reassurance the freshness rule exists to prevent, moved one call later.

The report's own vocabulary is pinned here too, because the labels are what a reader acts on:
`yes` measured now, `carried Nd` not rebuilt and N days old, `carried (age?)` not rebuilt and from
before dates were recorded, and `STALE` past the threshold.
"""

import datetime as dt
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

import repo_health  # noqa: E402


def test_second_snapshot_does_not_inherit_the_first_runs_measurement(monkeypatch):
    """A target measured once, then unmeasurable, must stop claiming it was built."""
    calls = {"n": 0}

    def fake_measure_flash():
        # First call measures a target the way the real one does; the second finds nothing
        # (the binary is gone, or too old to count) and records neither.
        calls["n"] += 1
        if calls["n"] == 1:
            repo_health.MEASURED_THIS_RUN.add("esp32-pico")
            repo_health.MEASURED_DATES["esp32-pico"] = "2020-01-01"
            return {"esp32-pico": 1000}
        return {}

    monkeypatch.setattr(repo_health, "measure_flash", fake_measure_flash)
    for name in ("measure_loc", "measure_comments", "measure_tests", "measure_docs",
                 "measure_complexity"):
        monkeypatch.setattr(repo_health, name, lambda: {})
    monkeypatch.setattr(repo_health, "_head", lambda: "deadbeef")

    first = repo_health.snapshot()
    assert first["measured"]["esp32-pico"] == "2020-01-01"
    assert repo_health._built_label("esp32-pico", first["measured"].get("esp32-pico")) == "yes"

    second = repo_health.snapshot()
    assert "esp32-pico" not in second["measured"], "the date must not survive into a second run"
    assert "esp32-pico" not in repo_health.MEASURED_THIS_RUN
    # And the label now describes a carry rather than a measurement.
    assert repo_health._built_label("esp32-pico", second["measured"].get("esp32-pico")) != "yes"


def test_built_labels_say_measured_carried_or_stale():
    """The four labels a reader acts on, including the undated carry."""
    repo_health.MEASURED_THIS_RUN.clear()
    repo_health.MEASURED_THIS_RUN.add("fresh")
    recent = (dt.date.today() - dt.timedelta(days=2)).isoformat()
    old = (dt.date.today() - dt.timedelta(days=repo_health.STALE_AFTER_DAYS + 1)).isoformat()

    assert repo_health._built_label("fresh", recent) == "yes"
    assert repo_health._built_label("other", recent) == "carried 2d"
    assert "STALE" in repo_health._built_label("other", old)
    # No date at all: honest about the gap rather than guessing one.
    assert repo_health._built_label("other", None) == "carried (age?)"
    assert repo_health._built_label("other", "not-a-date") == "carried (age?)"
    repo_health.MEASURED_THIS_RUN.clear()
