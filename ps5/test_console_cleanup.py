# SPDX-License-Identifier: GPL-3.0-or-later
"""Helper cleanup must preserve processes and payload slots owned by others."""
import unittest
import urllib.parse

from console_probe import Console


class RebootedConsole(Console):
    def __init__(self, ftp_running=False):
        self.state = {"helper_pid": 93, "owned_slots": ["local/our-transfer"], "closed": False}
        self.ftp_running = ftp_running
        self.slot_names = {"local/our-transfer", "local/user-payload"}
        self.deleted = []

    def processes(self):
        return [{"pid": 93, "name": "pldmgr.elf"}]

    def slots(self):
        return [{"slot": slot} for slot in self.slot_names]

    def ftp_listening(self):
        return self.ftp_running

    def ftp_open(self):
        raise AssertionError("Must not send commands to a reused PID or unrelated FTP server")

    def save(self):
        pass

    def request(self, path, data=None, length=None):
        assert path.startswith("/manage:delete?")
        slot = urllib.parse.parse_qs(path.split("?", 1)[1])["slot"][0]
        self.deleted.append(slot)
        self.slot_names.remove(slot)


class CleanupTests(unittest.TestCase):
    def test_reused_pid_preserves_process_and_unrelated_slot(self):
        console = RebootedConsole()
        console.close()
        self.assertTrue(console.state["closed"])
        self.assertEqual(console.state["helper_pid_reused"]["current_name"], "pldmgr.elf")
        self.assertEqual(console.deleted, ["local/our-transfer"])
        self.assertEqual(console.slot_names, {"local/user-payload"})
        self.assertEqual(console.state["final_processes"], [{"pid": 93, "name": "pldmgr.elf"}])

    def test_reused_pid_with_live_ftp_stops_without_mutation(self):
        console = RebootedConsole(ftp_running=True)
        with self.assertRaisesRegex(RuntimeError, "another FTP server"):
            console.close()
        self.assertFalse(console.state["closed"])
        self.assertEqual(console.deleted, [])

    def test_pending_diagnostic_remains_protected(self):
        console = RebootedConsole()
        console.state["pending_launch"] = {"path": "diagnostic.elf"}
        with self.assertRaisesRegex(RuntimeError, "no verified completion"):
            console.close()
        self.assertEqual(console.deleted, [])


if __name__ == "__main__":
    unittest.main()
