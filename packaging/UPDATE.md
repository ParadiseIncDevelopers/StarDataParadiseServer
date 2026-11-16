# Windows x64 güncelleme

Server, Workbench ve CLI için ayrı güncelleme paketi hazırlanır. `Update.ps1` uygulamayı ve Server verisini ayrı bir hedefe hazırlar. Workbench’te **Tools → View update status** ve CLI’da `--update-status FILE` aynı yerel günlüğü açar.

Paketin `update.json` dosyası ürün ve kaynak sürümünü, hedef sürümü, dosya boyutunu, SHA-256 değerini ve uyumluluk sürümlerini taşır. Ağ protokolü ve Native SDK ABI ayrı alanlardır. Storage, WAL, checkpoint, History, PL/SQL IR ve model için v1 kabul edilir. Workbench paketleri Qt 6.11.3 ile hazırlanır.

Üretim paketi için ayrık CMS imzasını `-SignaturePath` ile ver. İmzacı sertifikasının SHA-256 pinini `-TrustedSignerSha256` ile paketten bağımsız bir kaynaktan gir. İmzasız `development` manifesti açıkça `-AllowDevelopment` gerektirir. `-Action CheckOnline` aynı imza kontrolüyle yalnızca HTTPS manifestini okur.

## Server

Bakım penceresinde açık işlemleri tamamla, işleri bitir veya uygulamadan iptal et ve kaynak instance’ı durdur. Güncelleme veri dizinine özel kilit alır. Bekleyen iş veya etkin zamanlama bulunduğunda sonraki adım için güvenli durumda durur. `-StopOwnedService` verildiğinde kaynak EXE, yapılandırma ve kayıtlı hizmet kimliği kendi Windows hizmet komutuyla eşleştirilir.

```powershell
./packaging/Update.ps1 -Action Apply -Component server -BundleRoot $Bundle -JournalPath $Journal `
    -SourceProgram $CurrentServer -SourceConfig $CurrentConfig -TargetRoot $FreshTarget `
    -SignaturePath $Signature -TrustedSignerSha256 $ExpectedCertificate
```

Kaynak yapılandırması ve kaynak EXE hash’i günlüğe bağlanır. Server’ın kilitli snapshot komutu kaynak verinin kimliğini ve LSN’sini döndürür. Hedefin `backup` ve `data` kopyaları ayrı ayrı doğrulanır. SQL şeması, History, yordamlar ve modeller staging sırasında kontrol edilir. Güncelleme hedefin yerel TLS dinleyicisini yoklar. Tamamlanan aday bağımsız `data.json` ile başlatılabilir. Bu yapılandırma loopback adresinde ve sistemin seçtiği portta çalışır.

## İstemciler

Doğrulanmış taşınabilir ZIP’i paket dizinine koy. `-Component workbench` veya `-Component cli` ile yeni `-TargetRoot` seç. Güncelleme ZIP yol sınırlarını ve disk alanını doğrular. Workbench pencere başlangıcını, CLI sürüm komutunu çalıştırır. Hazır uygulama hedefteki `application` dizinindedir.

## Kesinti ve geri dönüş

`-Action Status -JournalPath $Journal` tamamlanan adımı ve sıradaki adımı gösterir. Aynı `Apply` komutu günlüğü kendi süreci için kilitler ve doğrulanmış adımları yeniden kullanır. Kaynak kimliği değiştiğinde veya yarım yazılmış bir hedefin sahipliği kanıtlanamadığında günlük `safe_stop` durumuna geçer. Yeni bir hedef kullanmadan önce kaynak ve korunmuş kopyaları incele.

`-Action Rollback` aynı parametrelerle hedef verinin doğrulanmış staging anındaki hash’ini ve LSN’sini denetler. Hedef değişmişse `has_new_writes` kaydedilir ve iki kopya inceleme için korunur. Hedef değişmemişse eski kopya kullanılabilir. Güncelleme sırasında durdurulan kayıtlı kaynak hizmeti geri dönüşte kendi hizmet komutuyla yeniden başlatılır.
