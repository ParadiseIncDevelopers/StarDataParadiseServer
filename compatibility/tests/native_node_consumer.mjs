// Loaded against the packed/installed SDK, never its source checkout.
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const {connectNative,NativeJson,NativeDecimal,NativeTemporal}=await import(pathToFileURL(process.env.SDP_NATIVE_NODE_ENTRY));
const db=await connectNative(JSON.parse(process.env.SDP_NATIVE_SETTINGS));
try {
  const first=async(sql,values=[]) => (await db.execute(sql,values)).sets[0].rows[0];
  const precise='12345678901234567890123456789012345.123456789012345678901234567890';
  const row=await first("SELECT CAST(? AS JSON),CAST(? AS DECIMAL(65,30)),CAST(? AS UNSIGNED),UNHEX('00FF'),NULL",
    [new NativeJson('{"id":9007199254740993}'),new NativeDecimal(precise),(1n<<64n)-1n]);
  assert(row[0] instanceof NativeJson);assert(row[0].text.includes('9007199254740993'));
  assert.equal(row[1].text,precise);assert.equal(row[2],(1n<<64n)-1n);
  assert.deepEqual(row[3],Buffer.from([0,255]));assert.equal(row[4],null);
  for (const value of [Buffer.from([0,1,254,255]),Buffer.alloc(0),Buffer.from('Türkçe'), 'Türkçe - 🌌',null]) {
    const [bound]=await first('SELECT ?',[value]);
    if(Buffer.isBuffer(value)) assert.deepEqual(bound,value);
    else assert.equal(bound,value);
  }
  for(const text of ['-01:02:03.000004','25:00:00','-00:00:00.000001']) {
    const [time]=await first('SELECT CAST(CAST(? AS CHAR) AS TIME(6))',[text]);
    assert(time instanceof NativeTemporal);assert.equal(time.text,text);
  }
  await db.execute('CREATE TEMPORARY TABLE native_consumer(id BIGINT PRIMARY KEY,note TEXT) ENGINE=InnoDB');
  await db.begin();await db.execute('INSERT INTO native_consumer VALUES (?,?)',[1n,"'); DELETE FROM native_consumer; --"]);
  await db.rollback();assert.equal((await first('SELECT COUNT(*) FROM native_consumer'))[0],0n);
  await db.execute('CREATE PROCEDURE native_consumer_node(IN p BIGINT,OUT q BIGINT) BEGIN SET q=p+1; SELECT q; SELECT p; END');
  const result=await db.execute('CALL native_consumer_node(?,@node_out)',[7n]);
  assert.deepEqual(result.sets.filter(part=>part.rows.length).map(part=>part.rows[0][0]),[8n,7n]);
  assert.equal((await first('SELECT @node_out'))[0],8n);
  await db.execute("CREATE PROCEDURE native_text_out_node(OUT q VARCHAR(32) CHARACTER SET utf8mb4) SET q='Türkçe'");
  const declared=await db.execute('CALL native_text_out_node(?)',[Buffer.from([255])]);
  assert.deepEqual(declared.sets.filter(part=>part.rows.length).map(part=>part.rows[0][0]),['Türkçe']);
  await db.execute('CREATE FUNCTION native_short_node(p BIGINT) RETURNS BIGINT DETERMINISTIC AS RETURN :p+1');
  assert.equal((await first('SELECT native_short_node(?)',[9n]))[0],10n);
  const [huge]=await first("SELECT JSON_OBJECT('value',REPEAT('İğ',200000))");
  assert(huge instanceof NativeJson);assert.equal(JSON.parse(huge.text).value,'İğ'.repeat(200000));
  console.log('PASS installed Node native TLS: JSON/U64/DECIMAL/BLOB/NULL/TIME, rollback, prepared CALL/OUT, AS RETURN and chunked UTF-8');
} finally { db.close(); }
