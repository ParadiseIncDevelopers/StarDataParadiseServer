"""Exercise the History SQL module against a CA-verified disposable MySQL instance."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import hashlib
import json
import os
import secrets
import subprocess

if __package__:
    from .provision import Column, Table, provision
else:
    from provision import Column, Table, provision


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def client(mysql, port, ca, env):
    common = [
        str(mysql), "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
        f"--port={port}", "--ssl-mode=VERIFY_CA", f"--ssl-ca={ca}",
        "--connect-timeout=3", "--batch", "--raw", "--skip-column-names",
    ]

    def run(sql, *, user="root", password=None, error=None):
        options = ["--skip-password"] if password is None else []
        child_env = dict(env)
        if password is not None:
            child_env["MYSQL_PWD"] = password
        result = subprocess.run(
            [*common, f"--user={user}", *options], env=child_env, input=sql,
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=25,
        )
        if error is not None:
            require(result.returncode != 0 and error.lower() in result.stderr.lower(),
                    f"expected error {error!r}, got {result.stderr[:400]!r}")
            return []
        require(result.returncode == 0, f"MySQL client failed: {result.stderr[:600]}")
        return [line.split("\t") for line in result.stdout.splitlines()]

    return run


def check_instance(run, datadir):
    rows = run("SELECT @@datadir, @@version; SHOW SESSION STATUS LIKE 'Ssl_cipher';")
    require(len(rows) == 2 and Path(rows[0][0]).resolve() == datadir
            and rows[0][1].endswith("-StarDataParadise") and rows[1][1],
            "MySQL test target is not the expected disposable, TLS-protected fork")


def tag(kind, value=None):
    return {"t": kind} if kind == "null" else {"t": kind, "v": value}


def image(value):
    return None if value == "NULL" else json.loads(value)["values"]


def run_checks(run):
    suffix = secrets.token_hex(6)
    table = Table(f"sdp_hist_src_{suffix}", "items", f"sdp_hist_{suffix}", "id",
                  (Column("id", "bigint"), Column("label", "text"), Column("payload", "blob")))
    user = f"writer_{suffix}"
    password = secrets.token_hex(32)
    source = table.source
    journal = table.journal
    run(provision(table))
    run(f"CREATE USER '{user}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;\n"
        f"GRANT SELECT, INSERT, UPDATE, DELETE ON {source} TO '{user}'@'localhost';")
    auth = {"user": user, "password": password}

    def write(sql):
        return run(sql, **auth)

    binary = bytes.fromhex("00ff617e35")
    write(f"INSERT INTO {source} VALUES (1,'first',UNHEX('{binary.hex()}'));")
    write(f"UPDATE {source} SET id=2, label='second' WHERE id=1;")
    write(f"DELETE FROM {source} WHERE id=2;")
    rows = run(f"CALL {journal}.history_log(NULL,NULL,NULL,NULL,NULL,0,32);")
    require(len(rows) == 3 and [row[:2] for row in rows] == [["1", "0"], ["2", "1"], ["3", "2"]],
            "journal lost ordering or skipped an event")
    row_id = rows[0][2]
    require(row_id.isdecimal() and int(row_id) > 0 and all(row[2] == row_id for row in rows),
            "primary-key update changed the historical identity")
    require(all(row[3].startswith(user + "@") and int(row[4]) > 0 for row in rows),
            "journal lost actor or event time")

    summary = {"type": "BLOB", "scope": "value", "size": len(binary),
               "sha256": hashlib.sha256(binary).hexdigest()}
    first = {"id": tag("i64", "1"), "label": tag("text", "first"), "payload": summary}
    second = {"id": tag("i64", "2"), "label": tag("text", "second"), "payload": summary}
    require([tuple(image(value) for value in row[5:]) for row in rows]
            == [(None, first), (first, second), (second, None)],
            "typed images or BLOB summary differ")
    require(run(f"CALL {journal}.history_log(1,3,{row_id},NULL,NULL,1,1);") == rows[1:2],
            "targeted History log pagination failed")
    require(run(f"CALL {journal}.history_log(NULL,NULL,NULL,{max(int(row[4]) for row in rows) + 1},NULL,0,8);") == [],
            "UTC lower bound was not enforced")
    snapshot = run(f"CALL {journal}.history_read(2,{row_id},0,32);")
    require(len(snapshot) == 1 and snapshot[0][0] == row_id and image(snapshot[0][1]) == second,
            "History READ did not reconstruct the selected revision")
    require(run(f"CALL {journal}.history_read(3,{row_id},0,32);") == [],
            "deleted row remained visible in later History READ")
    delta = run(f"CALL {journal}.history_diff(1,2,{row_id},0,32);")
    require(len(delta) == 1 and delta[0][0] == row_id
            and tuple(image(value) for value in delta[0][1:]) == (first, second),
            "History DIFF did not return net images")
    print("PASS typed History LOG/READ/DIFF, stable primary-key identity and scoped BLOB summary", flush=True)

    write(f"START TRANSACTION; INSERT INTO {source} VALUES (3,'abort',NULL); ROLLBACK;")
    require(run(f"SELECT head FROM {journal}.revision_counter;") == [["3"]]
            and write(f"SELECT id FROM {source};") == [],
            "rollback retained data, History events or allocated revisions")
    write(f"START TRANSACTION; INSERT INTO {source} VALUES (2,NULL,NULL); SAVEPOINT keep_change;"
          f"INSERT INTO {source} VALUES (3,'abort',NULL); ROLLBACK TO keep_change; COMMIT;")
    require(write(f"SELECT id FROM {source};") == [["2"]], "savepoint restoration failed")
    kept = run(f"CALL {journal}.history_log(4,4,NULL,NULL,NULL,0,1);")
    require(len(kept) == 1 and kept[0][2] != row_id
            and image(kept[0][6]) == {"id": tag("i64", "2"), "label": tag("null"), "payload": tag("null")},
            "reused primary key reused an identity, NULL lost its type, or savepoint changed history")
    run(f"SELECT after_image FROM {journal}.events;", **auth, error="denied")
    run(f"CALL {journal}.history_read(4,NULL,0,32);", **auth, error="denied")
    run(f"UPDATE {journal}.events SET actor='tampered';", **auth, error="denied")
    run(f"TRUNCATE TABLE {source};", **auth, error="denied")
    print("PASS rollback/savepoint atomicity, identity reuse boundary and SQL privilege isolation", flush=True)

    with ThreadPoolExecutor(max_workers=2) as workers:
        futures = [
            workers.submit(write, f"INSERT INTO {source} VALUES ({key},'concurrent',NULL);")
            for key in (10, 20)
        ]
        for future in futures:
            future.result()
    concurrent = run(f"CALL {journal}.history_log(5,6,NULL,NULL,NULL,0,32);")
    require([row[:2] for row in concurrent] == [["5", "4"], ["6", "5"]]
            and {image(row[6])["id"]["v"] for row in concurrent} == {"10", "20"},
            "concurrent commits did not receive ordered event revisions")
    print("PASS concurrent writers publish a contiguous per-table revision boundary", flush=True)
    return table


def check_after_restart(run, table):
    rows = run(f"CALL {table.journal}.history_log(1,6,NULL,NULL,NULL,0,32);")
    require([row[0] for row in rows] == list("123456")
            and run(f"SELECT id FROM {table.source} ORDER BY id;") == [["2"], ["10"], ["20"]],
            "source data and History events did not both survive restart")
    print("PASS source data and History journal survive a Server restart", flush=True)


def main(check=run_checks, description=__doc__):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--mysql", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--datadir", type=Path, required=True, help="exact disposable Server datadir")
    args = parser.parse_args()
    datadir = args.datadir.resolve(strict=True)
    with_shell = {key: val for key, val in os.environ.items()
                  if key.upper() not in {"MYSQL_PWD", "MYSQL_HOME", "MYSQL_TEST_LOGIN_FILE"}}
    with_shell["MYSQL_TEST_LOGIN_FILE"] = str(datadir / "disabled-login.cnf")
    run = client(args.mysql.resolve(strict=True), args.port, datadir / "ca.pem", with_shell)
    check_instance(run, datadir)
    check(run)


if __name__ == "__main__":
    main()
