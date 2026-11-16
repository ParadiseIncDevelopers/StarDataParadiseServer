# Etap 48 dayanıklılık ve kaynak kabulü

Windows 11 Home x64, build 26300, AMD Ryzen 5 4600H ve 7.37 GiB görünür RAM. Yükler ayrı veri ve geçici dizinler, rastgele yerel TLS portları ve tek çalıştırmalık kimlik bilgileri kullandı.

## Kalıcılık ve eşzamanlılık

- Release Server regresyonunda 24 test geçti. WAL, sayfa yazımı, flush ve checkpoint yayınında durdurulan çocuk süreçlerden sonra işlem LSN’si ve History doğrulandı. Kısa yazma, disk-dolu ve flush arızaları test kancalarıyla enjekte edildi. Gerçek Windows erişim reddi ayrı test dizinlerinde doğrulandı.
- Model sürümü önceki veya yeni LSN ile atomik kaldı. Eski etkin model, yeni sürüm eklendiğinde korunarak sorgulandı. Yedek ve restore yayın öncesi kaybolan süreçte hedef göstermedi. Yayın sonrası kesintide ortaya çıkan yedek ve restore sağlam ve sorgulanabilir kaldı.
- Eşzamanlı bakiye güncellemesi tek başarılı debit bıraktı. FK yarışında yetim satır ve commit kaydı oluşmadı. History pin veya purge kazanabildi ve iki sonuçta da History doğrulandı. Var olan UNIQUE, Decision cross-table, grid revizyonu ve eşzamanlı model aktivasyonu regresyonları geçti.
- CLI 2/2 ve Workbench 7/7 odaklı Release testi geçti. CLI’nin insan çıktısı kontrol baytlarını güvenli gösterdi. JSON tipi ve içeriği korundu. Günlük ve çıktı taraması test kimlik bilgilerini bulmadı. Workbench aktarım ve iptal testinin çalışma kümesi geçici TLS örneğinde ölçüldü.

## Güvenlik ve paket kapsamı

Wire, SQL, PL/SQL çalışma sınırları, model hash’i, storage/WAL/checkpoint ve restore doğrulaması ayrı test edildi. SQL girdi sınırının altında ve üstünde davranış ile geçersiz girdide kaynak hash’leri sınandı. Testler Windows x64, donanım ve yerel veri koşullarıyla sınırlıdır.

AddressSanitizer kapsamı Server’ın storage ve SQL kodu ile Native SDK ve onun C/C++ paket tüketicileridir. Qt ve OpenSSL gibi önceden derlenmiş bağımlılıklar kendi sabit paketlerinden gelir. OS düzeyinde dolu disk deneyi yerine sürücüde tekrarlanabilir hata enjeksiyonu çalıştırıldı.

İmzalı NuGet indirmeleri yeniden doğrulandı. Windows uygulama MSI’ları unsigned-development kanalında. Kaynak, paketler ve dokümanlar kapsam listesinde yazan kimlik bilgisi örüntülerine göre tarandı.

İlk AddressSanitizer sürümü History yanıtı oluşturulurken bellek erişimi hatası bildirdi. İlk ayrıştırma Release testini geçti ve aynı ASan bulgusu tekrarlandı. Çözümlenen katalog görüntüleri sahipli nesnelere alındı. Varsayılan sınırlarla özgün History testi ve final ASan temel 11/11 ile TLS 8/8 geçti. Native kurulu ASan paketi 24 davranış testinden geçti. PL/SQL özyineleme sınırı, tanılama kopyasının 8 MiB PE yığın rezerviyle doğrulandı. Release Server özgün yığın rezerviyle, History, SQL, PL/SQL, model ve backup dahil 11/11 geçti. Son EXE ile üç bağımsız MSI çıkarma/başlatma testi ve güncel SPDX denetimi geçti.

## Ölçümler

Tek TLS istemcisi, 1.200 başlangıç satırı, satır başına 480 bayt metin ve 72 dayanıklı autocommit yazması. Okuma gecikmesi hazırlama, yürütme, fetch ve kapatmayı içerir. Yazma gecikmesi Server’ın kalıcı yanıtına kadardır. Her yazmanın defter sonucu ayrıca sorgulandı. Yeniden açılış örneği süreç başlangıcı, TLS/auth ve ilk doğrulanmış okumayı içerir. İşletim sistemi dosya önbelleği temizlenmedi. Aynı makinede kabul testleri de çalışıyordu.

| İşlem | Örnek | p50 ms | p95 ms | p99 ms | İşlem/sn |
| --- | ---: | ---: | ---: | ---: | ---: |
| Sıcak TLS okuma | 160 | 150,25 | 152,91 | 155,22 | 6,89 |
| Dayanıklı autocommit | 72 | 256,02 | 276,90 | 282,40 | 3,89 |
| Yeni süreçte ilk okuma | 12 | 1.117,04 | 1.171,48 | 1.171,48 | 0,89 |

History 1.200’den 1.272 satır olayına çıktı. Son 72 olay 17 satırı geçmeyen diff sayfalarında okundu. Sınırlı SELECT 193 satırı 12 batch’te döndürdü. Store 372.736 bayt, WAL 705.044 bayt büyüdü. Server tepe çalışma kümesi 15.986.688 bayttı. Workbench aktarım/iptal test süreci 40.239.104 bayt tepe çalışma kümesi ölçtü. Bu değerler seçilen tek iş yükünün gözlemidir.

[Ham benchmark](../../out/sdp-m10/stage48/benchmark-final-data/resource-benchmark.json) · [Çökme/recovery ham süreleri](../../out/sdp-m10/stage48/rel/recovery-evidence/crash-matrix.json) · [Workbench bellek kaydı](../../out/sdp-m10/stage48/workbench-memory.json)

[Paket denetimi](../../out/sdp-m10/stage48/final-package/release-audit.json) · [SPDX envanteri](../../out/sdp-m10/stage48/final-package/release.spdx.json) · [Etap kaydı](STAGES/48.json)
