# Lisans düzenlemesi — 23 Kasım 2026

## Kapsam

Kullanıcının dokuz StarDataParadise deposuna uygun lisans yazılması isteği
kapsamında lisans dosyaları, README açıklamaları ve ilgili paket metadata'sı
hazırlandı. Bu, ürün işlevi veya yeni özellik etabı değildir.

| Depo | Lisans düzeni |
| --- | --- |
| StarDataParadise | Özgün Qt/wire istemcisi Free-Use 1.0; Classic kaynak ve build/kurulum kapsamı GPLv2 ve mevcut ayrı lisanslar |
| StarDataParadiseServer | Özgün kod ve wire sözleşmesi MIT; tanımlanan MySQL/native kapsamları GPLv2 |
| StarDataParadiseCli | Özgün wire istemcisi Free-Use 1.0; MySQL Shell entegrasyonu GPLv2 |
| StarDataParadiseDLLler | Özgün Native SDK MIT; bağımlılık lisansları ayrı |
| StarDataParadise.SDK.NET | Mevcut MIT ve telif satırı korunur |
| StarDataParadise.SDK.Java | Mevcut SDK Free-Use 1.0 lisansı aynen korunur |
| StarDataParadise.SDK.Python | Özgün SDK MIT |
| StarDataParadise.SDK.Node | Özgün SDK MIT |
| StarDataParadise-Docs | Doküman metni CC BY 4.0; özgün site/örnek kodu MIT; marka ve üçüncü taraf istisnaları açık |

GPL kapsamındaki kontrol edilen özgün katkılar için upstream sürümün
kapsamına bağlı ek bağlantı izni ayrıca verilir. Oracle ve diğer hak
sahiplerinin bildirimleri değiştirilmez. Önceden verilmiş lisans hakları
geri alınmaz.

## Doğrulama

| Durum | Kontrol |
| --- | --- |
| PASS | Dokuz deponun güncel varsayılan dalı ve kesilmemiş kaynak ağacı okundu; mevcut dosyalar blob SHA ile eşleştirildi. |
| PASS | Yeni Markdown dosya bağlantıları mevcut ağaç ve eklenen dosyalarla karşılaştırıldı. |
| PASS | Değişen JSON, TOML ve XML metadata dosyaları Python json, tomllib ve ElementTree ile ayrıştırıldı. |
| PASS | Node paket ve lock dosyası MIT kimliğinde; Python metadata'sı MIT ve LICENSE dağıtım girdisini içeriyor. Framework NuGet projesi MIT ve LICENSE paket girdisi içeriyor. |
| PASS | Server/Shell tam lisans kopyalarının Git blob SHA-1 değerleri sabit upstream sürümlerdeki dosyalarla birebir eşleşti. |
| PASS | Java ve .NET mevcut LICENSE dosyaları ile Classic Workbench upstream lisansı değişiklik kapsamına alınmadı; mevcut README içeriği korundu. |
| PASS | Kaynak yamaları, checksum manifestleri, contract ZIP'leri, uygulama kaynakları ve sürüm numaraları değişiklik kapsamı dışında kaldı. |
| NOT_RUN | Windows uygulama derlemeleri, çalışma zamanı testleri ve MSI/NuGet/wheel/npm/JAR paket üretimi bu lisans/metaveri düzenlemesinde çalıştırılmadı. |
| NOT_RUN | Paket registry yayını, release/tag ve GitHub Pages dağıtımı yapılmadı; mevcut yayımlanmış ikililer ve örnek ZIP'i yeniden üretilmedi. |

Kontroller kaynak lisans kapsamını ve metadata tutarlılığını doğrular;
bütün geçmiş ikili dağıtımlar için eksiksiz lisans uyum sertifikası değildir.
Paket metadata değişiklikleri sonraki paket üretiminde uygulanır.
