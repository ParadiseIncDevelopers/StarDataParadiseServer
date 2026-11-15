"""Verify real debugger suspension and peer-disconnect rollback over loopback TLS."""

import argparse
from pathlib import Path
import secrets
import ssl
import tempfile
import time

from security_rpc import CANCELLED, receive_reply
from service_smoke import certificates, execute, files_in, listening, require, save_config
from sql_rpc import BIGINT, INVALID_STATE, client, i64, token, write


SOURCE_LINE = 3
OWNER_TIMEOUT = 20_000
CLEANUP_SECONDS = 2
PROCEDURE = """CREATE PROCEDURE debug_hold(output_value OUT BIGINT) AS
BEGIN
    output_value := 7;
END;"""


WORK_PROCEDURE = """CREATE PROCEDURE debug_work(p_input IN BIGINT,p_out OUT BIGINT) AS DECLARE
local_value BIGINT := p_input;
BEGIN
local_value := local_value + 2;
p_out := (SELECT id FROM debug_items WHERE id = :local_value);
END;"""


def session_summaries(controller):
    return controller.call("session_list")["sessions"]


def pause_invocation(debuggee, controller, key):
    debuggee.call("tx_begin")
    pending = debuggee.mutate(f"INSERT INTO pending_rows VALUES ({key})")
    require(pending["affected_rows"] == 1 and "lsn" not in pending, "fixture write was not staged")
    rows, _ = debuggee.query("SELECT id FROM pending_rows")
    require(rows == [[i64(key)]], "debuggee could not see its staged row")
    active = [session for session in session_summaries(controller) if session["transaction_active"]]
    require(len(active) == 1, "one debuggee must own the only transaction")
    session_id = active[0]["session_id"]

    prepared = debuggee.call("query.prepare", sql="CALL debug_hold(NULL)")["statement_id"]
    binding = controller.call("debug_open", session_id=session_id, routine="main.public.debug_hold", revision=1)
    debug_id = binding["debug_id"]
    controller.call("debug_breakpoint", debug_id=debug_id, line=SOURCE_LINE, revision=1)
    debuggee.send_without_receiving("execute", statement_id=prepared)
    report = await_pause(controller, debug_id, SOURCE_LINE, "breakpoint")
    require(report["depth"] == 1, "debuggee did not pause at the pinned instruction")
    active = [summary for summary in session_summaries(controller)
              if summary["session_id"] == session_id and summary["transaction_active"]]
    require(len(active) == 1 and active[0]["request_id"] == debuggee.id,
            "suspended invocation did not hold the caller's lease")
    return session_id


def await_pause(controller, debug_id, line, reason):
    deadline = time.monotonic() + CLEANUP_SECONDS
    while time.monotonic() < deadline:
        report = controller.call("debug_status", debug_id=debug_id)
        if report["state"] == "paused":
            require(report["line"] == line and report["reason"] == reason,
                    "debugger stopped at the wrong instruction")
            return report
        require(report["state"] in ("armed", "running"), "invocation exited before reaching its instruction")
        time.sleep(0.01)
    raise RuntimeError("debugger did not reach the live instruction")


def test_normal_resume(port, context, password):
    with client(port, context, password) as controller:
        existing = {session["session_id"] for session in session_summaries(controller)}
        with client(port, context, password) as target:
            added = {session["session_id"] for session in session_summaries(controller)} - existing
            require(len(added) == 1, "debugger target session could not be identified")
            target_id = added.pop()
            with target.prepared("CALL debug_work(:p_value,NULL)", {"p_value": BIGINT}) as statement:
                debug_id = controller.call("debug_open", session_id=target_id, routine="debug_work",
                                           revision=1)["debug_id"]
                controller.call("debug_options", debug_id=debug_id, exception_stop=False)
                controller.call("debug_breakpoint", debug_id=debug_id, line=4, revision=1)
                target.send_without_receiving("execute", statement_id=statement, parameters={"p_value": i64(5)})
                await_pause(controller, debug_id, 4, "breakpoint")
                require(controller.call("debug_watch", debug_id=debug_id, name="local_value")["value"] == i64(5),
                        "debugger did not expose the pre-step local")
                controller.call("debug_step", debug_id=debug_id, mode="over")
                await_pause(controller, debug_id, 5, "step")
                require(controller.call("debug_watch", debug_id=debug_id, name="local_value")["value"] == i64(7),
                        "debugger did not expose the updated local")
                controller.call("debug_continue", debug_id=debug_id)
                completed = receive_reply(target)
                require(completed["ok"], f"resumed execute failed with {completed.get('error', {}).get('code')}")
                result = completed["body"]
                require(result["columns"] == [{"name": "p_out", "type": "BIGINT"}],
                        "routine OUT metadata changed after resume")
                returned, _ = target.collect(result, 1, 2)
                require(returned == [[i64(7)]], "hosted SELECT did not return its row after resume")

            samples = target.call("profile_last")["samples"]
            require(len(samples) == 1 and samples[0]["routine"][-1] == "debug_work"
                    and samples[0]["revision"] == 1 and samples[0]["instructions"] >= 2
                    and samples[0]["inclusive_us"] >= samples[0]["exclusive_us"] >= samples[0]["sql_us"] > 0,
                    "profile did not report actual time for resumed hosted SQL")
            require(controller.call("debug_status", debug_id=debug_id)["state"] == "finished",
                    "normal completion left the debugger active")
            controller.call("debug_close", debug_id=debug_id)
            require(target.call("health")["status"] == "ready", "normal debugger resume left the peer closed")

