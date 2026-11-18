# Yerel wire v1 servisi

`service.py`, StarDataParadise SDK’larının wire v1 bağlantılarını Server’ın Classic uç noktasına iletir. İstemci, Server’da yetkilendirilmiş hesabını kullanır. Her wire bağlantısı kendi Classic oturumuna sahiptir. Arka uç sertifikası ve sunucu adı TLS ile, `@@server_uuid` ve Server sürümü oturum açılışında doğrulanır. Servis yalnızca `127.0.0.1` üzerinde dinler ve varsayılan olarak SDK bağlantılarında da TLS ister.

Python 3.12 ile bağımlılığı ayrı bir sanal ortamda kurun. `--mysql-uuid` için hedef Server’da `SELECT @@server_uuid` çıktısını, `--schema` için uygulamanın InnoDB şemasını kullanın. Arka uç sertifikasının SAN alanı `--mysql-host` ile eşleşmelidir. SDK, yerel servisin sertifikasını imzalayan CA’yı ve sertifikada bulunan sunucu adını kullanır.

```powershell
python -m venv out/wire-v1-env
out/wire-v1-env/Scripts/python.exe -m pip install --require-hashes -r compatibility/wire_v1/requirements.txt
out/wire-v1-env/Scripts/python.exe -B compatibility/wire_v1/service.py --port 7401 --cert wire-server.pem --key wire-server.key --mysql-port 3306 --mysql-ca mysql-ca.pem --mysql-uuid 'server-uuid' --schema 'schema'
```

Yerel .NET geliştirme bağlantısı ve Workbench için `3197` portunu `--plaintext-loopback --share-classic-loopback` ile açın. StarDataParadiseDrive 2.0.2, `TLS Mode=Disabled` ile ortak portta wire v1 kullanır. Bu kipte SDK parolayı şifresiz iletir. Workbench **Standard (TCP/IP)** ve **Require** ile bağlanır. Classic oturumunun TLS ve kimlik doğrulama ayarlarını arka uç belirler. Örnek komuttaki `--mysql-ca` yalnızca servisin arka uca bağlanırken kullandığı CA’dır.

```powershell
out/wire-v1-env/Scripts/python.exe -B compatibility/wire_v1/service.py --port 3197 --plaintext-loopback --share-classic-loopback --mysql-port 41754 --mysql-ca mysql-ca.pem --mysql-uuid 'server-uuid' --schema 'schema' --ledger-key-file ledger.key
```

`wire_sql_basic_v1` yeteneği `query.prepare`, `execute`, `fetch`, `statement_close`, `result_cancel`, `session_reset` ve temel `tx_*` işlemlerini bildirir. Servis `SELECT` ve açık işlemlerde `INSERT`, `UPDATE`, `DELETE` kabul eder. `:name` biçimindeki bağlar yerel MySQL binary prepared statement protokolüne aktarılır. Desteklenen değerler NULL, 64 bit işaretli tamsayı, metin, binary ve 38 basamak sınırında exact decimal değerleridir. Eski `BOOLEAN` parametresi MySQL tamsayısına bağlanır. Sonuçlar, MySQL tür eşleşmesine göre paketlenir. İşlem garantileri için InnoDB tabloları kullanın.

Servis tek SQL ifadesi kabul eder. Bir ifadenin sonuna noktalı virgül eklenebilir. Yorumlar, ham `?` belirteçleri, DDL, timestamp ve bileşik wire değerleri kapsam dışıdır. Dizgelerdeki tek tırnak `''` ile yazılır. `sql_crud_v1` yeteneği tam eski tip, katalog ve SQL sözleşmesi için ayrılmıştır.

Bağımsız `SELECT` işlemleri, sonuç tüketilene veya kapatılana kadar MySQL `READ ONLY` işlemi içinde çalışır. Servis `SELECT INTO` ve kilit alan `SELECT` biçimlerini reddeder. Arka uç izinleri, SQL işlevlerinin çalıştırılmasını ve dosya gibi harici kaynaklara erişimi belirler. SDK hesabına yalnız uygulamanın gerekli tablo ve güvenilen işlev izinlerini verin.

