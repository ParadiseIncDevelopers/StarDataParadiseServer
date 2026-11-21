import com.stardataparadise.sdk.SdpNativeConnection;
import com.stardataparadise.sdk.SdpNativeConnection.Json;
import com.stardataparadise.sdk.SdpNativeConnection.Temporal;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.List;

/** External installed-JAR consumer; connection credentials are never command arguments. */
public final class NativeJavaConsumer {
    private static void require(boolean value,String label) {if(!value)throw new AssertionError(label);}
    private static List<Object> first(SdpNativeConnection db,String sql,Object...values) throws Exception {
        return db.execute(sql,values).sets().get(0).rows().get(0);
    }
    public static void main(String[] args) throws Exception {
        JsonObject config=JsonParser.parseString(System.getenv("SDP_NATIVE_SETTINGS")).getAsJsonObject();
        try(SdpNativeConnection db=SdpNativeConnection.open(config.get("host").getAsString(),config.get("port").getAsInt(),
            config.get("tlsName").getAsString(),Path.of(config.get("caFile").getAsString()),config.get("user").getAsString(),
            config.get("password").getAsString(),75000,true)) {
            String precise="12345678901234567890123456789012345.123456789012345678901234567890";
            BigInteger maximum=BigInteger.ONE.shiftLeft(64).subtract(BigInteger.ONE);
            List<Object> row=first(db,"SELECT CAST(? AS JSON),CAST(? AS DECIMAL(65,30)),CAST(? AS UNSIGNED),UNHEX('00FF'),NULL",
                new Json("{\"id\":9007199254740993}"),new BigDecimal(precise),maximum);
            require(row.get(0) instanceof Json && ((Json)row.get(0)).value().getAsJsonObject().get("id").getAsBigInteger().equals(new BigInteger("9007199254740993")),"JSON digits");
            require(row.get(1).equals(new BigDecimal(precise)),"DECIMAL digits");require(row.get(2).equals(maximum),"U64");
            require(Arrays.equals((byte[])row.get(3),new byte[]{0,(byte)255}),"BLOB");require(row.get(4)==null,"SQL NULL");
            for(byte[] value:List.of(new byte[]{0,1,(byte)254,(byte)255},new byte[0],"Türkçe".getBytes(java.nio.charset.StandardCharsets.UTF_8)))
                require(Arrays.equals((byte[])first(db,"SELECT ?",(Object)value).get(0),value),"bound binary bytes and type");
            require(first(db,"SELECT ?","Türkçe - 🌌").get(0).equals("Türkçe - 🌌"),"bound text type");
            require(first(db,"SELECT ?",(Object)null).get(0)==null,"bound SQL NULL");
            for(String text:List.of("-01:02:03.000004","25:00:00","-00:00:00.000001")) {
                Object time=first(db,"SELECT CAST(CAST(? AS CHAR) AS TIME(6))",text).get(0);
                require(time instanceof Temporal && ((Temporal)time).text().equals(text),"TIME interval");
            }
            db.execute("CREATE TEMPORARY TABLE native_consumer(id BIGINT PRIMARY KEY,note TEXT) ENGINE=InnoDB");
            db.begin();db.execute("INSERT INTO native_consumer VALUES (?,?)",BigInteger.ONE,"'); DELETE FROM native_consumer; --");
            db.rollback();require(first(db,"SELECT COUNT(*) FROM native_consumer").get(0).equals(0L),"rollback");
            db.execute("CREATE PROCEDURE native_consumer_java(IN p BIGINT,OUT q BIGINT) BEGIN SET q=p+1; SELECT q; SELECT p; END");
            var sets=db.execute("CALL native_consumer_java(?,@java_out)",BigInteger.valueOf(7)).sets();
            var emitted=sets.stream().filter(part->!part.rows().isEmpty()).map(part->part.rows().get(0).get(0)).toList();
            require(emitted.equals(List.of(8L,7L)),"CALL sets");
            require(first(db,"SELECT @java_out").get(0).equals(8L),"OUT");
            db.execute("CREATE PROCEDURE native_text_out_java(OUT q VARCHAR(32) CHARACTER SET utf8mb4) SET q='Türkçe'");
            var declared=db.execute("CALL native_text_out_java(?)",(Object)new byte[]{(byte)255}).sets();
            require(declared.stream().filter(part->!part.rows().isEmpty()).map(part->part.rows().get(0).get(0)).toList().equals(List.of("Türkçe")),"declared text OUT overrides binary input");
            db.execute("CREATE FUNCTION native_short_java(p BIGINT) RETURNS BIGINT DETERMINISTIC AS RETURN :p+1");
            require(first(db,"SELECT native_short_java(?)",BigInteger.valueOf(9)).get(0).equals(10L),"AS RETURN");
            Json huge=(Json)first(db,"SELECT JSON_OBJECT('value',REPEAT('İğ',200000))").get(0);
            require(huge.value().getAsJsonObject().get("value").getAsString().equals("İğ".repeat(200000)),"chunked UTF8");
            System.out.println("PASS installed Java native TLS: JSON/U64/DECIMAL/BLOB/NULL/TIME, rollback, prepared CALL/OUT, AS RETURN and chunked UTF-8");
        }
    }
}