def check_lease(controller, deadline):
    require(time.monotonic() < deadline, "disconnect cleanup exceeded its deadline")
    controller.call("tx_begin")
    rows, _ = controller.query("SELECT id FROM pending_rows")
    require(rows == [], "peer disconnect committed the debuggee's private row")
    controller.call("tx_rollback")
    require(time.monotonic() < deadline, "disconnected debugger retained the transaction lease")


def test_debuggee_disconnect(port, context, password):
    with client(port, context, password) as controller:
        with client(port, context, password) as debuggee:
            session_id = pause_invocation(debuggee, controller, 1)
            deadline = time.monotonic() + CLEANUP_SECONDS
        while time.monotonic() < deadline:
            if all(session["session_id"] != session_id for session in session_summaries(controller)):
                check_lease(controller, deadline)
                return
            time.sleep(0.01)
        raise RuntimeError("disconnect did not remove the paused debuggee before the pause timeout")


def test_controller_disconnect(port, context, password):
    with client(port, context, password) as observer, client(port, context, password) as debuggee:
        with client(port, context, password) as controller:
            session_id = pause_invocation(debuggee, controller, 2)
            deadline = time.monotonic() + CLEANUP_SECONDS
        result = receive_reply(debuggee)
        require(not result["ok"] and result["error"]["code"] == CANCELLED,
                "closing the controller did not cancel the actual suspended execution")
        active = next((session for session in session_summaries(observer) if session["session_id"] == session_id), None)
        require(active is not None and not active["transaction_active"],
                "controller close did not clear the debuggee's transaction")
        check_lease(observer, deadline)
        debuggee.error("tx_commit", INVALID_STATE, request_token=token())
        require(debuggee.call("health")["status"] == "ready", "controller close dropped the debuggee's connection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--openssl", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    server = str(args.server.resolve(strict=True))
    openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s18-", dir=args.scratch) as directory:
        fixture = Path(directory)
        for name in ("data", "log", "temp"):
            (fixture / name).mkdir()
        certificates(openssl, fixture)
        config = save_config(fixture, timeout_ms=OWNER_TIMEOUT, max_connections=6)
        require(execute(server, config, "--initialize-storage").returncode == 0, "debug storage setup failed")
        password = secrets.token_urlsafe(32)
        password_input = ((password + "\n") * 2).encode("ascii")
        require(execute(server, config, "--bootstrap-admin", "local-admin", stdin=password_input).returncode == 0,
                "debug administrator setup failed")
        context = ssl.create_default_context(cafile=str(fixture / "ca.pem"))

        with listening(server, config) as (_, port):
            with client(port, context, password) as setup:
                write(setup, "CREATE TABLE pending_rows (id BIGINT PRIMARY KEY)", 0)
                write(setup, PROCEDURE, 0)
                write(setup, "CREATE TABLE debug_items (id BIGINT PRIMARY KEY)", 0)
                write(setup, "INSERT INTO debug_items VALUES (7)", 1)
                write(setup, WORK_PROCEDURE, 0)
        committed = files_in(fixture / "data")

        with listening(server, config) as (_, port):
            test_normal_resume(port, context, password)
            test_debuggee_disconnect(port, context, password)
            test_controller_disconnect(port, context, password)

        require(execute(server, config, "--check-storage").returncode == 0, "debug fixture failed WAL/storage verification")
        require(files_in(fixture / "data") == committed, "disconnect changed committed data or WAL")

    print("Debugger TLS normal resume, hosted SQL, profile and both peer-disconnect rollbacks passed")


if __name__ == "__main__":
    main()
