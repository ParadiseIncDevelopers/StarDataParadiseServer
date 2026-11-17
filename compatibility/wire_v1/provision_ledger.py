"""Provision the loopback wire receipt ledger using a verified MySQL administrator connection."""

import argparse
from getpass import getpass
from pathlib import Path
import re
import sys

from mysql import connector
from mysql.connector.errors import Error

from ledger import read_key


DDL = """
CREATE DATABASE sdp_wire_v1;
CREATE USER 'sdp_wire_receipt'@'localhost' ACCOUNT LOCK;
CREATE TABLE sdp_wire_v1.anchor (
    id BINARY(16) PRIMARY KEY
) ENGINE=InnoDB;
CREATE TABLE sdp_wire_v1.outcome (
    actor VARBINARY(288) NOT NULL,
    token BINARY(16) NOT NULL,
    binding_id BINARY(16) NOT NULL,
    lsn BIGINT UNSIGNED,
    confirmed_at DATETIME(6),
    PRIMARY KEY(actor, token)
) ENGINE=InnoDB;
CREATE TABLE sdp_wire_v1.receipt_sequence (
    singleton TINYINT PRIMARY KEY CHECK (singleton = 1),
    lsn BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB;
INSERT INTO sdp_wire_v1.receipt_sequence VALUES (1, 0);
GRANT SELECT ON performance_schema.session_connect_attrs TO 'sdp_wire_receipt'@'localhost';
GRANT SELECT, INSERT, DELETE ON sdp_wire_v1.anchor TO 'sdp_wire_receipt'@'localhost';
GRANT SELECT, INSERT, UPDATE ON sdp_wire_v1.outcome TO 'sdp_wire_receipt'@'localhost';
GRANT SELECT, UPDATE ON sdp_wire_v1.receipt_sequence TO 'sdp_wire_receipt'@'localhost';

DELIMITER $$
CREATE DEFINER='sdp_wire_receipt'@'localhost' PROCEDURE sdp_wire_v1.receipt(
    IN actor_hex VARCHAR(576) CHARACTER SET ascii,
    IN action_name VARCHAR(8) CHARACTER SET ascii,
    IN request_id CHAR(32) CHARACTER SET ascii,
    IN anchor_id CHAR(32) CHARACTER SET ascii,
    IN proof CHAR(64) CHARACTER SET ascii,
    OUT value BIGINT UNSIGNED
) SQL SECURITY DEFINER MODIFIES SQL DATA
BEGIN
    DECLARE nonce VARCHAR(64) CHARACTER SET ascii;
    DECLARE key_pad VARBINARY(64);
    DECLARE verified VARBINARY(32);
    DECLARE actor_id VARBINARY(288);
    DECLARE duplicate_token BOOL DEFAULT FALSE;
    DECLARE CONTINUE HANDLER FOR 1062 SET duplicate_token = TRUE;

    SET value = NULL;
    IF NOT (@@GLOBAL.gtid_mode = 'ON' AND @@GLOBAL.enforce_gtid_consistency = 'ON'
            AND @@GLOBAL.log_bin = 1 AND @@SESSION.sql_log_bin = 1 AND @@SESSION.binlog_format = 'ROW'
            AND @@GLOBAL.innodb_flush_log_at_trx_commit = 1 AND @@GLOBAL.sync_binlog = 1
            AND @@SESSION.transaction_isolation <> 'READ-UNCOMMITTED') THEN
        SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 60001, MESSAGE_TEXT = 'Durable receipts unavailable';
    END IF;

    SELECT ATTR_VALUE INTO nonce FROM performance_schema.session_connect_attrs
        WHERE PROCESSLIST_ID = CONNECTION_ID() AND ATTR_NAME = 'sdp_wire_nonce';
    SET key_pad = UNHEX(CONCAT('{SIGNING_KEY}', REPEAT('00', 32)));
    SET verified = UNHEX(SHA2(CONCAT(key_pad ^ UNHEX(REPEAT('5c', 64)), UNHEX(SHA2(CONCAT(
        key_pad ^ UNHEX(REPEAT('36', 64)),
        CAST(CONCAT(nonce, ':', CONNECTION_ID(), ':', actor_hex, ':', action_name, ':', request_id, ':', anchor_id)
             AS BINARY)
    ), 256))), 256));
    IF NOT (verified IS NOT NULL AND UNHEX(proof) = verified) THEN
        SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 60002, MESSAGE_TEXT = 'Invalid receipt authorization';
    END IF;
    SET actor_id = UNHEX(actor_hex);

    CASE action_name
        WHEN 'probe' THEN
            SET value = 2;
        WHEN 'anchor' THEN
            INSERT INTO sdp_wire_v1.anchor VALUES (UNHEX(anchor_id));
        WHEN 'reserve' THEN
            INSERT INTO sdp_wire_v1.outcome (actor, token, binding_id)
            VALUES (actor_id, UNHEX(request_id), UNHEX(anchor_id));
        WHEN 'finish' THEN
            IF anchor_id <> REPEAT('0', 32) THEN
                DELETE FROM sdp_wire_v1.anchor WHERE id = UNHEX(anchor_id);
                IF ROW_COUNT() <> 1 THEN
                    SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 60001, MESSAGE_TEXT = 'Missing transaction anchor';
                END IF;
            END IF;
            UPDATE sdp_wire_v1.receipt_sequence SET lsn = lsn + 1 WHERE singleton = 1;
            SELECT s.lsn INTO value FROM sdp_wire_v1.receipt_sequence s WHERE singleton = 1;
            UPDATE sdp_wire_v1.outcome SET lsn = value, confirmed_at = UTC_TIMESTAMP(6)
                WHERE actor = actor_id AND token = UNHEX(request_id) AND lsn IS NULL;
            IF ROW_COUNT() <> 1 THEN
                SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 60001, MESSAGE_TEXT = 'Missing pending receipt';
            END IF;
        WHEN 'occupied' THEN
            SELECT 1 INTO value FROM sdp_wire_v1.outcome
                WHERE actor = actor_id AND token = UNHEX(request_id) FOR SHARE;
            SET value = COALESCE(value, 0);
        WHEN 'lookup' THEN
            SELECT o.lsn INTO value FROM sdp_wire_v1.outcome o
                WHERE actor = actor_id AND token = UNHEX(request_id)
                    AND confirmed_at > UTC_TIMESTAMP(6) - INTERVAL 24 HOUR;
        WHEN 'match' THEN
            SELECT o.lsn INTO value FROM sdp_wire_v1.outcome o
                WHERE actor = actor_id AND token = UNHEX(request_id) AND binding_id = UNHEX(anchor_id)
                    AND confirmed_at > UTC_TIMESTAMP(6) - INTERVAL 24 HOUR;
        ELSE
            SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 60002, MESSAGE_TEXT = 'Invalid receipt action';
    END CASE;
    IF duplicate_token THEN
        SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO = 60003, MESSAGE_TEXT = 'Duplicate receipt token';
    END IF;
END$$
DELIMITER ;
GRANT EXECUTE ON PROCEDURE sdp_wire_v1.receipt TO 'sdp_wire_receipt'@'localhost';
"""


