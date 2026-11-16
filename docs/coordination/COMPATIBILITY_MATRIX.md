# Uyumluluk ve yetenekler

| Bileşen | Doğrulanan durum |
| --- | --- |
| Windows x64 | Visual Studio 2026, v145, Windows SDK 10.0.26100.0, CMake 4.2.2 |
| Workbench | Qt Base 6.11.3. Bağlantısız başlangıç, iki bağımsız native TLS oturumu, Windows Credential Manager ve sır içermeyen profil dışa aktarımı doğrulandı. |
| CLI | C++20. Native ABI 1 ile TLS, DNS, profiller, terminal ve JSON durum çıktısı doğrulandı. |
| Server | TLS 1.2+, bootstrap, auth, hello, health, capabilities, süreç ve dizin kilidi. Adres varsayılanı loopback. |
| Native DLL | Ürün 0.3.0, C ABI 1, wire v1. Kurulu paket tüketicisi bağlanıp gerçek tipli CRUD çalıştırdı. |
| .NET SDK 2.0.0-preview.1 | StarDataParadiseDrive .NET 10, StarDataParadiseNetFramework .NET Framework 4.8. Temiz NuGet kurulumları ve canlı TLS doğrulandı. |
| JDBC 0.2.0 | JDK 17 ve Gradle 9.3.1. Kurulu Maven JAR üzerinden canlı TLS, typed sonuçlar ve transaction doğrulandı. |
| Python DB-API 0.2.0 | Python 3.12. Temiz wheel ve sdist, canlı TLS ve belirsiz commit doğrulandı. |
| Node.js SDK 0.1.0 | Node.js 22. Bağımsız npm arşivi, temiz kurulum, canlı TLS, tipli SQL, işlem iptali ve commit sonucu doğrulandı. |
| Yerel SQL ve katalog | DDL transaction, typed logical plan ve salt okunur Server denetleyicileri. [SQL uyumluluğu](../../SQL_COMPATIBILITY.md) |
| SQL CRUD ve transaction RPC | sql_crud_v1 ile hazırlanmış tipli sorgu, metadata, bounded fetch, savepoint ve kalıcı outcome ledger. [Wire eki](../../contracts/sql/SQL_RPC_V1.md) |
| İlişkisel bütünlük | Kalıcı B+Tree equality/range, PK/UNIQUE/FK/CHECK, DEFAULT/identity, salt okunur view ve reversible DDL journal. [SQL uyumluluğu](../../SQL_COMPATIBILITY.md) |
| İleri SQL ve optimizer | JOIN, aggregate, CTE/UNION, EXPLAIN ANALYZE, Türkçe NLS collation, REINDEX ve sorgu kotaları. [SQL uyumluluğu](../../SQL_COMPATIBILITY.md) |
| SQL güvenliği ve oturumlar | TLS actor, kalıcı GRANT/REVOKE, parola yenileme, reset, lock wait, iptal ve terminate. [Yetki sözleşmesi](../../SQL_SECURITY.md) |
| Yönetim RPC | Bağımsız instance, gerçek Health Center, config preview/apply, redacted support, iş kuyruğunda maintenance ve Native ABI 1. [Yönetim](../../ADMIN.md) |
| Storage | Disk formatı v1. Heap, BLOB, satır kodlama ve salt okunur integrity taraması doğrulandı. |
| WAL / değişiklik journal | WAL v1 ve iç transaction API, seri lease, savepoint, atomik satır/katalog değişiklik kaydı. |
| Recovery ve checkpoint | Checkpoint formatı v1. Windows süreç kesintisi, torn-page onarımı, idempotent replay, WAL rotasyonu ve Etap 08/09 uyumluluğu doğrulandı. M2 tam geçmiş için segmentleri saklar. |
| PL/SQL IR | IR v1, scalar fonksiyon/prosedür, caller transaction ve typed slot/source map. [IR sözleşmesi](../../IR_SPEC.md) |
| PL/SQL cursor ve trigger | Tipli OUT/compound ve birden çok named set, row trigger ve bağımlılık takibi. [PL/SQL](../../PLSQL_LANGUAGE.md) |
| PL/SQL debugger, test ve profil | Gerçek TLS attach/step, typed watch, isolate rollback ve duvar saati profili. [Debugger](../../PLSQL_DEBUG.md) |
| History v1 | Stable nesne ve satır imajı, LOG/DIFF/READ/VERIFY, mevcut izin, pin, mantıksal retention ve kota. [History](../../HISTORY.md) |
| History restore/revert v1 | Preview, current revision karşılaştırması, stable satır ve tablo restore, routine kaynak restore ve ledger replay. [History](../../HISTORY.md) |
| Decision v1 | Immutable kural sürümleri, tipli koşul IR, ortak yazma denetimi, canlı snapshot, kapalı modül hatası. [Decision](../../DECISION.md) |
| Decision inceleme v1 | Salt okunur WHYNOT/WHATIF/RECHECK, version impact corpus, gerçek koşul izi, güncel izinler ve Native ABI 1. [Decision](../../DECISION.md) |
| Jobs v1 | Kalıcı compute kuyruğu, Windows process budget, progress/cancel, interval ve DST schedule, Native ABI 1. [İşler](../../JOBS.md) |
| Dataset v1 | Parametreli snapshot, lineage, seeded/gruplu/zamanlı split, train-only fit ve yalıtılmış saf PL/SQL worker. [Dataset](../../DATASET.md) |
| Model registry v1 | Worker CPU linear, logistic, CART ve forest. SHA-256 immutable artifact, atomic aktivasyon ve ayrık held-out metrikleri. [Modeller](../../MODELS.md) |
| Denetimsiz CPU modelleri | K-Means merkez ve atama, DBSCAN eğitim kümesi/noise, PCA projection ve reconstruction. Ayrı metrik kapsamları ve doğrulanmış deney karşılaştırması. [Modeller](../../MODELS.md) |
| CPU MLP v1 | Tanh, ReLU veya sigmoid MLP, SGD veya Adam, BCE ve validation early stopping. MLP’siz ayrı minimal Release ile SQL, History ve Decision doğrulandı. [MLP](../../ml/DEEP_LEARNING.md) |
| CPU model checkpoint ve lifecycle | Atomik epoch checkpoint, restart sonrası birebir CPU devam, sürüm rollback/deactivate/delete, History pin güvenliği. Native C ABI 1 typed model ve dataset. GPU eğitim backend’i unavailable. [Modeller](../../MODELS.md) |
| Backup v1 | Quiesced fiziksel ve tipli logical yedek, SHA-256 manifest, doğrulanmış ayrı restore, Server job ve interval/daily schedule, Native ABI 1. PITR unavailable. [Yedek](../../BACKUP.md) |
| Native SDK 0.4.0 | ABI 1, 45 önceki export ve ek oturum bilgisi, C++ RAII, gerçek yedek/bakım işi ve bağımsız örnek. Windows x64 Release ve ASan doğrulandı. |
| Workbench SQL ve explorer Etap 37 | Native 0.4.0 ve Qt 6.11.3. İki canlı TLS oturumu, gerçek PL/SQL, tipli parametre, rollback ve taslak kurtarma. [Kabul](STAGES/37.json) |
| Workbench tablo işlemleri Etap 38 | Gerçek TLS ile transaction içinde DDL, ERD, revision kontrollü grid ve tipli aktarım. [Kabul](STAGES/38.json) |
| Workbench PL/SQL Studio Etap 39 | Gerçek TLS derleme, breakpoint/stack/watch, yalıtılmış test ve profil. JDK 17 standalone ve JDBC bağlantılı çıktı. [Kabul](STAGES/39.json) |
| Workbench History ve Decision Etap 40 | Şema ve satır geçmişi, canlı recovery conflict denetimi, WHYNOT ve salt okunur simülasyon. [Kabul](STAGES/40.json) |
| Workbench ML, Dataset ve Job Studio Etap 41 | Server snapshot, K-Means ve sınıflandırma, doğrulanmış metrikler, iki süreçli CPU MLP resume ve bağımsız availability. [Kabul](STAGES/41.json) |
| Workbench DBA ve Backup Etap 42 | Gerçek TLS sağlık, ayrı doğrulanmış restore ve CLI/SDK yetkileri geçti. Server, Workbench kapalıyken SQL ve yeniden eğitim görevlerini tamamladı. Native 0.4.1 aynı ABI ile iki türü destekler. [Kanıt](STAGES/42.json) |
| Workbench Compare/Migration Etap 43 | İki TLS Server ile DDL revizyon koruması ve PostgreSQL 18.1 / MySQL 8.0.43 ile kesintiden sonra doğrulanmış aktarım. [Kanıt](STAGES/43.json) |
| Bağımsız Windows x64 MSI Etap 44 | Server, Workbench ve CLI paketleri, ayrı uygulama yerel DLL sahipliği ve manifest çözümleyicisi doğrulandı. İmzasız geliştirme çıktıları. [Kanıt](STAGES/44.json) |
| Ortak Setup ve M9 Etap 45 | Çevrimdışı MSI yaşam döngüsü, uzak ve taşınabilir Workbench, veri ve etkin Decision koruması. Yönetici oturumunda sanal Windows hizmet hesabı ve kapsamı doğrulanmış firewall kuralı geçti. [Etap 45](STAGES/45.json) · [Yönetici kabulü](M9_ADMIN_ACCEPTANCE.md) |
| Linux, macOS ve diğer Windows mimarileri | NOT_RUN. |

Wire v1 kaynakları [contracts/v1](../../contracts/v1/PROTOCOL.md) altında. Etap kanıtları [M1](M1_REPORT.md) , [M2](M2_REPORT.md), [M3](M3_REPORT.md) , [M4](M4_REPORT.md) ve [M5](M5_REPORT.md) raporlarında.
