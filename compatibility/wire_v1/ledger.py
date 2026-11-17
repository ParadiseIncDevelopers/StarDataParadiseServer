"""Call the protected InnoDB receipt ledger in the caller's MySQL session."""

from hashlib import sha256
import hmac
import re

from mysql.connector.errors import Error

from sql import Fault, require


TOKEN = re.compile(r"[0-9a-f]{32}")
INACTIVE = 60001
INVALID_PROOF = 60002
DUPLICATE = 60003


def read_key(path):
    encoded = path.read_text(encoding="ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{64}", encoded):
        raise ValueError("the ledger key file must contain 64 lowercase hexadecimal characters")
    return bytes.fromhex(encoded)


def request_token(body):
    token = body.get("request_token")
    require(isinstance(token, str) and TOKEN.fullmatch(token))
    return token


class Receipts:
    def __init__(self, db, key, nonce, identity):
        self.db = db
        self.key = key
        self.nonce = nonce
        self.actor = identity[1].encode("utf-8").hex().upper()
        self.connection_id = identity[2]
        self.scoped_replay = False

    def command(self, action, token="0" * 32, anchor="0" * 32):
        signed = f"{self.nonce}:{self.connection_id}:{self.actor}:{action}:{token}:{anchor}"
        signature = hmac.new(self.key, signed.encode("ascii"), sha256).hexdigest()
        try:
            with self.db.cursor(buffered=True) as cursor:
                cursor.execute("CALL sdp_wire_v1.receipt(%s,%s,%s,%s,%s,@sdp_receipt_value)",
                               (self.actor, action, token, anchor, signature))
                if action in {"probe", "finish", "lookup", "match", "occupied"}:
                    cursor.execute("SELECT @sdp_receipt_value")
                    return cursor.fetchone()[0]
        except Error as error:
            if error.errno == DUPLICATE:
                raise Fault(1013) from None
            if error.errno in {INACTIVE, INVALID_PROOF, 1305, 1370}:
                raise Fault(1004) from None
            raise

    def available(self):
        try:
            version = self.command("probe")
            self.scoped_replay = version == 2
            return version in (1, 2)
        except Fault as error:
            if error.code == 1004:
                return False
            raise
