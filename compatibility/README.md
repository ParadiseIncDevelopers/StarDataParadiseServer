# MySQL tabanında uyumluluk

Bu dizindeki araçlar MySQL tabanlı StarDataParadise Server ile çalışır. SQL araçlarını kaynaktan derlenen Windows CLI’ın Python kipinde, ilgili yetkilere sahip bir MySQL hesabıyla çalıştır. Uzak bağlanırken `VERIFY_IDENTITY` ve sunucunun CA dosyasını kullan. Üretimde `sdp_jobs`, `sdp_models` ve History journal tablolarına doğrudan DML erişimini güvenilir işletim hesaplarıyla sınırla.

- [Wire v1 TLS hizmeti ve SDK kontrolleri](wire_v1/README.md)
- [History kurulumu, `LOG`, `READ` ve `DIFF`](history/README.md)
- [Decision doğrulama kuralı ve MySQL tetikleyicisi](decision/README.md)
- [PL/SQL BIGINT `IN`, `OUT`, `IN OUT` yordam aktarımı](plsql/PROCEDURE.md)
- [Yedek, compute, PL/SQL ve model canlı kabul koşucusu](tests/README.md)
- [Fiziksel InnoDB yedekleme ve geri yükleme](backup/PHYSICAL.md)

## Çalıştırma

Örneklerde `$cli`, `$ca` ve `$archive` sırasıyla derlenmiş `mysqlsh.exe`, CA ve mevcut yerel arşiv dizinidir. Host ve portu kendi test örneğine göre değiştir. CLI parolayı kendi isteminde alır.

JavaScript ve Python içeren doğrulanmış yerel Windows derlemesi `out/mysql-migration/js-staging/cjs/RelWithDebInfo/bin/mysqlsh.exe` yolundadır. Çalıştırılabilir dosya paketindeki DLL ve çalışma zamanı dizinleriyle birlikte kullanılır.

```powershell
$connection = @('--mysql', '--py', '--host=localhost', '--port=3306', '--user=operator', '--ssl-mode=VERIFY_IDENTITY', "--ssl-ca=$ca")
```

### Mantıksal yedekleme

