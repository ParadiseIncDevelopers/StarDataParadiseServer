# Native SQL v1 aktarım sözleşmesi

Bu ek, wire v1 çerçevesini veya sabit CRUD/makbuz sözleşmesini değiştirmez. Sunucu yalnız `--native-sql` ile açıldığında `native_sql_v1: true` bildirir. Native AI ve dil yetenekleri ayrıca backend capability ve kurulum durumuna bağlıdır. Bu aktarımın bulunması bütün SQL ailelerinin desteklendiği anlamına gelmez. Server0.5.0 release'inde SQL HIST/Git ve yeni SQL JOB komutları ertelenmiştir; aşağıdaki eski spike kabulü bu özellikleri etkinleştirmez.

## Oturum ve yürütme

`hello`, `auth`, `capabilities` normal wire v1 akışını kullanır. Her wire oturumu aynı kimlikle açılmış kendi TLS Classic bağlantısına sahiptir. İstemci `native.open {"autocommit":true|false}` çağırır. Oturumda açık legacy transaction, sonuç veya prepared handle bulunmamalıdır. Açılan native oturum yalnız `capabilities` ve `native.*` işlemlerini kabul eder; eski CRUD makbuzu/transaction API'siyle aynı oturumda karıştırılmaz.

`native.execute` alanları:

| Alan | Anlam |
| --- | --- |
| `sql` | En fazla 65.536 UTF-8 byte, NUL içermeyen tek native SQL ifadesi |
| `parameters` | En fazla 128 sıralı, tipli değer; native `?` belirteçleri |
| `timeout_ms` | Backend aktarım süresi, 1.000–300.000 ms; varsayılan 65.000 |
| `max_rows` | Bütün sonuç kümelerindeki toplam satır sınırı, 1–10.000; varsayılan 1.000 |

Parametreler SQL metnine eklenmez. Bağlı komutlar MySQL binary prepared protocol'ünü kullanır. Parametresiz komutun tam kaynağı bir COM_QUERY olarak iletilir. Gateway SQL'i sınıflandırmaz, `:name` dönüştürmez ve procedure gövdesini bölmez. MULTI_STATEMENTS kapalıdır. Procedure içindeki noktalı virgüller native parser'a aittir; istemciye ait `DELIMITER` komutu bu API'ye gönderilmez.

İşlem sınırlarını native SQL belirler. `START TRANSACTION`, `COMMIT`, `COMMIT ASYNC`, `ROLLBACK`, savepoint ve AI kapsamı aynı Classic oturumunda yürür. Sonuç tüketilmesi örtük application commit üretmez. Oturumu kapatmak açık SQL transaction'ını geri alır. Başarıyla commit edilmiş bir AI run bağlantının kapanmasıyla iptal edilmez.

## Tipli değerler

NULL `{"t":"null"}` biçimindedir. Diğer değerler `{"t":"<tag>","v":...}` kullanır:

| Tag | `v` | Not |
| --- | --- | --- |
| `i64` | Ondalık metin | İşaretli 64 bit |
| `u64` | Ondalık metin | İşaretsiz 64 bit |
| `decimal` | Düz ondalık metin | En fazla 65 basamak, 30 kesir basamağı; sessiz yuvarlama yok |
| `float` | Sonlu sayı metni | Native kayan nokta |
| `text` | UTF-8 metin | Geçersiz surrogate reddedilir |
| `binary` | Küçük harfli hex | Byte değeri korunur |
| `json` | Serialize edilmiş JSON metni | JSON içindeki büyük tamsayılar JavaScript Number'a zorla dönüştürülmez |
| `bool` | JSON boolean | Parametre için native tamsayı karşılığı |
| `date` | ISO tarih metni | Sonuç; temsil edilemeyen sıfır tarih açık hata verir |
| `datetime` | SQL tarih-saat metni | Sonuç; saat dilimi native oturum kurallarına aittir |
| `time` | `[-]HH:MM:SS[.ffffff]` | Sonuç; HH 24'ü aşabilir, işaret bütün süreye uygulanır |