def grants(accounts):
    for account in accounts:
        actor, sep, host = account.rpartition("@")
        if not (sep and re.fullmatch(r"[A-Za-z0-9_.-]{1,32}", actor)
                and re.fullmatch(r"[A-Za-z0-9_.:%-]{1,255}", host)):
            raise ValueError("expected an existing MySQL account as name@host")
        yield f"GRANT EXECUTE ON PROCEDURE sdp_wire_v1.receipt TO '{actor}'@'{host}';"


def provisioning_sql(key, accounts):
    return DDL.replace("{SIGNING_KEY}", key.hex()) + "\n".join(grants(accounts))


def provision(db, key, accounts):
    statements = provisioning_sql(key, accounts)
    with db.cursor(buffered=True) as cursor:
        cursor.execute("SHOW BINARY LOG STATUS")
        status = cursor.fetchone()
        if status is None or status[2] or status[3]:
            raise ValueError("binary logging must be enabled without database filters")
        cursor.execute(statements)
        while cursor.nextset():
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--host", choices=("127.0.0.1", "localhost", "::1"), default="127.0.0.1")
    parser.add_argument("--ca", type=Path, required=True)
    parser.add_argument("--user", default="root")
    parser.add_argument("--key-file", type=Path, required=True)
    parser.add_argument("--grant", action="append", required=True, metavar="USER@HOST")
    args = parser.parse_args()
    try:
        key = read_key(args.key_file)
        password = getpass("MySQL administrator password (Enter for an empty password): ")
        with connector.connect(host=args.host, port=args.port, user=args.user, password=password, use_pure=True,
                               ssl_ca=str(args.ca), ssl_verify_cert=True, ssl_verify_identity=True,
                               allow_local_infile=False, autocommit=True, connection_timeout=5) as db:
            provision(db, key, args.grant)
    except (OSError, ValueError, Error) as error:
        detail = error.errno if isinstance(error, Error) else type(error).__name__
        print(f"Receipt provisioning failed ({detail})", file=sys.stderr)
        return 1
    print("Installed the wire v1 receipt ledger and its procedure grants.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
