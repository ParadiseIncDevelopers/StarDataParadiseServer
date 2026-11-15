# M3 SQL çekirdeği ve standart güvenlik

**PASSED · Windows x64 · Etap 11–15**

| Etap | Doğrulanan sonuç | Kayıt |
| --- | --- | --- |
| 11 | Katalog ve DDL rollback/restart, tipli SQL planı, NULL/decimal ve kaynak konumlu denetleyici. | [11](STAGES/11.json) |
| 12 | Gerçek native TLS CRUD, tipli parametreler, bounded fetch, savepoint ve kalıcı commit sonucu. | [12](STAGES/12.json) |
| 13 | Kalıcı B+Tree equality/range, FK/UNIQUE/CHECK, identity ve DROP/TRUNCATE before-image kaydı. | [13](STAGES/13.json) |
| 14 | JOIN, aggregate, CTE/UNION, gerçek EXPLAIN ANALYZE, Türkçe collation ve REINDEX. | [14](STAGES/14.json) |
| 15 | Kalıcı kullanıcı ve rol kataloğu, canlı GRANT/REVOKE, reset, gerçek lock wait ve iptal. | [15](STAGES/15.json) |

## Son regresyon

| Proje | Windows x64 sonucu |
| --- | --- |
| Server | 20/20 CTest ve mevcut Visual Studio çözümü. AddressSanitizer ile storage, WAL, recovery, SQL ve oturum güvenliği 7/7. [Release](../../out/sdp-m3/stage15/ctest-final.log) · [ASan](../../out/sdp-m3/stage15/asan/ctest.log) |
| Workbench | 2/2 CTest. Mevcut native 0.1.0 tüketicisi Etap 15 Server’a bağlandı. [Log](../../out/sdp-m3/stage15/client-regression/workbench/ctest.log) |
| CLI | 4/4 CTest. Mevcut native 0.1.0 tüketicisi Etap 15 Server’a bağlandı. [Log](../../out/sdp-m3/stage15/client-regression/cli/ctest.log) |
| Native SDK | 2/2 CTest, C ABI 1 export uyumu ve bağımsız Release/ASan paketiyle gerçek TLS, SQL ve güvenlik. [Release](../../../StarDataParadiseDLLler/out/sdp-m3/stage15/security-integration.log) · [ASan](../../../StarDataParadiseDLLler/out/sdp-m3/stage15/asan/security-integration.log) |
| .NET SDK | NuGet paket üretimi ve assembly yükleme. [Log](../../../StarDataParadise.SDK.NET/out/sdp-m3/milestone/test.log) |
| Java SDK | Gradle çevrimdışı JAR üretim ve yükleme testi. [Log](../../../StarDataParadise.SDK.Java/out/sdp-m3/milestone/test.log) |
| Python SDK | Yerel wheel üretimi, kurulum ve sürüm testi. [Log](../../../StarDataParadise.SDK.Python/out/sdp-m3/milestone/test.log) |

Wire v1 taban arşivi ve disk/WAL v1 korundu. İstemciler yeni SQL ve güvenlik çağrılarını ek capability’ler üzerinden keşfeder. Stage 08 ve 09 storage fixture’ları son regresyonda tekrar açıldı. Paket SHA-256 değerleri ve çalıştırılan komutlar Etap 15 kaydında. [SQL uyumluluğu](../../SQL_COMPATIBILITY.md) · [SQL güvenliği](../../SQL_SECURITY.md)

Native 0.2.0 Windows x64 paketi yerel sürüm adayıdır. Sıradaki etap tipli PL/SQL ve rutin yürütücüsüdür. [Paket](../../../StarDataParadiseDLLler/out/sdp-m3/stage15/package/StarDataParadiseDLLler-0.2.0-win-x64.zip) · [Sıradaki etap](NEXT_TASK.md)
