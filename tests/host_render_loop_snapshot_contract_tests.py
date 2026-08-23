#!/usr/bin/env python3
"""Source contract for the Host render loop's lean control snapshot."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


if len(sys.argv) < 3:
    raise RuntimeError("expected desktop main.cpp and host_control.cpp paths")

MAIN_SOURCE = Path(sys.argv.pop(1))
HOST_CONTROL_SOURCE = Path(sys.argv.pop(1))


class HostRenderLoopSnapshotContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = MAIN_SOURCE.read_text(encoding="utf-8-sig")
        loop_start = source.index(
            "    while (!quit && !hostWindow.closeRequested())"
        )
        loop_end = source.index(
            "\n    if (backgroundExecution.transactionActive)", loop_start
        )
        cls.loop_source = source[loop_start:loop_end]

        control_source = HOST_CONTROL_SOURCE.read_text(encoding="utf-8-sig")
        snapshot_start = control_source.index(
            "HostRuntimeSnapshot HostControlPlane::runtimeSnapshot() const"
        )
        snapshot_end = control_source.index(
            "\nHostStateSnapshot HostControlPlane::snapshot() const",
            snapshot_start,
        )
        cls.runtime_snapshot_source = control_source[
            snapshot_start:snapshot_end
        ]

    def test_render_loop_uses_only_the_runtime_snapshot(self) -> None:
        # Keep this check at the real main-loop call site: a unit test of
        # runtimeSnapshot() alone would not catch a future hot-path regression.
        self.assertIn(
            "const bafx::desktop::HostRuntimeSnapshot controlState =",
            self.loop_source,
        )
        self.assertEqual(self.loop_source.count("control.runtimeSnapshot()"), 1)
        self.assertNotIn("control.snapshot()", self.loop_source)

    def test_runtime_snapshot_does_not_materialize_cold_profile_state(self) -> None:
        for field in (
            "config_",
            "configGeneration_",
            "paused_",
            "ipc_.stopRequested()",
        ):
            self.assertIn(field, self.runtime_snapshot_source)
        for forbidden in (
            "snapshotLocked",
            "fxProfileStore_",
            "summaries()",
            "activeProfileName",
            "loadWarning",
        ):
            self.assertNotIn(forbidden, self.runtime_snapshot_source)


if __name__ == "__main__":
    unittest.main()
