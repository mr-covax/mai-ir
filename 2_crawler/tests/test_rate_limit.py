from unittest.mock import AsyncMock

import pytest

from crawler.http import wait_for_host


@pytest.mark.parametrize("next_request", [{}, {"example.org": 8.0}])
async def test_no_wait_when_host_is_ready(monkeypatch, next_request):
    sleep = AsyncMock()
    monkeypatch.setattr("crawler.http.monotonic", lambda: 10.0)
    monkeypatch.setattr("crawler.http.asyncio.sleep", sleep)

    await wait_for_host("https://example.org/page", next_request, 2)

    sleep.assert_not_awaited()
    assert next_request["example.org"] == 12.0


async def test_wait_until_host_is_ready(monkeypatch):
    clock = iter([10.0, 12.0])
    sleep = AsyncMock()
    monkeypatch.setattr("crawler.http.monotonic", lambda: next(clock))
    monkeypatch.setattr("crawler.http.asyncio.sleep", sleep)
    next_request = {"example.org": 12.0}

    await wait_for_host("https://example.org/page", next_request, 2)

    sleep.assert_awaited_once_with(2.0)
    assert next_request["example.org"] == 14.0


async def test_hosts_have_independent_delays(monkeypatch):
    sleep = AsyncMock()
    monkeypatch.setattr("crawler.http.monotonic", lambda: 10.0)
    monkeypatch.setattr("crawler.http.asyncio.sleep", sleep)
    next_request = {"other.org": 20.0}

    await wait_for_host("https://example.org/page", next_request, 2)

    sleep.assert_not_awaited()
    assert next_request == {"other.org": 20.0, "example.org": 12.0}
