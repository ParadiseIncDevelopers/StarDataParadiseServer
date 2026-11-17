"""Run by the Windows Workbench GUI after its native GRT frontend has loaded."""

import json
import os
from pathlib import Path

import grt
import mforms
from workbench.db_utils import MySQLConnection, MySQLError


def connection(config, ca):
    connection = grt.classes.db_mgmt_Connection()
    connection.name = "StarDataParadise Disposable TLS Test"
    connection.owner = grt.root.wb.rdbmsMgmt
    connection.driver = next(driver for driver in grt.root.wb.rdbmsMgmt.rdbms[0].drivers
                             if driver.name == "MysqlNative")
    connection.hostIdentifier = "Mysql@" + "127.0.0.1:" + str(config["port"])
    for parameter, value in {"hostName": "127.0.0.1", "port": config["port"], "userName": config["user"],
                             "schema": config["schema"], "useSSL": 4, "sslCA": ca}.items():
        connection.parameterValues[parameter] = value
    return connection


def fetch(database, statement):
    result = database.executeQuery(statement)
    rows = []
    valid = result.firstRow()
    while valid:
        rows.append([result.stringByIndex(index) for index in range(1, result.numFields() + 1)])
        valid = result.nextRow()
    return rows


def publish_sample_profile(config, typed):
    profile = connection({**config, "user": config["viewer"]}, config["ca"])
    viewer = MySQLConnection(profile, password="")
    viewer.connect()
    try:
        if fetch(viewer, "SELECT id,cost,HEX(payload) FROM sdp_wb_gui.sample") != typed:
            raise RuntimeError("the read-only display account did not return the same sample")
        grants = fetch(viewer, "SHOW GRANTS FOR CURRENT_USER")
        expected = "GRANT SELECT ON `sdp_wb_gui`.`sample` TO `wb_gui_view`@`localhost`"
        if len(grants) != 2 or [expected] not in grants:
            raise RuntimeError("the display account is not restricted to reading the sample table")
    finally:
        viewer.disconnect()
    grt.root.wb.rdbmsMgmt.storedConns.append(profile)
    grt.modules.Workbench.saveConnections()
    grt.modules.Workbench.refreshHomeConnections()
    mforms.App.get().set_status_text("StarDataParadise Server TLS and SQL verified")


def probe(config):
    before = MySQLConnection(connection(config, config["foreign_ca"]), password=config["password"])
    try:
        before.connect()
    except MySQLError as error:
        detail = str(error).lower()
        if not any(term in detail for term in ("ssl", "tls", "certificat")):
            raise RuntimeError("wrong CA connection failed for a reason other than TLS: " + str(error))
    else:
        before.disconnect()
        raise RuntimeError("Workbench native connector accepted an untrusted CA")

    profile = connection(config, config["ca"])
    database = MySQLConnection(profile, password=config["password"])
    database.connect()
    try:
        expected = [[config["version"], config["uuid"]]]
        if fetch(database, "SELECT @@version, @@server_uuid") != expected:
            raise RuntimeError("Workbench GRT reached an unexpected Server")
        [[name, cipher]] = fetch(database, "SHOW SESSION STATUS LIKE 'Ssl_cipher'")
        if name != "Ssl_cipher" or not cipher:
            raise RuntimeError("Workbench GRT did not use TLS")
        typed = fetch(database, "SELECT id,cost,HEX(payload) FROM sdp_wb_gui.sample ORDER BY id")
        if typed != [["9007199254740993", "123.4500", "00FF"]]:
            raise RuntimeError("typed rows differ: " + repr(typed))

        database.execute("SET SESSION sdp_compatibility_mode='legacy'")
        analyze = fetch(database, "ANALYZE sdp_wb_gui.sample")
        if not any(row[1:] == ["analyze", "status", "OK"] for row in analyze):
            raise RuntimeError("legacy ANALYZE did not return OK: " + repr(analyze))

        database.execute("START TRANSACTION")
        database.execute("UPDATE sdp_wb_gui.sample SET cost=200.2500")
        if fetch(database, "SELECT cost FROM sdp_wb_gui.sample") != [["200.2500"]]:
            raise RuntimeError("Workbench did not read its transactional write")
        database.execute("ROLLBACK")
        if fetch(database, "SELECT cost FROM sdp_wb_gui.sample") != [["123.4500"]]:
            raise RuntimeError("Workbench rollback did not restore the committed value")

        if config["show"]:
            publish_sample_profile(config, typed)
        return {"ok": True, "cipher": cipher, "typed": typed, "legacy_analyze": True, "rollback": True,
                "wrong_ca_rejected": True}
    finally:
        database.disconnect()


def main():
    settings = Path(os.environ["SDP_WB_GUI_FIXTURE"])
    config = json.loads(settings.read_text(encoding="utf-8"))
    try:
        success = probe(config)
    except Exception as error:
        message = str(error).replace(config["password"], "<redacted>")
        success = {"ok": False, "error": f"{type(error).__name__}: {message}"}
    Path(config["result"]).write_text(json.dumps(success), encoding="utf-8")


main()
