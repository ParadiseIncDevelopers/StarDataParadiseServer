# Gereksinim → etap eşleşmesi

Eski 00–18 paketinin işlev kapsamı korunarak 50 küçük işe bölündü. Son kullanıcı kararları önce gelir: C++ ana uygulamalar, son proje isimleri ve yedi bağımsız depo. Eski Rust/Avalonia/monorepo tercihleri taşınmadı. Bu tablo uygulama tamamlanma raporu değil, kapsam izidir.

| ID | Gereksinim | Etaplar | Kapsam | Dayanak |
|---|---|---|---|---|
| R01 | Kesin yedi proje adı; Workbench adı doğrudan StarDataParadise | 01, 02, 50 | Zorunlu | Son kullanıcı isim/dizin kararları |
| R02 | Server/Workbench/CLI ve native DLL C++20; SDK’lar kendi dilinde | 02, 05, 07, 31, 32, 33, 34 | Zorunlu | Son kullanıcı teknoloji kararı |
| R03 | Ayrı source/repos kökleri, bağımsız Git/build/solution; mevcut projeyi koruma | 01, 02, 47, 50 | Zorunlu | Son kullanıcı repo kararı |
| R04 | Sıfırdan disk/WAL/SQL motoru; hazır DB kullanmama | 08, 09, 10, 11, 12, 13, 14, 48 | Zorunlu | 00–03 |
| R05 | İptal edilen Big Data/multi-model/Custom Libraries kapsamını geri almama | 01, 02, 50 | Zorunlu | 00 ve sonraki kullanıcı sadeleştirmeleri |
| R06 | Mevcut kullanıcı değişiklikleri, untracked/binary ve güvenli checkpoint | 01, 02, 47, 49, 50 | Zorunlu | Son kullanıcı: proje bozulmadan sıra |
| R07 | Sürümlü wire contract, hata kodu ve capability ayrımı | 03, 04, 05, 12, 31, 47 | Zorunlu | 01,09,10 |
| R08 | DLL ABI/allocator/exception güvenliği, native paket bağımsızlığı | 05, 31, 44, 47 | Zorunlu | C++ ve DLL kararları |
| R09 | Gerçek SQL CRUD/prepared queries/streaming/limit/cancel | 11, 12, 14, 31, 36, 37 | Zorunlu | 03,10,12,15 |
| R10 | İndeks/PK/FK/check/unique/default/identity/view ve transactional DDL | 13, 14, 38, 43 | Zorunlu | 02,03,12 |
| R11 | JOIN/group/subquery/CTE, optimizer ve execution plan | 14, 37, 48 | Zorunlu | 03,12 |
| R12 | UTF-8/Türkçe collation/decimal/NULL/timezone; format değişimi | 03, 11, 14, 32, 33, 34, 46, 47 | Zorunlu | 01,03,10,17 |
| R13 | Transaction/savepoint/izolasyon/WAL/durable commit | 09, 10, 12, 15, 48 | Zorunlu | 02,03,09 |
| R14 | Crash recovery/torn page/checkpoint/Integrity Checker | 08, 10, 13, 19, 29, 30, 48, 49 | Zorunlu | 02,05,09,18 |
| R15 | PL/SQL blok/function/procedure/cursor/trigger/exception | 16, 17, 36, 39 | Zorunlu | 04,12,13 |
| R16 | Typed IR, routine sürümleri, debug/test/profiler ve limitler | 16, 18, 35, 39 | Zorunlu | 04,11,13 |
| R17 | History temel kaydının transaction ile atomik olması | 09, 12, 19, 20, 48 | Zorunlu | 00,02,05 |
| R18 | History log/diff/schema/routine/model/rule geçmişi | 19, 20, 21, 25, 40 | Zorunlu | 05,06,07,13 |
| R19 | Row/table/field restore; DROP/TRUNCATE; revalidation ve revert | 13, 20, 40, 49 | Zorunlu | 05,13 |
| R20 | History retention/pin/GC/kota/PURGE/VERIFY ve güncel yetki | 19, 20, 24, 28, 40 | Zorunlu | 05,07,13 |
| R21 | Deterministik Decision validation/transition/computation | 21, 22, 40 | Zorunlu | 06,13 |
| R22 | Decision bütün yazma yollarında uygulama; modül yokken koruma | 12, 21, 22, 45, 49 | Zorunlu | 06,16 |
| R23 | WHYNOT/WHATIF/recheck/impact/rule test; yan etki ve veri sızıntısı engeli | 22, 40, 48 | Zorunlu | 06,13 |
| R24 | Kalıcı job kuyruğu, izole worker, iptal, restart ve idempotent publish | 23, 25, 27, 28, 41 | Zorunlu | 07,08,13 |
| R25 | Task Scheduler: bakım, SQL, backup ve retrain; timezone/DST | 23, 29, 30, 42, 49 | Zorunlu | 08,09,14 |
| R26 | CPU/RAM/GPU/temp disk/iş kotaları; OLTP rezervi | 08, 09, 14, 16, 23, 28, 48 | Zorunlu | 02,03,04,08,18 |
| R27 | SQL dataset, snapshot/lineage ve leakage-free preprocessing | 24, 25, 26, 41 | Zorunlu | 07,13 |
| R28 | Kullanıcı PL/SQL hesaplama/modelleme; typed zengin sonuçlar | 16, 17, 24, 31, 32, 33, 34, 35 | Zorunlu | 04,07,10,11 |
| R29 | Linear/logistic/tree/forest; gerçek eğitim ve metrikler | 25, 41 | Zorunlu | 07 ve Workbench kapsamı |
| R30 | K-Means/DBSCAN/PCA; metrik ve prediction sınırları | 26, 41 | Zorunlu | 07,13 |
| R31 | Deep learning MLP CPU; gradient/optimizer/loss/inference | 27, 28, 41 | Zorunlu seçilebilir modül | 08,13 |
| R32 | GPU tespiti/backend/quota; destek ve test iddiası ayrımı | 28, 41, 44, 45, 49 | Koşullu: gerçek donanım ve seçilmiş paket | 08,16 |
| R33 | Gerçek checkpoint/RNG/optimizer/data iterator ile resume | 23, 27, 28, 41 | Zorunlu MLP yolu | 08 |
| R34 | Model registry/version/active pointer/rollback/History | 25, 26, 27, 28, 41 | Zorunlu | 07,08,13 |
| R35 | TLS/auth/user-role/GRANT-REVOKE standart yetkileri | 04, 15, 30, 42, 48 | Zorunlu | 09,14 |
| R36 | Service & Instance Manager; çoklu data/port/service izolasyonu | 04, 30, 42, 45, 49 | Zorunlu | 09,14,16 |
| R37 | Config validation/restart/dynamic, safe apply ve protection | 04, 30, 42, 46 | Zorunlu | 09,14,17 |
| R38 | Ayrı logical/physical backup; gerçek restore doğrulama | 29, 42, 49 | Zorunlu | 09,14,16,18 |
| R39 | PITR ile History time-travel ayrımı; yanlış destek iddiası olmaması | 19, 29, 42, 50 | PITR koşullu; temel restore zorunlu | 09,14 |
| R40 | Health Center checked_at/evidence/unknown; gerçek tanılama | 30, 42, 45, 49 | Zorunlu | 09,14,16 |
| R41 | Query/Audit/WAL/History/operational/ML/Decision log ayrımı | 04, 15, 19, 22, 23, 30, 42 | Zorunlu | 00,09,14 |
| R42 | Support bundle redaction/preview; dışarı otomatik paylaşmama | 30, 42, 46, 48 | Zorunlu | 09,14,17 |
| R43 | Pool tanılama/reset, unknown transaction outcome ve güvenli retry | 12, 15, 31, 32, 33, 34, 47 | Zorunlu | 09,10 |
| R44 | C++/.NET/Java/Python yerel paket ve dört dil contract testleri | 31, 32, 33, 34, 47, 50 | Zorunlu | 10 ve DLL kararı |
| R45 | PL/SQL->Java kaynak export, standalone ve connected ayrımı | 35, 39, 49 | Zorunlu desteklenen alt küme | 11,13 |
| R46 | Satranç örneği; sınırlı gerçek algoritma ve Server kapalı Java testi | 35, 39, 49 | Zorunlu örnek; tam satranç motoru değil | 11 ve kullanıcı satranç açıklaması |
| R47 | Workbench first-run/connection/TLS/credential/opsiyonel SSH | 07, 37, 42 | Zorunlu bağlantı; SSH gereksinime bağlı | 12 |
| R48 | Workbench SQL editor/snippet/bookmark/completion/refactor sınırları | 37, 38, 42, 43 | Zorunlu; kör rename yok | 12,14 |
| R49 | Grid/edit/conflict/PK güvenliği/BLOB/large result virtualization | 38, 48 | Zorunlu | 12 |
| R50 | Import/export encoding/mapping/reject; ERD/data dictionary | 38, 42 | Zorunlu | 12,14 |
| R51 | Workbench PL/SQL debugger/test/profiler ve Java/SDK Studio | 39 | Zorunlu | 13 |
| R52 | Workbench History/Decision/ML stüdyoları ve SQL/API önizlemesi | 40, 41 | Zorunlu | 13 |
| R53 | Workbench DBA/permissions/scheduler/log/backup/help | 42 | Zorunlu | 14 |
| R54 | Profile/workspace backup/crash recovery/global search/shortcuts | 07, 37, 42 | Zorunlu | 12,14 |
| R55 | Schema/data compare, safe deploy ve dependency revalidation | 13, 38, 43 | Zorunlu | 14 |
| R56 | MySQL/PostgreSQL migration, snapshot-safe resume ve uyumsuz kod raporu | 43 | Zorunlu adapter kapsamı | 14 |
| R57 | Renkli CLI/multiline/Tab/history; meta-command vs SQL | 06, 36 | Zorunlu | 15 |
| R58 | CLI JSON/CSV/NDJSON/stderr/exit codes/terminal-escape/NO_COLOR | 06, 36, 48 | Zorunlu | 15 |
| R59 | CLI tam yetkili yönetim ve istemciden bağımsız jobs | 36, 49 | Zorunlu | 15 |
| R60 | Bağımsız MSI ve ortak wizard; component dependency/ownership | 44, 45, 49 | Zorunlu | 16 |
| R61 | Optional ML/DL/GPU/Decision; ileri istemci/SDK/docs seçimleri | 44, 45, 49 | Zorunlu | 00,16 |
| R62 | Prerequisite/port/firewall/TLS/servis hesabı/post-install health | 44, 45, 49 | Zorunlu | 16 |
| R63 | Modify/Repair/Reconfigure/Uninstall; data-directory protection | 45, 49 | Zorunlu | 16 |
| R64 | Silent/offline/per-user/portable ve güvenli response-file | 44, 45, 49 | Zorunlu desteklenen hedefler | 16 |
| R65 | Update Manager/version/format/ABI/runtime/collation compatibility | 46, 47, 49 | Zorunlu | 17 ve C++ kararı |
| R66 | Kesintili upgrade, eski binary/yeni format koruması; rollback sınırı | 46, 49 | Zorunlu | 17 |
| R67 | Sanitizer/fuzz/concurrency/resource/security/crash regression | 10, 15, 18, 47, 48, 49 | Zorunlu hedef araç kapsamı | 18 ve C++ kararı |
| R68 | Gerçek benchmark/SBOM/lisans/checksum/signing durumu | 02, 44, 48, 50 | Zorunlu; production imzası sertifikaya bağlı | 18 |
| R69 | Release artifacts, Türkçe/offline belgeler, örnekler ve açık sınırlama | 42, 50 | Zorunlu | 18 |
| R70 | Her beş etapta milestone; test olmadan sonraki aşamaya geçmeme | 05, 10, 15, 20, 25, 30, 35, 40, 45, 50 | Zorunlu | Son kullanıcı: 50 etap, proje bozulmadan ilerleme |