## Kalıcı işlem sonuçları

MySQL Server’ı `--log-bin`, `--gtid-mode=ON`, `--enforce-gtid-consistency=ON` ve `--performance-schema=ON` ile başlatın. `binlog_format=ROW`, `sync_binlog=1` ve `innodb_flush_log_at_trx_commit=1` kullanın. Binary log için veritabanı filtreleri tanımlamayın. PITR için binary log dosyalarının saklanmasını ayrıca yapılandırın.

32 rastgele baytı küçük harfli hex olarak içeren bir anahtar dosyası oluşturun. Dosyayı servisin hesabına ve Server yöneticisine özel tutun. Yönetici bu dosyayı kullanarak ledger’ı bir kez kurar ve wire hesaplarına yalnızca yordamı çalıştırma izni verir. Bu hesapların izinlerini ilgili uygulama şemasına ve güvenilen yordamlara sınırlayın. Global `SELECT`, `SHOW_ROUTINE` ve Performance Schema bağlantı özniteliklerini okuma izni yalnız yönetici hesaplarında bulunmalıdır. `sdp_wire_v1` şeması, yordam tanımı ve erişim anahtarı Server’la birlikte korunmalıdır. Bir MySQL hesabının `name@host` kimliğini yeni bir kullanıcı için yeniden kullanırsanız geçmiş sonuçlar da aynı etkili hesap kimliğine aittir.

```powershell
out/wire-v1-env/Scripts/python.exe -B compatibility/wire_v1/provision_ledger.py --port 3306 --ca mysql-ca.pem --key-file ledger.key --grant 'writer@localhost'
out/wire-v1-env/Scripts/python.exe -B compatibility/wire_v1/service.py --port 7401 --cert wire-server.pem --key wire-server.key --mysql-port 3306 --mysql-ca mysql-ca.pem --mysql-uuid 'server-uuid' --schema 'schema' --ledger-key-file ledger.key
```

Başarılı yordam kontrolü `wire_sql_transactions_v1` yeteneğini açar. Autocommit `INSERT`, `UPDATE` ve `DELETE` için yeni bir `request_token` gönderin. `tx_begin` seri hale getirilebilir bir işlem başlatır. İşlem içindeki sorguları tokensız, `tx_commit` isteğini ise yeni bir token ile gönderin. Token 32 küçük harfli hex karakterden oluşur. Aynı hesaptaki yinelenen token `INVALID_STATE` döndürür. Commit sonucu ile ilgili InnoDB değişiklikleri tek işlemde kesinleşir. `lsn`, bu kayıtların sıralı kimliğidir. MySQL redo LSN’sinden bağımsızdır.

Bağlantı veya yanıt kaybolduğunda aynı MySQL hesabıyla yeniden bağlanıp `tx_outcome` çağırın. `committed` ile `lsn`, 24 saat içinde kesinleşmiş kaydı doğrular. `unknown`, kaydın bulunmadığı ya da süresinin dolduğu anlamına gelir. Süresi dolan token tekrar kullanılamaz. Yordam kontrolü başarısız olursa bu ledger ile başlatılmış servis ilgili hesap için yazmayı kapatır. JDBC’nin `wire_sql_transactions_v1` desteği bulunan yerel derlemesi bu yetenekle yazma ve elle işlem kiplerini açar.

## Eski RPC işlemleri

Desteklenen RPC’ler açık sunucu yapılandırmasıyla devreye girer. `--legacy-rpc-config` bir JSON dosyası alır. Hesaplar MySQL’in döndürdüğü etkin `CURRENT_USER()` adına göre eşleşir. History apply ile bağımsız compute submit ve schedule oluşturma için güncel sürüm 2 ledger kurulur. Önceki ledger, temel SQL işlem sonucunu ve mevcut kayıtlarını sunmaya devam eder.

