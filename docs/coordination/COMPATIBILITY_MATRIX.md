# Uyumluluk ve yetenekler

| Bileşen | Doğrulanan durum |
| --- | --- |
| Windows x64 | Visual Studio 2026, v145, Windows SDK 10.0.26100.0, CMake 4.2.2 |
| Workbench | Qt Base 6.11.3. Bağlantısız başlangıç, iki bağımsız native TLS oturumu, Windows Credential Manager ve sır içermeyen profil dışa aktarımı doğrulandı. |
| CLI | C++20. Native ABI 1 ile TLS, DNS, profiller, terminal ve JSON durum çıktısı doğrulandı. |
| Server | TLS 1.2+, bootstrap, auth, hello, health, capabilities, süreç ve dizin kilidi. Adres varsayılanı loopback. |
| Native DLL | Ürün 0.3.0, C ABI 1, wire v1. Kurulu paket tüketicisi bağlanıp gerçek tipli CRUD çalıştırdı. |
| .NET, Java, Python SDK | Yerel paket 0.1.0 ve paket smoke testleri. Ağ istemcileri sonraki SDK etaplarında. |
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
| Linux, macOS ve diğer Windows mimarileri | NOT_RUN. |

Wire v1 kaynakları [contracts/v1](../../contracts/v1/PROTOCOL.md) altında. Etap kanıtları [M1](M1_REPORT.md) , [M2](M2_REPORT.md), [M3](M3_REPORT.md) , [M4](M4_REPORT.md) ve [M5](M5_REPORT.md) raporlarında.
