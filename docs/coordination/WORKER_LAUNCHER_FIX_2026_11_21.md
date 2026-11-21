# Server 0.5.2 - embedded Python ortamını worker'dan ayırma

0.5.1 Startup ve bağımsız EXE kabulü geçmişti. Teslim edilen Workbench 0.3.1'in
manuel düğmesiyle worker kapalı durumdan açılınca Workbench'in PYTHONHOME/PYTHONPATH
değerlerinin MySQL Shell'e taşındığı görüldü. MySQL Shell, Workbench'in eksik
ctypes/SSL Python ortamını yüklemeye çalışıyor ve worker başlamıyordu.

Native başlatıcı yalnız çocuk sürecin PYTHON* değişkenlerini kaldırır. İşletim
sistemi politikaları, global ortam, Windows kasası ve diğer kullanıcı provider
ortam değişkenleri değiştirilmez. Aynı EXE Windows Startup ve Workbench manuel
başlatma tarafından kullanılır. Workbench kaynakları/binary'si için ek değişiklik
gerekmez; 0.3.1 bu Server düzeltmesiyle çalışır. Eski stable etiketler değiştirilmedi;
düzeltme yeni Server 0.5.2 commit/tag ve stable release olarak yayımlanır.

Doğrulamalar:

- PASS: Gerçek Workbench 0.3.1 GRT düğmesi kapalı worker'ı açtı; TLS/owner health
  ready ve beş AI katalog akışı doğrulandı.
- PASS: Aynı 0.5.2 EXE, kayıtlı Startup kısayolundan temiz durdurulmuş worker'ı
  yeniden açtı. Gerçek logoff yapılmadı; kısayolun kendisi çağrıldı.
- PASS: Derlenmiş launcher'a kasıtlı bozuk PYTHONHOME/PYTHONPATH/PYTHONSTARTUP
  verilerek ayrı test mysqlsh.exe süreci çalıştırıldı. PYTHON* kaldırılmış,
  ilgisiz sentinel ortamı korunmuş. Eksik deployment config'i exit 1 ile reddedildi.
- PASS: Nihai 0.5.2 ZIP CRC ve payload hash'leri, ayrı datadir initialize/TLS,
  bakım katalog kurulumu, native AI/Knowledge ve short SQL kabulü.
- Model kabulünün veri envanteri adımında bir salt okunur CHECKSUM CLI çıktısı
  boş geldi; mutasyonlar tekrar edilmedi. Yerel tanılama script'inde yalnız
  beklenen satırlı envanter okumaları güvenli biçimde tekrarlandı. Başarılı
  gerçek model/yanıt/veri-koruma yeniden kontrolü yerel exit kaydındadır.
- Sürüm wrapper'ı 0.5.2'ye derlenip üç CLI kontrolü tekrar koşuldu. Python worker
  sözleşmesi ve native engine byte'ları değişmedi; 0.5.1'in 60 unit/50 CTest ve
  Workbench'in 35 unit/paketli live kabulü geçerlidir.

Workbench minör tabanı 8.0.47 ve ürün sürümü 0.3.1 kalır. Server ürünü 0.5.2,
native uyumluluk sürümü 26.10.0-StarDataParadise'dır. API anahtarları yokken
gerçek OpenAI/Claude inference kabulü iddia edilmez. Yayın planı ve hash/test
kayıtları `out/release-0.5.2/` altında, mevcut veri yedekleri korunur.