Native `JSON` sütun metadata'sı `type:"JSON"` taşır. JSON domain değeri bir metin sütunuymuş gibi etiketlenmez. JSON text'in SQL JSON'a dönüştürülmesi gerekiyorsa parametre hedef tipi veya açık `CAST(? AS JSON)` native binder tarafından denetlenir.

## Sonuçlar ve parçalar

Normal yanıt `sets`, `in_transaction` ve `autocommit` alanlarını içerir. Her kümede `columns`, tipli `rows`, ondalık metin `affected_rows` ve varsa `last_insert_id` bulunur. Column alanları `name`, `type`, `native_type` ve `nullable` olur. CALL'un bütün result/OUT kümeleri sırayla korunur. En fazla 32 küme ve küme başına 128 sütun kabul edilir.

Yanıt bütün olarak en fazla 16 MiB olabilir. Satır/byte sınırını aşmak açık hata üretir; sessiz satır kesme yapılmaz. Büyük yanıt önce backend'den tamamen tüketilir; parça aktarımı yeni SQL yürütmez. Wire v1'in 1 MiB çerçeve sınırı korunur.

Parçalı yanıt `native_result` nesnesi taşır: `id`, byte `offset`, `total_bytes`, SHA-256 `sha256`, base64 `chunk` ve boolean `end`. Her parça en fazla 256 KiB ham veridir. Sonraki istek `native.fetch {"result_id":"...","offset":...}` olur. İstemci kimlik/sıra/boyut/hash değerlerini doğrular; UTF-8 ve sonuç JSON'unu bütün byte'lar birleştikten sonra çözer. Parçalar bitmeden yeni native SQL kabul edilmez. Oturum kapanınca bekleyen bellek sonucu bırakılır.

## Hata ve tekrar deneme

Native hata mevcut zarf içinde `code`, `native_errno`, `sqlstate`, `message`, `unknown_outcome` ve isteğe bağlı `phase`, `commit_state`, `run_state`, `run_id`, `retry` alanlarını taşır. Yeni AI public hata numaraları altı basamaklıdır; eski Classic hata anlamları korunur. Örneğin 511003/61093 bekleme hatası `commit_state:"committed"` ve run kimliğini taşıyabilir. Bekleme süresinin dolması run iptali değildir.

AI araç çağrılarında ek isteğe bağlı alanlar `call_id`, `action_commit_state`,
`cause_native_errno` ve `cause_sqlstate` olur. `commit_state` ilk AI turunun,
`action_commit_state` ayrı araç transaction'ının sonucudur. Örneğin 510005
(Classic61085), AI turu kaydedildikten sonra aracın kendi transaction'ının kesin
olarak geri alındığını bildirebilir: `commit_state:"committed"`,
`action_commit_state:"rolled_back"`, `unknown_outcome:false`. Native yürütücü
başarılı rollback ve hiç COMMIT denenmediği kanıtını sağlar. Asıl timeout3024
gibi hata `cause_native_errno` içinde kalır; üst `native_errno`61085 korunur.
COMMIT denendikten sonraki belirsizlik 511002/61092 ve
`action_commit_state:"unknown"` kullanır. Bağlantı kaybı, gömülü bir false
değerine dayanarak kesin rollback'e çevrilmez. Durum çağrı kimliğiyle sorgulanır;
araç veya sağlayıcı otomatik yeniden yürütülmez.

Prosedür COMMIT edip sonrasında SIGNAL verebilir. Salt statement hatası, daha önceki etkilerin geri alındığını kanıtlamaz. Açık commit bilgisi yoksa istemci sonucu belirsiz kabul eder. Aktarım kopması, limit veya bozuk yanıt sonrasında SQL otomatik tekrar edilmez. Native yol genel CRUD receipt token'ı üretmez. AI `request_key` aynı turun/run'ın tekilleştirilmesi içindir; bağımsız application DML'nin körlemesine yeniden yürütülmesini güvenli yapmaz. Uygulama önce run'ı veya kendi kalıcı işlem kaydını sorgular.

