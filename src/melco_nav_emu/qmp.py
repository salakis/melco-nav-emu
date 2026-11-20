# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
"""Minimal QMP client for controlling a running emulator."""
from __future__ import annotations

import json
import socket
from typing import Any, Dict, List, Optional, Tuple


class QMPError(RuntimeError):
    """A QMP command returned an error."""


class QMPClient:
    def __init__(self, address: str, timeout: float = 10.0):
        self.address = address
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._file = None
        self.events: List[Dict[str, Any]] = []

    # -- connection -----------------------------------------------------
    def connect(self) -> "QMPClient":
        if self.address.startswith("unix:"):
            path = self.address[5:].split(",", 1)[0]
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(self.timeout)
            sock.connect(path)
        else:
            host, _, port = self.address.rpartition(":")
            if not host:
                raise ValueError("QMP address must be unix:/path or host:port")
            sock = socket.create_connection((host, int(port)), self.timeout)
        self._sock = sock
        self._file = sock.makefile("rwb", buffering=0)
        greeting = self._read_json()
        if "QMP" not in greeting:
            raise QMPError("unexpected QMP greeting: %r" % (greeting,))
        self.execute("qmp_capabilities")
        return self

    def close(self) -> None:
        try:
            if self._file is not None:
                self._file.close()
        finally:
            if self._sock is not None:
                self._sock.close()
            self._file = None
            self._sock = None

    def __enter__(self) -> "QMPClient":
        return self.connect()

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- protocol -------------------------------------------------------
    def _read_json(self) -> Dict[str, Any]:
        if self._file is None:
            raise QMPError("not connected")
        line = self._file.readline()
        if not line:
            raise QMPError("QMP connection closed")
        try:
            value = json.loads(line.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise QMPError("invalid QMP JSON: %r" % (line,)) from exc
        if not isinstance(value, dict):
            raise QMPError("QMP message is not an object: %r" % (value,))
        return value

    def execute(self, command: str, arguments: Optional[Dict[str, Any]] = None) -> Any:
        if self._file is None:
            raise QMPError("not connected")
        request: Dict[str, Any] = {"execute": command}
        if arguments:
            request["arguments"] = arguments
        self._file.write((json.dumps(request) + "\n").encode("utf-8"))
        while True:
            msg = self._read_json()
            if "event" in msg:
                self.events.append(msg)
                continue
            if "error" in msg:
                err = msg["error"]
                raise QMPError(
                    "%s: %s" % (err.get("class", "error"), err.get("desc", err))
                )
            if "return" in msg:
                return msg["return"]
            raise QMPError("unexpected QMP response: %r" % (msg,))

    # -- convenience ----------------------------------------------------
    def screendump(self, filename: str, device: Optional[str] = None) -> None:
        args: Dict[str, Any] = {"filename": filename}
        if device:
            args["device"] = device
        self.execute("screendump", args)

    def send_key(self, qcode: str, down: bool = True) -> None:
        self.execute(
            "input-send-event",
            {
                "events": [
                    {
                        "type": "key",
                        "data": {"down": down, "key": {"type": "qcode", "data": qcode}},
                    }
                ]
            },
        )

    def human_monitor(self, command: str) -> str:
        return self.execute("human-monitor-command", {"command-line": command})

    def quit(self) -> None:
        self.execute("quit")