`create` tutarlı bir uygulama şeması dökümü ve SHA-256 manifesti üretir. Aynı arşiv kimliği, kaynak UUID ve şema ile yeniden çağrılırsa mevcut arşiv doğrulanır. `verify` tamamlanma ve checksum dosyalarını da içeren dosya manifestini denetler. `restore` arşivi doğrular, farklı bir Server UUID’si ve kullanılmamış bir hedef şema ister. Shell yüklenen tablo verisini dökümün checksum’larıyla karşılaştırır. Hedef sunucu MySQL Shell dump yüklemesi için `local_infile` desteği sağlamalıdır. Başarısız bir yükleme yarım şema bırakabilir. Bu durumda işlemi boş bir hedefte yeniden çalıştır. [MySQL Shell döküm](https://dev.mysql.com/doc/mysql-shell/26.10/en/mysql-shell-utilities-dump-instance-schema.html) · [MySQL Shell yükleme](https://dev.mysql.com/doc/mysql-shell/26.10/en/mysql-shell-utilities-load-dump.html)

```powershell
$id = [guid]::NewGuid().ToString('N')
& $cli @connection --file compatibility/backup/logical_backup.py --archive-root $archive create $id --schema example
python compatibility/backup/logical_backup.py --archive-root $archive verify $id
# Use the restore target's connection settings.
& $cli @connection --file compatibility/backup/logical_backup.py --archive-root $archive restore $id
```

### Compute işleri

`install` InnoDB kuyruğunu ve zamanlama kayıtlarını hazırlar. Mevcut kuyruklarda deneme sayısı ve durum olayları için gereken sütunları ekler. `submit` aynı MySQL hesabı ve token altında aynı parametreler için tek iş döndürür. `work-once` sıradaki işi işler ve eski 64 bit checksum sonucunu kaydeder. Terminal durumu `status` ile okunur. `cancel` kuyruktaki işi iptal eder veya aktif çalışana iptal talebi bırakır. Çalışan süreç bağlantısıyla birlikte kaybedilirse sonraki çalışma yarım işi `interrupted` olarak işaretler. Eski compute RPC’nin kurulumu ve işletim hesabı izinleri [wire kılavuzunda](wire_v1/README.md) bulunur.

`schedule` compute işi için 1–86.400 saniyelik UTC aralığı kaydeder. `serve` çalışanı açık kaldığı sürece işleri ve süresi gelen kayıtları işler. Sonraki çalışma zamanı ve kuyruk kaydı tek InnoDB transaction’ında yazılır. Kapanma sırasında kaçırılan aralıklar en fazla bir çalışmayla telafi edilir. Bir önceki iş hâlâ kuyruktaysa yeni iş eklenmez. `cancel-schedule` gelecekteki çalışmaları durdurur. Günlük yerel saat, DST, planlı SQL, yedek ve yeniden eğitim eski takvim sözleşmesine göre ayrıca taşınacak.

```powershell
& $cli @connection --file compatibility/jobs/compute_jobs.py install
& $cli @connection --file compatibility/jobs/compute_jobs.py submit ([guid]::NewGuid().ToString('N')) 1000
& $cli @connection --file compatibility/jobs/compute_jobs.py work-once
& $cli @connection --file compatibility/jobs/compute_jobs.py status $jobId
& $cli @connection --file compatibility/jobs/compute_jobs.py schedule ([guid]::NewGuid().ToString('N')) 1000 --every-seconds 60
& $cli @connection --file compatibility/jobs/compute_jobs.py serve
```

### PL/SQL skaler fonksiyonu

Desteklenen eski biçim bir `IN` parametresi alır ve tek tablodan parametreye bağlı bir skaler sorgu döndürür. Parametre ve sonuç `BIGINT` veya en fazla 38 basamaklı `DECIMAL` olabilir. SQL dosyasını yükleyiciye ver. Oluşturulan MySQL fonksiyonu `SQL SECURITY INVOKER` ile çağıranın işleminde ve izinleriyle çalışır. Desteklenen başka biçimler genişletilene kadar yükleyici eşleşmeyen tanımları reddeder.

```sql
CREATE FUNCTION quantity_for(p_key IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN (SELECT i.quantity FROM items AS i WHERE i.id = :p_key);
END;
```

```powershell
& $cli @connection --schema=example --file compatibility/plsql/scalar_function.py quantity_for.sql
```

### CPU model araçları

#### Classic SQL ve Workbench Query 1

`CREATE MODEL`, `TRAIN MODEL` ve `PREDICT MODEL`, yamalı 26.10 Classic sunucuda isteğe bağlıdır. Sunucu `--sdp-model-sql=ON` ile başlatıldığında `STARDATAPARADISE_CAPABILITIES()` bu üç komutu bildirir. Workbench 8.0.47’nin güncel paketinde bağlantıyı yeniden açın. Query 1’de `CREATE MO`, `TRAI` veya `PRED` yazıp `Ctrl+Space` ile önerileri görebilirsiniz.

Kurulum için sunucu yamasını derleyin. Server deposunu yapılandırırken `-DSDP_MYSQL_UDF_INCLUDE_DIR=<26.10-kaynak-dizini>/include` verin ve Release yapılandırmasında `sdp_mysql_model_udf` hedefini derleyin. `sdp_mysql_model_udf.dll` dosyasını `SELECT @@plugin_dir;` ile gösterilen dizine koyun. Yönetici hesabıyla DLL’i kaydedin ve [classic_model.sql](ml/classic_model.sql) dosyasını Classic `mysql` istemcisiyle yükleyin.

```sql
CREATE FUNCTION sdp_model_compute RETURNS STRING SONAME 'sdp_mysql_model_udf.dll';
```

Sunucu yapılandırmasına `[mysqld]` altında `sdp-model-sql=ON` ekleyip sunucuyu yeniden başlatın. `registry` yazılmazsa varsayılan kayıt şeması `sdp_models` kullanılır. Ayrı hesaplar için ayrı kayıt şemaları oluşturun. Yönetici `CALL sdp_models.install_model_registry('model_lab');` ile mevcut bir şemayı hazırlar. Hesaba `sdp_models.*` için `EXECUTE`, kendi şemasındaki `sdp_model_definitions` ve `sdp_model_versions` tabloları için `SELECT, INSERT`, eğitim kaynağındaki ilgili sütunlar için `SELECT` verin. Aynı kayıt şemasını paylaşan hesaplar o şemadaki modelleri paylaşır.

Örnekte `model_lab` hazırdır ve `training.samples(id, amount, revenue)` sayısal eğitim verileri içerir.

```sql
CREATE MODEL USING '{"registry":"model_lab","name":"sales","algorithm":"linear"}';

TRAIN MODEL USING '{"registry":"model_lab","name":"sales","request_token":"0123456789abcdef0123456789abcdef","schema":"training","table":"samples","order_key":"id","features":["amount"],"label":"revenue"}';

PREDICT MODEL USING '{"registry":"model_lab","name":"sales","features":{"amount":120}}';
```

`TRAIN MODEL` en fazla 1.024 satır ve 64 özellik sütunuyla senkron çalışır. Sıralama sütunu benzersiz ve `NULL` içermeyen değerler taşımalıdır. Aynı model, eğitim girdisi ve `request_token` aynı kayıtlı sürümü döndürür. Yeni eğitim isteğinde yeni token kullanın. `PREDICT MODEL` bir `version` verilmediyse son sürümü seçer ve eğitim sütunları için güncel `SELECT` yetkisini tekrar denetler. `mlp`, `split_column` içinde `train` ve `validation` bölümlerini ister. `kmeans`, `dbscan` ve `pca` için `label` kullanılmaz. DBSCAN eğitim satırlarını kümeler, yeni satır tahmini yapmaz. Classic eğitim gövdesi eski `dataset_id` yerine kaynak tabloyu tarif eder. Yordamlar `sdp_classic_model_stmt` ve `@sdp_classic_model_*` oturum adlarını kullanır.

Yerel kabul için `python -B -m compatibility.tests.classic_models_acceptance --help` geçici TLS örneklerinde kullanılan sunucu, istemci ve DLL parametrelerini gösterir.

#### Model yordamlarını geri kurma

`CREATE MODEL` komutu `1305` hatasıyla `sdp_models.create_model` yordamının bulunamadığını söylüyorsa yönetici hesabıyla `sdp_models` kurulumunu denetleyin. `DROP DATABASE sdp_models` yordamları ve varsayılan kayıt tablolarını birlikte siler.

```sql
SELECT ROUTINE_NAME
FROM information_schema.ROUTINES
WHERE ROUTINE_SCHEMA = 'sdp_models'
  AND ROUTINE_NAME IN ('create_model', 'train_model', 'predict_model');

SELECT UDF_LIBRARY
FROM performance_schema.user_defined_functions
WHERE UDF_NAME = 'sdp_model_compute';
```

DLL kaydı eksikse yukarıdaki `CREATE FUNCTION` komutunu kullanın. Ardından yönetici olarak Classic `mysql` istemcisinden `SOURCE compatibility/ml/classic_model.sql` çalıştırın. Taşınabilir Windows paketindeki dosya yolu `sql/classic_model.sql` olur. Aynı kurulum komutu eksik yordamları ve kayıt tablolarını oluşturur. Kayıt tabloları silindiyse model sürümlerini yedekten geri yükleyin veya modelleri tekrar eğitin.

Kurulum bir yordamın metadata kilidini bekliyorsa ilgili oturumdaki açık işlemi tamamlayın ve kurulumu yeniden deneyin. Eğitim tablolarını ayrı bir uygulama şemasına koyun ve `TRAIN MODEL` gövdesindeki `schema` alanına o şemayı yazın. Workbench’te komutun tamamını seçip çalıştırın.

#### MySQL Shell yardımcı aracı

CMake’de `sdp_mysql_model_runner` hedefini derle. Bu hedef eski C++ lineer, lojistik, CART, random forest, K-Means, DBSCAN, PCA ve CPU MLP eğitim kodunu kullanır. `train` tek MySQL sorgusunda en fazla 1024 satırı ve 64 özellik sütununu okur. Sıralama anahtarı benzersiz olmalıdır. Denetimli algoritmalar `--label` ister. `--options` eski algoritma seçeneklerini JSON nesnesi olarak alır. `mlp`, mevcut `SDP_WITH_CPU_MLP` seçeneğini ve `train` ile `validation` değerlerini taşıyan `--split-column` sütununu gerektirir. DBSCAN mevcut eğitim satırlarını kümeleyip artifaktı kaydeder. `predict` diğer algoritmalarda MySQL sürümünün artifakt checksum’ını ve özellik adlarını doğrular.

Sürümler hesap bazında saklanır. `status` sürümleri ve etkin işaretçiyi gösterir. `activate` doğrulanmış bir sürüm seçer, `rollback` etkin sürümden eski bir sürüme geçer, `deactivate` işaretçiyi temizler. `predict` sürüm sayısı yerine `active` alabilir. Etkin sürümle farklı sıralanmış özellikler reddedilir. Etkinleştirme ve tahminde kaynak MySQL sütunlarına güncel SELECT izni denetlenir. Dataset kataloğu, eğitim checkpoint’i ve eski model RPC’leri bekliyor.

```powershell
& $cli @connection --file compatibility/ml/cpu_model.py install
& $cli @connection --file compatibility/ml/cpu_model.py train sales --worker $modelWorker --algorithm linear --schema example --table samples --order-key id --feature amount --label revenue
& $cli @connection --file compatibility/ml/cpu_model.py activate sales 1
& $cli @connection --file compatibility/ml/cpu_model.py predict sales active '{"amount":120}' --worker $modelWorker
```
