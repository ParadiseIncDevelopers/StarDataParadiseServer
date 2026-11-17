"""Commit legacy mutations and their wire receipt in one authenticated MySQL transaction."""

from hashlib import sha256
import json

from mysql.connector.errors import Error

from ledger import request_token
from sql import Fault, require


NO_SCOPE = "0" * 32


def rollback(session):
    try:
        session.db.rollback()
    except (Error, OSError):
        session.close()
        raise Fault(1010, close=True) from None


def matching_receipt(session, token, binding, replay):
    session.db.start_transaction(readonly=True, isolation_level="REPEATABLE READ")
    try:
        lsn = session.receipts.command("match", token, binding)
        require(lsn is not None, 1013)
        response = replay(token)
        require(isinstance(response, dict), 1013)
    finally:
        rollback(session)
    return {**response, "lsn": str(lsn)}


def mutate(session, body, apply, *, scope=None, replay=None, authorize=None):
    """Run app DML using this session. A replay callback may only read committed domain state."""
    require(session.result is None and not session.db.in_transaction, 1013)
    require(session.receipts is not None, 1004)
    require(replay is None or scope is not None, 1015)
    token = request_token(body)
    binding = NO_SCOPE
    if scope is not None:
        require(session.receipts.scoped_replay, 1004)
        context = json.dumps(scope, separators=(",", ":"), sort_keys=True, allow_nan=False).encode("utf-8")
        binding = sha256(b"sdp-wire-mutation-v1\x00" + context).hexdigest()[:32]
        require(binding != NO_SCOPE, 1015)

    session.db.start_transaction()
    authorizing = authorize is not None
    reserved = False
    try:
        if authorize is not None:
            authorize()
        authorizing = False
        session.receipts.command("reserve", token, binding)
        reserved = True
        response = apply(token)
        require(isinstance(response, dict) and session.db.in_transaction, 1015)
        lsn = session.receipts.command("finish", token)
    except Exception as error:
        rollback(session)
        if (not authorizing and not reserved and replay is not None and isinstance(error, Fault)
                and error.code == 1013 and not error.close):
            return matching_receipt(session, token, binding, replay)
        raise

    try:
        session.commit_backend()
    except Fault as error:
        if not error.close:
            rollback(session)
        raise
    return {**response, "lsn": str(lsn)}