```json
{
  "jobs": ["operator@localhost"],
  "history": {
    "items": {
      "table": "items",
      "journal": "items_history",
      "object_id": "41",
      "readers": ["operator@localhost", "reader@localhost"],
      "writers": ["operator@localhost"]
    }
  }
}
```

- History `log`, `read` ve `diff` çağrıları kurulu tablo journal’ını okur. Her hedefe ayrı journal verin. `object_id`, yönetici tarafından sabitlenen pozitif ve benzersiz uyumluluk kimliğidir. Canlı satırdaki restore/revert için zorunludur. Yetkileri [History kılavuzuna](../history/README.md) göre verin. Apply, değişiklik ile kalıcı wire kaydını birlikte kesinleştirir. Aynı plan ve token 24 saat içinde başka oturumda tekrar okunabilir. Log’daki `observed_utc` tetikleyicinin olay zamanıdır. Eski commit-zamanı filtreleri ve katalog geçmişi kapsam dışıdır.
- Compute RPC, `compute_jobs.py install` ile hazırlanmış InnoDB kuyruğunu kullanır. Hesabı güvenilir işletim hesabı olarak yetkilendirin. Doğrudan MySQL tablo izinlerinin kapsamı RPC’nin sahip filtresinden daha geniştir. UTC interval zamanlamasında `misfire: "run_once"` açıkça seçilir. Autocommit submit ve schedule oluşturma kalıcı `lsn` döndürür. Açık işlemdeki submit, çağıranın commit veya rollback sonucuna bağlıdır. İşlem içindeki job token’ı için ayrı commit kaydı oluşmaz.
- `--legacy-decision-operator USER@HOST`, seçilen hesapta `decision_validate` ile desteklenen statik kuralı ve gerçek tablo/sütun türlerini denetler.
- `--legacy-model-worker PATH`, mevcut CPU model kaydında etkinleştirme, rollback, devre dışı bırakma ve desteklenen tahmin çağrılarını açar. Model değişikliği kalıcı ledger ve çağıranın kaynak erişimini gerektirir.
- Fiziksel arşiv RPC’si olan `backup_catalog`, `backup_verify` ve `backup_restore` için [yedek yapılandırmasına](../backup/PHYSICAL.md) bakın.

Eski yetenek bayrakları tüm ilgili çağrılar ve veri biçimleri hazır olduğunda açılır. `legacy_rpc_acceptance.py` History ve compute işlemlerini ayrı bir Server’da çalıştırır.

## Canlı kabul

`acceptance.py`, ayrı bir boş InnoDB örneği ve iki geçici hesap üzerinde çalışır. Yeni Server’da `wire_sample(id BIGINT PRIMARY KEY, note TEXT, amount DECIMAL(38,4), payload BLOB)` tablosunu oluşturun. Yazıcı hesaba `SELECT, INSERT, UPDATE, DELETE`, okuyucu hesaba `SELECT` verin. Script, iki test parolasını sırayla standart girdiden alır. Her çalıştırma öncesinde tablo boş olmalıdır. Test kalıcı bir örnek satırı yazar.

```powershell
out/wire-v1-env/Scripts/python.exe -B compatibility/wire_v1/acceptance.py --port 7401 --ca wire-ca.pem --owner 'writer' --reader 'reader'
```

Kalıcı sonuç kabulü, kendi TLS sertifikalarını ve geçici MySQL örneğini oluşturur. Eşzamanlı yinelenen token’ı, hesap yalıtımını, yanıt kaybını, rollback işlemini ve test Server’ı kesildikten sonraki kurtarmayı doğrular. Gerekirse `--mysqld`, `--mysql`, `--basedir` ve `--openssl` ile yerel araç yollarını verin.

```powershell
out/wire-v1-env/Scripts/python.exe -B compatibility/wire_v1/durable_fork.py
```
