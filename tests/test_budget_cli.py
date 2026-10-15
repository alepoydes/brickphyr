# SPDX-License-Identifier: Apache-2.0
"""Budget preflight reports firmware reservations without performing a launch."""

import asyncio
import json
import sys
from types import SimpleNamespace

import cbor2
import pytest

from brickphyrdev import budget, cli


class Reply(SimpleNamespace):
    def model_dump(self, *, exclude):
        return {key: value for key, value in vars(self).items() if key not in exclude}


def arguments(**options):
    return SimpleNamespace(**({"name": None, "stack": 0, "controller": None, "resource": None,
                               "parameters": 0, "record_size": None, "record_count": None,
                               "record_config": 0, "native_code": False, "dynamic": None} | options))


def test_budget_queries_and_reports_replacement_with_arena_reuse(monkeypatch):
    calls = []

    async def memory_snapshot(_client):
        calls.append("memory")
        return {"overview": {"heap_total": 1000, "heap_used": 300, "heap_free": 700,
                             "heap_peak": 400, "staging": 100, "source_flash": 70,
                             "python_arena": 200},
                "stacks": []}

    async def checked(_client, request):
        calls.append(request)
        if isinstance(request, budget.RecordBudget):
            return Reply(bytes=40)
        if request.control:
            return Reply(format=budget.application.Format.NATIVE, image=60, source_ram=0,
                         source_flash=60, transfer=0, stack=50, user_stack=64, priv_stack=16,
                         arena=0, code_limit=0, native=90, alignment=32, supervisor=8)
        return Reply(format=budget.application.Format.FILES, image=100, source_ram=100,
                     source_flash=0, transfer=100, stack=80, user_stack=96, priv_stack=16,
                     arena=200, code_limit=30, native=0, alignment=32, supervisor=8)

    monkeypatch.setattr(budget.application, "memory_snapshot", memory_snapshot)
    monkeypatch.setattr(budget, "checked", checked)
    report = asyncio.run(budget.inspect(None, arguments(controller="motor", parameters=12,
                                                       record_size=8, record_count=3,
                                                       native_code=True, dynamic=5)))
    assert calls[0] == "memory"
    assert [type(call) for call in calls[1:]] == [budget.ImageBudget, budget.ImageBudget,
                                                  budget.RecordBudget]
    assert cbor2.loads(calls[1].BYTES[8:]) == {"name": "", "stack": 0}
    assert cbor2.loads(calls[2].BYTES[8:]) == {"name": "motor", "resource": "",
                                              "control": True, "parameters": 12}
    assert cbor2.loads(calls[3].BYTES[8:]) == {"size": 8, "capacity": 3, "config": 0,
                                              "control": True, "parameters": 12}
    assert report["resident_accounted"] == 673
    assert report["phases"]["load"]["accounted"] == 773
    # Current heap already owns the ready source and a reusable Python arena.
    # Before release only the new primary stack and supervisor must coexist.
    assert report["phases"]["replacement"]["before_release_accounted"] == 420
    assert report["phases"]["replacement"]["peak"] == "unknown"
    assert report["potential_store_mutation_snapshot"] == 70
    assert report["staged_transfer"] == 100
    assert report["admission"]["status"] == "fit_unknown"
    json.dumps(report)


def test_budget_rejects_incomplete_recorder_before_queries(monkeypatch):
    monkeypatch.setattr(budget, "connection", lambda **_kwargs: pytest.fail(
        "Invalid options must not open a hub connection"))
    with pytest.raises(ValueError, match="provided together"):
        asyncio.run(budget.execute(arguments(record_size=8)))


@pytest.mark.parametrize("controller", [None, "motor"])
def test_budget_read_error_retains_operation_and_errno(monkeypatch, controller):
    async def memory_snapshot(_client):
        return {"overview": {}, "stacks": []}

    async def checked(_client, request):
        if controller is None or request.control:
            raise OSError(22, "invalid image")
        return object()

    monkeypatch.setattr(budget.application, "memory_snapshot", memory_snapshot)
    monkeypatch.setattr(budget, "checked", checked)
    monkeypatch.setattr(budget, "_image_dict", lambda _reply: {"format": "python"})
    with pytest.raises(RuntimeError, match=("Controller" if controller else "Primary") +
                       ".*Errno 22") as caught:
        asyncio.run(budget.inspect(None, arguments(controller=controller)))
    assert isinstance(caught.value.__cause__, OSError)


def test_budget_cli_selects_read_only_handler(monkeypatch):
    selected = []
    monkeypatch.setattr(budget, "run", selected.append)
    monkeypatch.setattr(sys, "argv", ["brickphyrdev", "app", "budget", "program",
                                      "--controller", "motor", "--parameters", "12"])
    cli.main()
    assert len(selected) == 1
    assert (selected[0].name, selected[0].controller, selected[0].parameters) == (
        "program", "motor", 12)