SDK'lar mevcut bağlantı API'lerini koruyarak ayrı native bağlantı sunar: Python `connect_native`, Node `connectNative`, Java `SdpNativeConnection`. .NET karşılığı aynı bağımsız oturum sözleşmesini kullanır. Native API içinde legacy otomatik read retry veya örtük CRUD transaction yönetimi kullanılmaz.

## .NET tüketicisi

`StarDataParadise.SDK.SdpNativeConnection.OpenAsync(connectionString)` ve `SdpFactory.Instance.OpenNativeConnectionAsync(...)` ayrı, havuzlanmayan native oturum açar. `ExecuteAsync(sql, positionalValues)` native `?` parametrelerini kullanır. `BeginAsync`, `RollbackAsync`, `CommitAsync`, `FetchResponseAsync` ve `CancelRunAsync` aynı oturuma native SQL gönderir. Eski `SdpConnection`/ADO.NET ve request-token CRUD receipt yolu korunur.

Exact türler `SdpNativeJson` (serialize JSON metni), `SdpNativeDecimal` (65 basamak/scale 30), `ulong`, `byte[]` ve `SdpNativeTemporal` olur. Yeni temporal wrapper, MySQL TIME'ın işaretli ve 24 saati aşabilen süre anlamını eski civil `SdpTime` ile karıştırmaz. `SdpException.Details`, `NativeErrno`, `Phase`, `CommitState`, `RunState`, `RunId` ve `Retry` sunucunun hata kanıtını korur. Makbuz token'ı bulunmadan da gönderilmiş native isteğin belirsiz sonucu bildirilir.

SDK.NET deposundaki `native-smoke` tüketicisi net8.0, net10.0 ve net48 üzerinde derlendi ve wire sınır testlerini geçti: exact değerler, 650 KiB parça/hash aktarımı, bozuk hash reddi, SQL kaynağının korunması, transaction flag'leri, kayıp COMMIT yanıtı ve structured errors. Kayıtlar Server `out/dotnet-native-net8.log`, `out/dotnet-native-net10.log`, `out/dotnet-native-net48.log` içindedir. Bunlar loopback wire fixture testleridir; gerçek native SQL/TLS kabulünün yerine geçmez.

Modern tüketici `dotnet build native-smoke/NativeSmoke.csproj -c Release --artifacts-path out/native-consumer` ile; framework tüketici aynı komuta `-p:FrameworkFixture=true --artifacts-path out/native-consumer-framework` verilerek oluşturulur. `SDP_NATIVE_CONNECTION` ortam değişkeni doluysa tüketici ayrıca gerçek sunucuda prepared U64/DECIMAL/JSON/BLOB, rollback ve büyük JSON testlerini çalıştırır. Parola bu değişkende tutulur, komut satırına veya loga yazılmaz.

`python -B -m compatibility.tests.dotnet_native_acceptance --mysqld out/hist-native-spike-bin/mysqld.exe` gerçek izole MySQL/TLS + native bridge üzerinden net8.0, net10.0 ve net48 tüketicilerinde PASS verdi (`out/dotnet-native-tls-fifth.log`). Her hedef exact prepared değerleri, signed/25 saati aşan/mikrosaniyeli TIME, prepared CALL/OUT ve iki result set, gerçek InnoDB rollback ve 800 KiB JSON parça aktarımını doğruladı; boundary suite de aynı tüketicide geçti. İlk koşu bridge ortamındaki eksik `mysql.connector` bağımlılığıyla durmuştu; runner mevcut pinli `out/mysql-migration/wire-v1/dependencies` ağacını kullanır. TIME testinin sıfır-fraction canonical metin beklentisi düzeltildi; bir readiness denemesi nonsecret durum teşhisi eklenmeden önce durdu, tekrar geçti. Bu eski spike ordinary SQL kabulü yeni AI/SQL/HIST grammar kabulünün yerine geçmez. Runner varsayılan binary ile son merged motor üzerinde de çalıştırılabilir.
