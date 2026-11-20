# Classic arayüz kaynak yayını - 20 Kasım 2026

Kullanıcı, tamamlanan çalışmaların tümünü `master` dalına commit/push etmeyi ve sürüm adlarıyla amaçlarını tek tek kaydetmeyi istedi.

| Depo | Sürüm ve adı | Amaç |
| --- | --- | --- |
| StarDataParadise Workbench | `v0.3.0-preview.5 - Classic Birleşik Çalışma Alanı` | Ortak kabuk/sekmeler, SQL/Veri/Şema, native editör ve SQL önizleme, dil paketi, yönetim/performans ve ürün araçlarını kaynak, test, ikon, tasarım ve kabul görselleriyle birlikte yayımlamak. |
| StarDataParadise Server | `v0.5.0-preview.6 - Classic Arayüz Kabul ve Eşgüdüm Kaydı` | Workbench yayınının derleme, veri/DDL onay sınırı, TLS hazırlığı, geçici veri temizliği ve canlı kabul kanıtlarını ortak proje kaydına almak. Bu etiket belge/eşgüdüm kapsamındadır. |
| StarDataParadiseCli | Mevcut `v0.3.0-preview.1` | Bu çalışmada değişiklik yok; mevcut sürüm korunur. |
| StarDataParadiseDLLler | Mevcut `v0.4.1` | Bu çalışmada değişiklik yok; mevcut sürüm korunur. |

Workbench kaynak tabanı 8.0.47 olarak kalır. Server'ın SQL motoru, wire sözleşmesi ve SDK ABI'si bu belge yayınıyla değiştirilmez.

Workbench'in bütün 409 dosyalık değişiklik kümesi master'da [3c3887e06e2fdb7ef19e007c552cea473e708dcc](https://github.com/ParadiseIncDevelopers/StarDataParadise/commit/3c3887e06e2fdb7ef19e007c552cea473e708dcc) commit'ine kaydedildi. Master ve açıklamalı `v0.3.0-preview.5` etiketi aynı atomik Git push ile gönderildi; etiketin bu commit'e açıldığı doğrulandı. Bu Server kaydı `v0.5.0-preview.6` etiketiyle yayımlanır.

## Yayımlanan Workbench kapsamı

1. Başlık/ana sekmeler/menü/çalışma alanı sırası, profil avatarları ve satır içi bağlantı bekleme/iptali.
2. Şema gezgini ikonları, ortak kompakt bileşenler, Çalıştır araçları ve okunur SQL/sonuç sekmeleri.
3. Gerçek Recordset'e bağlı grid/sağ form, native tablo/şema düzenleme, sütun/indeks/salt okunur DDL inceleme.
4. DML/DDL SQL önizlemesi, betiği kaydetme, uygulama/iptal ve geri alma sınırları.
5. Sonuç Tablo/Form/Sütun türleri/Ölçümler/Plan sayfaları ve gerçek planın Ağaç/Tablo/JSON görünümleri.
6. Native/Python/WinForms ortak Türkçe/İngilizce dil paketi, ` - ` ayracı ve teknik ham değerlerin korunması.
7. Gerçek SDP SQL, model/iş/geçmiş ve TLS/sunucu ölçümleri; Değişkenler açılış hatası düzeltmesi.
8. Türkçe hata penceresi, beş ürün aracı, derleme/sahneleme betikleri, kaynak testleri, ikon/SVG lisansları ve bütün tasarım/kabul görselleri.

## Doğrulama ve sınırlar

- 18 native hedefin tam derlemesi ve son WBControls/wbprivate iki hedefli derleme geçti.
- 66 Python sınaması ve 21 ayrı derlenmiş C++/C# kontrolü geçti.
- Dokuz kaynak ve yedi runtime grubu; 340 kaynak hash'i, dört katalog ve 18 x64 PE doğrulandı.
- Canlı kabulün altı veri/TLS ve on teslim UI kontrolü geçti. DDL iptali tanımı korudu; DML kabulü yalnız işe ait geçici tabloda yapıldı.
- Geçici QA şeması temizlendi ve yokluğu doğrulandı. Mevcut kullanıcı tabloları/ayarları korundu.
- 21 native JPEG ve son paket denetiminin 18 kontrolü geçti. Son uygulama gerçek sekiz satırlık tablo sonucunda açık bırakıldı.
- Süresi dolmuş yalnız test fixture sertifikaları aynı anahtarlarla yenilendi; localhost/IP doğrulaması ve strict TLS resume geçti. Mevcut 3306 sunucusu korunur.

MSI kur/kaldır, tam DPI/yüksek kontrast/ekran okuyucu matrisi ve canlı SSH/GDAL kabulü ayrı kapsamdır. `out/` çalışma zamanları, bağımlılıklar ve yerel test/kullanıcı ayarları Git ignore kapsamındadır.

[Ortak etap kaydı](MYSQL_MIGRATION.md) - [Workbench sürüm notları](https://github.com/ParadiseIncDevelopers/StarDataParadise/blob/v0.3.0-preview.5/packaging/classic-workbench/SURUM_NOTLARI.md) - [Workbench canlı kabulü](https://github.com/ParadiseIncDevelopers/StarDataParadise/blob/v0.3.0-preview.5/packaging/classic-workbench/tasarim/sql-calisma-alani/VERI-SEMA-YONETIM.md)
