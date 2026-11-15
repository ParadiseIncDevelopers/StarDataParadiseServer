import com.stardataparadise.sdk.SdpConnection;
import stardataparadise.exported.ConnectedRoutine;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.sql.DriverManager;
import java.sql.SQLException;
import java.util.Properties;

public final class CheckConnected {
    public static void main(String[] args) throws Exception {
        var config = new Properties();
        config.setProperty("user", "local-admin");
        config.setProperty("caFile", args[1]);
        config.setProperty("tlsName", "localhost");
        config.setProperty("password", new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8)).readLine());
        try (var conn = DriverManager.getConnection("jdbc:sdp://127.0.0.1:" + args[0] , config)
                .unwrap(SdpConnection.class)) {
            conn.setAutoCommit(false);
            try {
                var sets = ConnectedRoutine.invoke(conn, 1L);
                if ("stale".equals(args[2])) throw new AssertionError("stale export accepted");
                long actual = -1;
                for (var set : sets) if (!set.rows().isEmpty()) actual = ((Number) set.rows().get(0).get(0)).longValue();
                if (actual != 7L) throw new AssertionError("typed JDBC result mismatch");
            } catch (SQLException failure) {
                if (!"stale".equals(args[2]) || !"55000".equals(failure.getSQLState())) throw failure;
            } finally {
                conn.rollback();
            }
        }
    }
}
