# Instance yönetimi ve Health Center

Mevcut her Server instance için ayrı `instance_name`, port, data, log, temp ve isteğe bağlı backup dizinleri seç. Instance kimliği config adı ve canonical data dizininden türetilir. WAL, History ve model artifaktları aynı data dizinindeki storage işlemlerine bağlıdır. Varsayılan dinleme adresi yerel loopback’tir.

Yerel Windows hizmeti oluşturma ve kontrol, işletim sistemi yetkili konsolundan yapılır.

```powershell
.\StarDataParadiseServer.exe --config .\server.json --service install
.\StarDataParadiseServer.exe --config .\server.json --service start
.\StarDataParadiseServer.exe --config .\server.json --service status
.\StarDataParadiseServer.exe --config .\server.json --service stop
.\StarDataParadiseServer.exe --config .\server.json --service remove
```

SCM kimliği `StarDataParadise-<instance_name>`, çalışan hesap `NT SERVICE\StarDataParadise-<instance_name>` olur. İşletim sistemi yöneticisi bu hesaba exe, config, TLS ve bootstrap dosyalarında gerekli okuma/çalıştırma erişimini, yalnız ilgili data, log, temp ve backup dizinlerinde gerekli yazma erişimini verir. Remove ilgili hizmet kaydını kaldırır ve instance dosyalarını korur. Windows hizmeti erişimi ile Server administrator hesabı ayrı yetki sınırlarında çalışır. Linux hizmet adaptörü bu Windows paketinin kapsamı dışındadır.

## Yönetim API

- `admin_status` gerçek bağlı oturumları, aktif transaction ve lock wait sayılarını, 250 ms ve üstü tamamlanan sorgu olaylarını, storage sayfalarını ve sınırları döndürür. Deadlock grafiği capability alanında kapalıdır.
- `admin_health` her kontrol için son kanıtı gösterir. Storage, History, backup, sertifika ve jobs ilk kontrolden önce `UNKNOWN` durumundadır. `admin_health_check` ölçüm yapar. Storage kontrolü sayfaları ve model artifakt kullanımını, History kontrolü journal bütünlüğünü ve saklama kullanımını, backup kontrolü doğrulanmış arşivin manifest kanıtını raporlar.
- `admin_config_preview` izinli değişikliği doğrular. `admin_config_apply` önceki dosyayı yanında koruyup aynı dizinde atomik değiştirir. `timeout_ms` yeni ağ I/O zaman sınırlarında ve yeni SQL oturumlarında yürürlüğe girer. `port` ve `max_connections` yeniden başlatma gerektirir.
- `admin_maintenance_submit`, `integrity`, `statistics` veya `index` işlemlerini ortak Scheduler’a ekler. İstatistik işi mevcut ANALYZE işleyicisini, indeks işi REINDEX akışını kullanır. İş sonucu `job_status` ile okunur.
- `admin_support_preview` güvenli config alanları ve log sayımlarını gösterir. `admin_support_create` mevcut olmayan bir isimle `temp/support/<target_name>/bundle.json` atomik olarak yaratır. Destekte yalnız izinli alanlar, redacted loglar ve bundle’a özel korelasyon takma değerleri bulunur.

Structured JSONL logları query, audit, recovery, History, operational, ML, Decision ve error kategorilerinde yazılır. Her dosya 2 MiB eşiğinde bir yedek kopyayla döner ve kategori başına istek oranı sınırlıdır. SQL metni, kimlik bilgileri ve TLS özel anahtarı log şemasının dışında kalır.
