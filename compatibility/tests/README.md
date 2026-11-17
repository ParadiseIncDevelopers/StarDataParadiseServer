# MySQL tabanlı Server kabul testleri

Windows x64 Server, klasik istemci, Python destekli Shell ve model worker derlemelerinden sonra depo kökünde çalıştır.

```powershell
python -B -m compatibility.tests.acceptance
```

Varsayılan yollar `out/mysql-migration` altındaki `bfast`, `cli-b` ve `ml-b` derlemeleridir. Başka bir derleme için `--mysqld`, `--mysql`, `--basedir`, `--shell` ve `--worker` verilebilir. `--case backup`, `--case jobs`, `--case plsql` ve `--case models` ayrı seçimlerdir. Birden fazla `--case` birlikte kullanılabilir. `--worker` yalnız model testi için gerekir.

Her test `out/compatibility-tests` altında ayrı geçici veri dizini kullanır. Test Server’ı loopback üzerinde açılır. PID, veri dizini, StarDataParadise sürümü ve CA doğrulamalı TLS kontrol edilir. Backup testi iki bağımsız süreç çalıştırır. Testler açık bir MySQL adresi veya var olan veri dizini almaz.

- Backup tutarlı mantıksal dökümü, checksum denetimini ve bağımsız hedefe tipli geri yüklemeyi sınar.
- Jobs eşzamanlı idempotency, checksum, kuyrukta ve çalışırken iptal, zaman aşımı ve gerçek worker kaybından kurtarmayı sınar. Kalıcı UTC aralığının canlı worker ile çalışması ve iptal edilmesi de denetlenir.
- PL/SQL aktarılmış BIGINT ve DECIMAL skaler fonksiyonlarını, NULL, invoker izinleri ve çağıranın transaction’ıyla sınar.
- Models InnoDB verisiyle lineer, lojistik, CART, random forest, K-Means, DBSCAN, PCA ve CPU MLP eğitimini, tahmin veya dönüşümü, değişmez sürümleri ve artifakt checksum denetimini sınar. Etkin sürüme geçiş, rollback, özellik uyuşmazlığı ve devre dışı bırakma da denetlenir. Model worker’ı varsayılan `SDP_WITH_CPU_MLP=ON` seçeneğiyle derle.

JavaScript etkin CLI derlemesini aynı test Server’ı üzerinde sınamak için `--case shell-js --shell <mysqlsh.exe>` ver. Senaryo aynı Windows derlemesinde JavaScript ve Python üzerinden gerçek SQL sorgusu çalıştırır.

Klasik Windows Workbench yürütülebilir dosyasını yeni Server’ın geçici bir örneğiyle sınamak için:

```powershell
python -B -m compatibility.tests.workbench_native --exe 'C:/path/StarDataParadise.exe'
```

Workbench’in yerel GRT bağlantı modülü TLS kimlik doğrulamasını, yanlış CA reddini, tipli SQL değerlerini, `ANALYZE` komutunu ve rollback işlemini kontrol eder. `--keep-visible` ile ayrı profilde bir salt okunur örnek bağlantısı hazırlanır. Örnek Server Workbench penceresi kapanana kadar çalışır.
