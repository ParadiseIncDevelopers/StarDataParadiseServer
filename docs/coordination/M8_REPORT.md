# M8 Workbench ve istemci geliştirme akışları

| Etap | Doğrulanan sonuç |
| --- | --- |
| 37 | İki TLS bağlantısı, SQL, tipli parametreler ve taslak kurtarma. [Kabul kaydı](STAGES/37.json) |
| 38 | Tablo tasarımı ve ERD, güvenli grid, typed transfer ve atomik iptal. [Kabul kaydı](STAGES/38.json) |
| 39 | PL/SQL derleme, canlı debugger ve test, çevrimdışı Java ve bağlı JDBC artefaktı. [Kabul kaydı](STAGES/39.json) |
| 40 | History kurtarma ve Decision simülasyonu. [Kabul kaydı](STAGES/40.json) |

Son Windows x64 koşusunda Workbench 11, CLI 7 ve Server 48 test geçti. Native 0.4.0, iki .NET 2.0.0-preview.1 paketi, JDBC 0.2.0, Python 0.2.0 ve Node 0.1.0 tüketicileri gerçek TLS üzerinden doğrulandı. Workbench ayrıca dağıtılmış Qt Windows eklentisiyle açıldı.

History testleri tutarlı kaynaktan sabit satır kimliğiyle geri yüklemeyi, güncel anahtar ve yetki kontrollerini, saklama sınırı ve eskimiş plan reddini doğruladı. Decision testleri gerçek kural reddini ve değerlendirilen/atlanan koşulları gösterdi. WHATIF ve sürüm karşılaştırması aynı snapshot’ta çalıştı.

Workbench ve Server kaynakları, test komutları ve artifact hash’leri ilgili etap kayıtlarında bulunur. [Sıradaki etap](NEXT_TASK.md)