## Açık sınırlar

History branch/merge, hazır veritabanına dayalı motor, LLM/MCP ve iptal edilmiş Big Data özellikleri bu pakete eklenmez. Mevcut hedef log/diff/revert/restore’dur. PITR varsa gerçek base-backup/WAL zinciriyle test edilir; aksi halde unavailable kalır.
SSH tunnel, üretim imzası, ilave işletim sistemleri ve GPU donanım doğrulaması koşulları belgede ayrıdır. Bunların olmaması gizlenmez. Temel TLS bağlantısı, Windows/CPU testleri ve doğrulanmış backup/restore isteğe bağlı değildir.
Satranç talebi algoritmayı uygulamaya taşıma kullanımını doğrular; tam satranç oyunu veya tüm PL/SQL/ML kodunun sınırsız Java dönüşümü vaat edilmez.
Bir gereksinimin Server ve Workbench etaplarında birlikte bulunması tekrar geliştirme değil backend/istemci/test ayrımıdır. Paket kontrolü yalnız eşleşme ve dosya bütünlüğünü sınar; yazılım doğruluğunu kanıtlamaz.

Etap 11 katalog, SQL grameri ve tip denetimi kabul kanıtı [Etap 11 kaydında](STAGES/11.json).

Etap 12–15 SQL ve güvenlik kabul kanıtları [M3 raporunda](M3_REPORT.md).

Etap 16–20 PL/SQL ve History kabul kanıtları [M4 raporunda](M4_REPORT.md).

Etap 21–25 Decision, Jobs ve CPU model kabul kanıtları [M5 raporunda](M5_REPORT.md).
