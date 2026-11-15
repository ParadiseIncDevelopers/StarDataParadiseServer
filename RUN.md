# StarDataParadiseServer yerel çalışma

Server Windows x64 üzerinde foreground çalışır. TLS 1.2 ve üstü kullanır. Varsayılan adres `127.0.0.1:7443`. Gerekli dizinleri ve PEM sertifika/anahtarı önceden oluştur. Anahtar dosyasını yalnız servis hesabı ile sistem yöneticilerinin erişebildiği bir konumda tut. İstemcinin doğrulayacağı DNS adı veya IP adresi sertifikanın SAN alanında yer almalı.

Örnek `server.json`

```json
{
  "data_dir": "data",
  "log_dir": "logs",
  "temp_dir": "tmp",
  "tls_certificate": "tls/server-chain.pem",
  "tls_private_key": "tls/server-key.pem",
  "port": 7443,
  "max_connections": 16,
  "timeout_ms": 10000
}
```

Göreli yollar config dosyasının dizininden çözülür. `port: 0`, yerel test için işletim sisteminden boş port ister. `bind` sayısal IPv4/IPv6 adresidir. Loopback dışına bind etmek ayrıca `"allow_remote": true` gerektirir. `timeout_ms`, el sıkışma ve her çerçeve okuma/yazma işleminin zaman sınırıdır. TLS kapanışı 500 ms ile sınırlıdır.

```powershell
.\StarDataParadiseServer.exe --config .\server.json --bootstrap-admin local-admin
.\StarDataParadiseServer.exe --config .\server.json
```

Bootstrap parolayı iki kez stdin üzerinden okur. Etkileşimli konsolda giriş yankısı kapalıdır. Parola 16–1024 UTF-8 bayt olmalı. `admin-v1.json` dosyasına rastgele salt ile PBKDF2-HMAC-SHA256 doğrulayıcısı yazılır. İlk kurulum işlemi veriyi üzerine yazmadan mevcut yöneticiyi korur. Başlatma tamamlandığında süreç `LISTENING` satırında adresi ve portu bildirir.

Operasyon ve kimlik doğrulama olayları ayrı log dosyalarına yazılır. Her log 256 KiB ile ve tek bir döndürülmüş kopyayla sınırlıdır. Ctrl+C dinleyiciyi kapatır, bekleyen işlemleri iptal eder ve TLS kapanışını tamamlamayı dener. `.sdp.lock` dosyası yerinde kalır. Süreç Windows dosya kilidini bırakır ve dizin yeniden açılabilir.

## Storage maintenance

A new instance can initialize a storage file explicitly with --initialize-storage. Use --check-storage for read-only validation, --recover-storage for offline replay and --checkpoint-storage for an offline checkpoint and retention summary. Server startup recovers verified committed WAL before it binds the listener. See DISK_FORMAT.md and RECOVERY.md for the on-disk format and recovery behavior.

## Instance ve Health Center

Birden fazla yerel instance için config içinde benzersiz `instance_name`, port ve ayrı data, log, temp dizinleri kullan. Hizmet kurulum ve kontrol adımları, health kontrolleri ve destek paketi [ADMIN.md](ADMIN.md) dosyasında. Doğrulanabilir yedek konumu `backup` config alanıyla etkinleşir. Geri yükleme akışı [BACKUP.md](BACKUP.md) dosyasında.
