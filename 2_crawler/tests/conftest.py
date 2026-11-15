import os
from uuid import uuid4

import pytest
from pymongo import MongoClient


@pytest.fixture
def mongo_config():
    config = {
        "uri": os.environ.get(
            "MONGODB_URI", "mongodb://localhost:27017/?replicaSet=rs0&directConnection=true"
        ),
        "name": "crawler_test_" + uuid4().hex,
    }
    with MongoClient(config["uri"], serverSelectionTimeoutMS=5000) as client:
        client.admin.command("ping")
        yield config
        client.drop_database(config["name"])
