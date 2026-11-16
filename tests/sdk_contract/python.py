"""Run the shared Stage 47 contract through an installed Python DB-API wheel."""

from decimal import Decimal
import json
import math
import os
from pathlib import Path
import secrets
import sys

import stardataparadise_sdk as db


def setting(name):
    return os.environ["SDP_SDK_" + name]


def connect(login, *, target="PORT", autocommit=True, retry_reads=False, tls_name=None):
    user, password = login
    return db.connect(setting("HOST"), int(setting(target)), user=user, password=password,
                      ca_file=setting("CA"), tls_name=tls_name or setting("TLS_NAME"),
                      autocommit=autocommit, retry_reads=retry_reads, timeout=15)


def query(connection, sql, parameters=None):
    with connection.cursor() as cursor:
        return cursor.execute(sql, parameters).fetchall()


def write(connection, sql, parameters=None):
    with connection.cursor() as cursor:
        return cursor.execute(sql, parameters).rowcount


def failure(action, code):
    try:
        action()
    except db.Error as error:
        assert error.code == code, (error.code, code)
        return error
    raise AssertionError(f"Expected SDK error {code}")


def main():
    owner = (setting("USER"), sys.stdin.readline().rstrip("\r\n"))
    guest = (setting("GUEST"), sys.stdin.readline().rstrip("\r\n"))
    assert owner[1] and guest[1]
    assert "site-packages" in Path(db.__file__).parts
    assert db.__version__ == "0.2.0" and db.apilevel == "2.0"
    assert db.WIRE_CONTRACT_SHA256 == setting("CONTRACT_SHA256")
    failure(lambda: connect((owner[0], guest[1])), 1005)
    failure(lambda: connect(owner, tls_name="absent.invalid"), 1011)
    with connect(guest) as connection:
        assert query(connection, "SELECT id FROM sdk_seed WHERE id=1") == [(1,)]
        assert failure(lambda: connection.history("sdk_seed"), 1006).sqlstate == "42501"

    with connect(owner) as connection:
        first, second = query(connection, "SELECT note,amount,stamp,payload FROM sdk_seed ORDER BY id")
        assert first[0] == "İstanbul'un notu; a\0b" and first[3] == bytes.fromhex("00017f80feff")
        assert first[1].as_tuple() == Decimal("-12345678901234567890123456789012.3400").as_tuple()
        assert first[2] == db.TimestampValue(-1, 999999999, 180) and second == (None,) * 4

        with connection.cursor() as cursor:
            cursor.callproc("sdk_list", (1, None))
            assert cursor.fetchall() == [(1,)] and cursor.set_name == "out"
            assert cursor.nextset() and cursor.set_name == "found" and cursor.fetchall() == [(1,)]
            assert cursor.nextset() and cursor.set_name == "all_rows" and cursor.fetchall() == [(1,), (2,)]
            assert cursor.nextset() is None

        write(connection, "CREATE TABLE sdk_python_tx (id BIGINT PRIMARY KEY,note TEXT)")
        assert write(connection, "INSERT INTO sdk_python_tx VALUES (:id,:note)",
                     {"id": 1, "note": db.Bind(None, "TEXT")}) == 1
        assert write(connection, "UPDATE sdk_python_tx SET note=:note WHERE id=1", {"note": "updated"}) == 1
        assert query(connection, "SELECT note FROM sdk_python_tx WHERE id=1") == [("updated",)]
        assert write(connection, "DELETE FROM sdk_python_tx WHERE id=1") == 1
        assert query(connection, "SELECT id FROM sdk_python_tx") == []

        denied = failure(lambda: write(connection, "INSERT INTO sdk_guard VALUES (1,-1)"), 1006)
        assert denied.sqlstate == "42501" and not denied.unknown_outcome
        history_rows = sum(entry["kind"] == "row" for entry in connection.history("sdk_seed")["entries"])
        assert history_rows == 2
        prediction = connection.predict(setting("MODEL"), {"reading": 17})["prediction"]
        assert math.isclose(prediction, 58, abs_tol=1e-4)
        observed = {"decision_denied": denied.code, "history_rows": history_rows, "prediction": round(prediction)}
        if expired := os.environ.get("SDP_SDK_EXPIRED_TOKEN"):
            observed["expired_outcome"] = connection.outcome(expired)["status"]
            assert observed["expired_outcome"] == "unknown"

    connection = connect(owner, autocommit=False)
    original = connection.session_id
    try:
        write(connection, "INSERT INTO sdk_python_tx VALUES (11,'pending')")
        with connection.cursor() as cursor:
            failure(lambda: cursor.executemany("INSERT INTO sdk_python_tx VALUES (:id,'batch')",
                                                 [{"id": 12}, {"id": 11}]), 1012)
        assert query(connection, "SELECT id FROM sdk_python_tx ORDER BY id") == [(11,)]
    finally:
        connection.close()
    with connect(owner) as fresh:
        assert fresh.session_id != original and query(fresh, "SELECT id FROM sdk_python_tx") == []
    print("SDP_STAGE47_RESET verified_close_rollback")

    if "SDP_SDK_FAULT_PORT" in os.environ:
        request = secrets.token_hex(16)
        connection = connect(owner, target="FAULT_PORT", autocommit=False)
        try:
            write(connection, "INSERT INTO sdk_python_tx VALUES (21,'committed')")
            error = failure(lambda: connection.commit(request_token=request), 1010)
            assert isinstance(error, db.OutcomeUnknown) and error.request_token == request
        finally:
            connection.close()
        with connect(owner) as fresh:
            assert fresh.outcome(request)["status"] == "committed"
            assert query(fresh, "SELECT id FROM sdk_python_tx") == [(21,)]

    if "SDP_SDK_READ_FAULT_PORT" in os.environ:
        with connect(owner, target="READ_FAULT_PORT", retry_reads=True) as connection:
            session = connection.session_id
            assert query(connection, "SELECT id FROM sdk_seed WHERE id=1") == [(1,)]
            assert session != connection.session_id
        print("SDP_STAGE47_READ_RECOVERY automatic_read_retry")
    print("SDP_STAGE47_RESULT " + json.dumps(observed, sort_keys=True))
    print("SDP_MANAGED_OK")


if __name__ == "__main__":
    main()
