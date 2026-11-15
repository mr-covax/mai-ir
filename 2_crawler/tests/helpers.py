from contextlib import contextmanager
from functools import wraps

from pymongo import MongoClient
from inspect import isawaitable

import httpx


@contextmanager
def connect(config):
    with MongoClient(config.uri, serverSelectionTimeoutMS=5000) as client:
        yield client[config.name]


def rows(database, collection, fields, *, sort="_id", query=None):
    return [
        tuple(document.get(field) for field in fields)
        for document in database[collection].find(query or {}).sort(sort, 1)
    ]


def without_robots(handler):
    @wraps(handler)
    async def respond(request):
        if request.url.path == "/robots.txt":
            return httpx.Response(404)

        response = handler(request)
        return await response if isawaitable(response) else response

    return respond
