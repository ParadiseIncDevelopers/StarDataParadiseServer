"""Verify both scripting languages in a Shell linked with the JIT Executor."""

from .fixture import require, run


def verify(fixture):
    scripts = {
        "--js": 'print("SDP_LANGUAGE " + session.runSql("SELECT 6 * 7").fetchOne()[0]);',
        "--py": 'print("SDP_LANGUAGE " + str(session.run_sql("SELECT 6 * 7").fetch_one()[0]))',
    }
    with fixture.scratch("shell-languages") as root, fixture.disposable(root / "server") as instance:
        env, _, shell = instance
        for mode, code in scripts.items():
            invocation = [mode if option == "--py" else option for option in shell]
            result = run([*invocation, f"--execute={code}"], env, 30)
            require(result.returncode == 0 and "SDP_LANGUAGE 42" in result.stdout.splitlines(),
                    f"Shell {mode} query failed: {result.stdout}{result.stderr}")
            print(f"PASS Shell {mode} issued a Classic SQL query against the fork", flush=True)
