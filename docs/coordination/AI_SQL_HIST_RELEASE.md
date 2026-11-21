# AI/Knowledge ve kısa SQL/PLSQL release çalışması

Son kabul ve yayın durumu [Stable release kaydında](STABLE_RELEASE_2026_11_21.md)
toplanır. Aşağıdaki günlük eski denemeleri de içerir; tarihsel NOT_RUN/HIST
planları güncel kapsam ve son kabul tablosunun önüne geçmez.

## Son kapsam kararı - 21 Kasım 2026

Kullanıcı HIST/Git ve yeni SQL JOB sistemini şimdilik çıkardı; bunlar sonraki
etaba ertelendi. Bu karar aşağıdaki eski HIST/JOB planlarının önüne geçer.
HIST komutları, branch/merge/remote/sync, geçmiş ağacı ve yeni JOB ekranları bu
release'e girmez. Kaynak ve kabul kanıtları ayrı yerel WIP kaydında korunur.
Mevcut compute API'si ve kullanıcı verileri kaldırılmaz. AI run/model import
yürütmesinin dahili dayanıklı kuyruk ihtiyacı, kullanıcıya sunulan yeni SQL JOB
özelliği olarak yayımlanmaz. Release kapsamı AI/Knowledge, normal SQL/PLSQL
iyileştirmeleri, ilgili Workbench ekranları ve dört SDK'nın native API'sidir.

Build11'in gerçek ORDER/pipeline, PL/SQL local capture ve legacy SQL/SP/FOR
kabul kapıları geçti. Build12 kapsamı bu karara göre AI+SQL olarak ayrılıyor;
FULL OUTER ve Knowledge/context runtime kabulü henüz bekleniyor. AI tools/onay
native kabulü tamamlanmadan AI sistemine bitti denmez.

Araç hata ABI'si 510005/61085 eklendi. İlk AI turun `commit_state` bilgisi ile
aracın `action_commit_state` bilgisi ayrı korunur. `call_id`,
`cause_native_errno`, `cause_sqlstate` dört SDK'ya taşınır. 61085 yalnız native
yürütücünün kendi transaction'ını başarıyla geri aldığı ve COMMIT denemediği
durumda kesin rollback metadata'sı içerir; kayıp bağlantı veya COMMIT şüphesi
bilinmeyen sonuç olarak kalır. Python8/Node6 native birim kontrolü geçti;
Java tam paket kontrolü ve .NET uyarlaması sürüyor.

Ertelenen HIST kaynakları Server yerel Git dalında `4c874f5`, Workbench'te
`b02b526` commit'leriyle `codex/deferred-hist-job-20261121` referansında korunur.
Bu dal release değildir ve push edilmedi. Sunucuda 26 yeni HIST kaynak/test/
sözleşme dosyası hash denetimiyle `out/future-wip/hist-20261121/deferred-source`
altına taşındı; önceden var olan beş legacy history dosyası korundu. Native
96 dosyalık WIP zip'i SHA256
`1af4327c43f52a0f3a50aa2cd898514c5f6636285c017f13146ac226698201af`
ile, CRC ve dosya SHA denetimlerinden geçti.

Kararlı paket kontrolleri Python0.3.0, Node0.2.0 ve Java0.3.0 için geçti;
kurulu/packed tüketici, değişmez wire arşivi, native testleri ve Java Javadoc
dahildir. Loglar `out/sdk-native/{python,node,java}-stable-ci.log` olur.
Server aktarım14 ve worker20 kontrolü geçti. `compute-ai-only-scope.log` gerçek
sunucuda eski kayıtların korunmasını, AI inference/import ayrımını, iptal,
timeout, gerçek worker kaybı ve mevcut UTC compute schedule davranışını
doğruladı; yeni HIST schedule genişlemesi release kodundan çıkarıldı.

AI-only native12 kaynak snapshot'ı temiz upstream'e uygulanıp normalize kaynak
byte'larını aynen üretti:78 dosya,333.060 byte, SHA256
`7d53c83646f57c4861562cb0693e9bcf2390e7e9e0b0b7f17d68bbcc0c2879b7`.
Bison36 ve11 translation unit `/Zs` geçti; tam build/kabul sürüyor. Capability
kapıları gerçek kabul tamamlanmadan açılmaz.

`.NET` kararlı paketleri2.1.0 ve Framework2.0.0, net8/net10/net48 kurulu ADO ve
native tüketicilerde PASS/0warning/0error verdi. Yeni510005 action ABI gerçek
native13 binary kabulü ayrıca bekleniyor. Paket/hash manifesti
`out/dotnet-release-diagnostics/stable-packages.json`; beş SDK paketi referansı
`packaging/sdk-artifacts.json` içinde güncellendi.

Workbench20 backend kontrolü ve sekiz native sayfanın extended ACTION form
probe'u PASS: `out/classic-provider-form-acceptance/action-native-result.json`.
Model/Behavior/yanıt şeması USE, konuşma SELECT/APPEND ve yordam/Knowledge
EXECUTE ayrımı düzeltildi. Açık typed araç listesi ve PREPARE/DESCRIBE/APPROVE/
DENY/EXECUTE ACTION SQL önizleme kontrolleri eklendi. Gözlenen action_id literal
olarak sabitlenir; değişiklikte önizleme geçersizleşir. İlk probe, native
TextEntry programatik set_value çağrısının changed callback üretmediğini buldu.
Komut anında kimlik/hedef eşleşmesi de doğrulanarak stale önizleme engellendi;
gerçek native tekrar geçti. Probe metadata fixture kullanır; network, credential
write ve sunucu mutasyonu yoktur. Canlı sunucu kabulünün yerine sayılmaz.

`install --queue-only` gerçek fresh sunucuda yalnız compute_jobs tablosu kurdu;
schedule/occurrences yokluğu ve eski compute regression kabulü PASS
(`out/ai-provider-tests/compute-internal-queue-only.log`). Worker21 kontrolü,
boş WITH FUNCTIONS allowlist'inin örtük araç açmadan normal inference yapmasını
da doğruladı. Native13'te aynı semantik SQL tarafına bağlanıyor.

Action/Role native13 profili readonly global startup `sdp_ai_actions` ile
seçilir. Kurulum/yükseltme bakım profilinde `--sdp-ai-sql=ON
--sdp-ai-actions=OFF` ile catalog → tool → action sırasını izler; temiz yeniden
başlatmadan sonra release profili `--sdp-ai-sql=ON --sdp-ai-actions=ON` olur.
Action profili açıkken eksik/değişmiş policy helper hata verir; kurulum için
örtük atlama yapılmaz. İki bayrak da startup-only'dir. Public ACTION/rol SQL'i
ikisinin açık olmasını ister; ordinary CALL onay denetimi ai_sql startup OFF
olduğunda da actions ON ile sürer. Yönetilen action kataloğu kuruluysa bakım
actions OFF profilindeki DROP/RENAME de eski grant ve henüz yürümemiş onayları
aynı native ACL transaction'ında temizler. Core-only AI kurulumu actions OFF
kalabilir; capability ayrı `ai_action_v1` ve `ai_role_acl_v1` kapılarından
bildirilir. Gerçek native13 Action/rol kabulü tamamlanmadan bu kapılar kabul
edilmiş sayılmaz.

### Dar runtime düzeltmeleri ve güncel doğrulama

AI-only12 tam build geçti. Gerçek kurulu Python0.3.0 wheel'i ile GGUF
import/load, native SQL inference, sync TEXT/JSON capture, provider I/O sırasında
business COMMIT/satır kilidinin bırakılması ve ASYNC rollback kabulü PASS oldu
(`build12-installed-wheel-local-llm.log`). Packed Node0.2.0 ve dış JAR
Java0.3.0 TLS tüketicileri JSON/U64/Decimal65/BLOB/NULL/TIME, rollback,
parametreli CALL/OUT, AS RETURN ve büyük UTF-8 sonuçlarını doğruladı.

12a'da basic Knowledge/prepared capture, before/after COMMIT hata bilgisi ve
FULL OUTER JOIN'in bağımsız kapsamlı runtime kabulü geçti. 12b'de Knowledge'ın
SELECT/LEFT JOIN tablo kaynağı, PS metadata/rebind, caller rollback/purity,
kesin uint64/Decimal JSON context, Behavior ve konuşma ALTER yaşam döngüsü de
PASS oldu (`build12b-context-core.log`). Knowledge+FULL birleşimi ayrıca
InnoDB autocommit assertion'ı buldu; explicit START TRANSACTION/ROLLBACK aynı
12b sorgusunu doğru sonuç ve temiz kapanışla geçirdi. LEFT Init/materialize
işlemini RIGHT spool'dan önce yapma düzeltmesi 12c'ye hazırlanıyor; gerçek
yeniden kabul tamamlanmadan birleşik sorgu geçmiş sayılmaz.

Güncel nupkg byte'larıyla çalışan .NET net8/net10/net48 gerçek TLS testi,
parametreli non-UTF8 byte dizisinin sonuç metadata'sında yanlış charset
bildirildiğini buldu. Test gevşetilmedi ve SQL CAST/UNHEX ile değiştirilmedi.
Native bridge bytes bağları MYSQL_TYPE_BLOB olarak gönderir; native sunucunun
explicit binary descriptor'ı protocol sınırında koruması ayrıca 12c'de
doğrulanacak. Node/Java gerçek tüketicilerine boş/non-UTF8/UTF8 binary,
normal metin ve NULL bağ testleri eklendi.

Server yönetim uygulaması product_version ve CMake sürümü0.5.0 olarak
eşleştirildi; Release build ve help/version/invalid_option3 CTest PASS
(`out/release/server-wrapper-*`). Build'deki mevcut makro yeniden tanımlama
uyarıları kayıtta korunur. Wire15 +worker22 kontrolü birlikte37 PASS
(`transport-worker-current-v3.log`). İlk iki komut eksik PYTHONPATH nedeniyle
import hatası verdi; pinned connector ve wire module yolu eklenerek düzeltildi.
Bu ortam hataları native/SDK kabul sonucu olarak sayılmaz.

Native13 tools/actions/roles/UTC ve gerçek capability/UI uçtan uca kapıları
henüz bekleniyor. Canlı OpenAI/Claude anahtarlı çağrıları NOT_RUN; anahtarlar
sohbetten istenmedi ve gerçek yerel GGUF kabulü cloud kabulü sayılmaz.

12c tam build0 ve79 kaynak byte denetimi PASS; snapshot SHA256
`30630c5ab1707d585a8afac9e75dc54e80ef3e20e466c5a2d05cb4538e2408d2`,
mysqld SHA256 `a5d465ddc8580090418f870d28eb05c8e2d9257e21907a589a94a1820c463d66`.
Gerçek autocommit Knowledge/FULL birleşimi sol/sağ yön, tekrarlı PS rebind,
boş kaynak, unmatched kayıtlar ve unsigned metadata ile PASS oldu. Bağımsız
FULLextended, temel Knowledge ve commit-DD guard tekrarları da PASS/cleanexit
verdi. Güncel nupkg byte'ları doğrulanmış .NET net8/net10/net48 TLS tüketicileri
ve Node/Java'nın non-UTF8/boş binary dahil genişletilmiş TLS kabulü geçti.

Raw binary fixture'ın CAST kontrollerinde kendi hataları düzeltildi: uint64
değeri signed-string yerine unsigned integer PS paketiyle bağlanır; binary
JSON CAST'in native opaque/base64 semantiği metin JSON ayrıştırmasından ayrı
beklenir. OUT sonucu istemek için CLIENT_PS_MULTI_RESULTS eklenince tanımlı
VARCHAR OUT'un giriş binary collation'ını koruduğu gerçek hata görüldü.
Native13'e yalnız direct PARAM_ITEM + OUT bilgisi + simple string koşulunda
tanımlı charset'i kullanma düzeltmesi eklendi; sql_class.cc `/Zs` PASS.
Generic Item ve user-variable charset yolları değiştirilmez. Son raw OUT
runtime kabulü yeni binary'yi bekler.

Güncel AI provider/schema/manifest/owned-process/worker ve wire grupları
birlikte71 unit PASS (`ai-transport-current.log`). Workbench20 backend ve
en son paketlenmiş native8 sayfa/2 diyagram/provider/action form probe'u PASS
(`backend-final-native-result.json`). Gerçek sunucu capability/katalog ve
main-session action önizlemesi için ayrıca workspace_live_acceptance hazır;
bu test henüz çalışmış sayılmaz.

### Native13 birleşimi - henüz release kabulü değil

93 dosyalık tam kaynak snapshot'ı temiz pinned upstream'e uygulanıp kaynakları
aynen üretti; SHA256 `3085e3798c5606637adaaf533abf15dd4d20cd7ad1a596c0378ca59c52be8045`.
Birleşik Bison36, etkilenen tüm TU `/Zs` ve tam MSVC build PASS/exit0 verdi.
Immutable mysqld SHA256 `9a16886bd3b846f3c45f6af39cbd2de9455b64e16706c68b2b899d38cccf64c4`;
93 dosya listesi ve her kaynak hash'i build sonrası tekrar doğrulandı.

Altı ordinary SQL regresyon ailesi13 üzerinde PASS: projection/SP/FOR,
relational, inspection/persistence, named insert/views, ORDER ve extended FULL.
Aktif RPC sırasında çağrılmayan JSON_KIND/ASSERT_WORKER rutinlerinin ALTER'ı
doğru DD kilitlerinde bekledi, RPC bitince ikisi de geçti. Knowledge numeric
ALTER2, Behavior ALTER2 ve mevcut dispatch ile response schema ALTER2 gerçek
kabulü PASS; üç model seçeneğinin UI SQL ifadeleri de metadata'yı doğru üretti.
Bu son test cloud/model I/O iddiası içermez. Önceki gerçek GGUF testi ayrıdır.

Yeni post-OUT devam testi13'te gerçek wire hatası buldu: classic EOF dalı OUT
sonuç sonunda MORE_RESULTS bayrağını düşürdüğünden son OK paketi sokette kalır;
sonraki prepared sorgu0 parametre gibi görünür. Raw OUT charset tek başına
PASS, raw continuation ve yeni kurulu .NET/Node/Python devam testleri FAIL
oldu. Native14 için yalnız EOF wire flag'lerini düzeltme hazırlanıyor; bağlantı
körlemesine drain edilmez ve SQL yeniden yazılmaz.

Action13 ilk gerçek kabulü FAIL: nested private metadata RPC'nin SELECT sonucu
SUB_STMT_FUNCTION/MULTI_RESULTS denetimine takıldı1415. Gerçek custom GRANT da
native DCL'nin geçici autocommit wrapper'ını kullanıcı transaction'ı sandığı
için61081 verdi. İkisi dar, verified private RPC/DCL istisnası olarak14'e
hazırlanıyor; diğer transaction ve SP kısıtları korunacak. İlk gerçek prepared
Knowledge tool geçti; function registration Routine.load çökmesi ayrıca14
runtime düzeltmesi bekliyor. Full tools/actions ve yeni installedSDK61085
kanıtı tamamlanmadan capability kapıları açılmaz.

UI edit kontrolü düzeltildi: tanım gövdesi doğrudan çalıştırılmaz; ayrı body
editöründen tam güncelleme SQL önizlemesi üretilir. JSON Decimal'leri sayı
kalır, mevcut parametre/sonuç tipi ve NULL bilgisi korunur.27 backend ve en son
native8 sayfa/JSON editör/stale action disable probe'u PASS. Knowledge'ın
canonical CHARACTER SET/COLLATE metadata'sının roundtrip'i13 parser'da1064
verdi;14 için iki column production'a opt_collate eklemesi izole Bison36 ve
parser `/Zs` PASS, gerçek full-collation gate'i bekliyor. Editör140 gerçek
lexer/completion +splitter12 +failed-generator gate PASS verdi.

Salt okunur release envanterinde kaynaklara sızmış temp/credential/binary
adayı bulunmadı; beş SDK paket hash'i mevcut dosyalarla eşleşti. Proje sınırları
doğrulanarak12 yeniden üretilebilir Python cache dizini temizlendi; kullanıcı
ayarları, tasarım görselleri ve ertelenen HIST WIP korunur
(`out/release/source-cache-cleanup.json`). Payload ürün adları normalleştirildi;
eski unsigned MSI oluşturucusunun development güven politikası değiştirilmedi.

21 Kasım 2026 (20 Kasım'da başladı). Kullanıcı üç aileyi ekranlarıyla birlikte tamamlamayı, gerektiğinde SDK güncellemelerini, testlerden sonra preview olmayan release commit/push işlemini ve en son MySQL installation wizard'ın açılmasını istedi. Bu kayıt devam eden çalışmadır; tamamlanmamış motor dilimleri release olarak işaretlenmez.

Önceki dosya adı/açılış düzeltmeleri master'a Workbench `a98eb36`, Server `b309a79` ile sürüm artırmadan gönderildi. Yeni dallar iki depoda da `codex/ai-sql-hist-release` olur. Ortak modal stili Workbench `e4bfb53`, kabul kaydı Server `f85a908` ile özellik dalına gönderildi. Bu başlangıç commit'leri yeni stable release değildir.

## Geçerli kullanıcı kararları

- Yeni ekranlar mevcut Classic native arayüzüne bağlanır. Knowledge bağımlılıkları diyagram, sürümler timeline, HIST geçmişi gerçek ebeveynlerden çizilen graph/işlem akışı olur.
- Başlık/bilgi ayıracı ` - ` olur. Modal işlem düğmeleri koyu petrol yeşili `#043133`, yazıları beyazdır; disabled, focus, yüksek kontrast ve native güvenlik çizimi korunur.
- Yeni konuşma formundan sonra ayrıca SQL yürütme ekranı bulunur; form açılması betiği kendiliğinden çalıştırmaz.
- Compact ayrı SQL modu/keyword değildir. Kısa biçimler normal SQL/PLSQL içindedir.
- API sağlayıcısı kullanıcının OpenAI veya Claude anahtarı olabilir; üçüncü seçenek kendi yerel LLM dosyasını içe aktarmaktır. İlk yerel format GGUF/llama.cpp olarak uygulanıyor.
- Yeni hata gösterimi altı basamaklı sayısal kod olur; mevcut hata ABI/protokol anlamları korunur.

## Önceki durum - ilk çalışma notu

| Bileşen | Durum ve kapsam |
| --- | --- |
| Native Workbench ekranları | Dokuz mforms çalışma alanı sayfası, üç Cairo diyagram yolu, parametre/kaynak/credential modalleri ve ayrı AI SQL ekranı gerçek uygulamada oluşturuldu. 13 backend kontrolü ve native probe geçti. Sunucu katalog/komutlarıyla uçtan uca kabul bekliyor. |
| AI provider/JSON/worker | 46 unit geçti. Native SQL → gerçek GGUF import/load → ortak JOB → gerçek inference → sync TEXT/JSON metadata capture geçti. Bağımsız bağlantı provider I/O sırasında business COMMIT ve satır kilidi bırakılmasını doğruladı. OpenAI/Claude gerçek anahtarlı çağrı yapılmadı; native araç yürütücüsü kabulü sürüyor. |
| Ortak JOB | Mevcut compute tablosuna job_kind/payload migration eklendi; ikinci bağımsız kuyruk oluşturulmadı. Gerçek compute kabulü ve AI/HIST iş türü izolasyonu geçti. |
| Native AI SQL | Build8 tam MSVC/fresh initialize/TLS; katalog, keyword/UDF OFF/ON, Knowledge, tam supplied response/capture kabulü geçti. Private hesap, per-routine grant korunması, escaped hesaplar ve NO_BACKSLASH_ESCAPES reinstall geçti. Behavior/context/tools ve PL/SQL local capture genişlemesi sürüyor. |
| Kısa SQL/PLSQL | Build7 projection/SP/LET/FOR, relational QUALIFY/BY NAME/pipeline ve SHOW EXPANDED binary prepared metadata/source maps/durable dil sürümü üç kabulü geçti. View/ODKU, sıra koruyan pipeline ve PL/SQL local capture sonraki birleşik derlemeye alındı; FULL OUTER sürüyor. |
| HIST | Build8 tam native kabul geçti: handler capture/savepoint/rollback, actor/ACL, typed BLOB/latin1, plan/apply/restore/stale/CHECK/idempotent receipt, event/ACK/tag, graceful ve forced crash atomic recovery. Session branch/LAST/remote/merge/sync genişlemesi sürüyor. |
| SDK/CLI | Dört SDK ayrı native API/structured hata/typed sonuç içerir. Python, packed Node, external-JAR Java ve .NET net8/net10/net48 gerçek TLS/PS/AS RETURN/CALL/OUT/TIME/chunk kabulünü geçti. Sürümler ve final paketler henüz stable release'e çevrilmedi. |
| Release/installer | Henüz hazır değil. Kabul tamamlanmadan preview etiketi stable'a çevrilmez; son wizard adımı henüz uygulanmadı. |

## Çalıştırılan kontroller ve sınırlar

Yeni AI testleri: `python -B -m unittest compatibility.tests.test_ai_providers compatibility.tests.test_ai_response_schema compatibility.tests.test_ai_worker -v` - 30 PASS. Bunlar provider/worker sözleşme ve aktarım fixture testleridir; harici gerçek sağlayıcı kabulü veya native AI SQL kabulü değildir.

Ortak JOB gerçek kabulü: `python -B -m compatibility.tests.acceptance --case jobs` - PASS. Final log `out/ai-provider-tests/compute-regression-final.log`. Eski schema upgrade, atomik queue davranışları, token tekrarları, iptal, timeout, gerçek çalışan kaybı, UTC schedule ve job_kind izolasyonu doğrulandı. Geçici TLS sunucu normal kapandı.

İlk provider testinde Windows sandbox hard-link yayını reddetti; Windows'un var olan hedefi değiştirmeyen rename yolu kullanıldı. Unicode hata fixture'ı geçersiz JSON yerine ayrı Unicode kodunu bekleyecek şekilde düzeltildi. Sonraki 30 test geçti. İlk native UI başlatma denemesi launcher shell yaşam süresi nedeniyle boş çıktı verdi; probe JSON'una kadar helper'ı açık tutan son çalıştırma native 9 sayfa/diyagram/modal kabulünü geçti. Kaynak ağacında testten oluşan altı ignore edilmiş `.pyc` dosyası temizlendi; kaynak ve kullanıcı ayarları silinmedi.

Gerçek OpenAI/Claude API key'i kullanılmadı. Resmi llama.cpp binary'si ve sabit GGUF ağırlığıyla inference ve native SQL uçtan uca kabulü geçti. Sağlayıcı request formatları resmi API belgelerine karşı kontrol edildi; anahtarlar log/SQL/transcript içine taşınmadı. Ayrıntı `compatibility/ai/README.md` içinde tutulur.

## Önceki kabul - build5 ve yerel LLM

Build5 tam sunucu derlemesi `out/ai-native-check/build-fifth-resumed.log` / `.exit`: **0**. Temiz `--initialize-insecure`, TLS ve VERSION() bootstrap geçti. Testler değişmez `out/sql-short-native/build5-runtime/` kopyası ve ayrı geçici datadir/port kullanır; kullanıcı sunucusu kapatılmaz.

- `compatibility/wire_v1/native_fork.py --mysqld out/sql-short-native/build5-runtime/mysqld.exe`: **PASS**. Gerçek TLS native prepared JSON/U64/DECIMAL65/BLOB, signed/25h TIME, unsplit SP, CALL/OUT/multiple sets, AS RETURN/:param, bound injection text, rollback, tek top-level statement, chunked UTF-8, COMMIT sonrası SIGNAL unknown outcome ve caller ACL. Log `out/sdk-native/native-build5.log`.
- `compatibility.tests.acceptance --case jobs --mysqld ...build5-runtime/mysqld.exe`: **PASS**. Owner576 migration, shared compute/AI/HIST isolation, retry/cancel/timeouts/actual worker loss/schedule. Log `out/ai-provider-tests/compute-build5.log`.
- Provider/worker/schema/owned-process: **44 unit PASS**. Araçların tek atomik typed batch olarak yayını, allowlist dışı/yanlış tür argüman reddi, protected call-ID-only execution, approval wait/no external effect testleri dahildir. Bunlar henüz native tool execution kabulü değildir.
- `compatibility.tests.ai_local_runtime`: **PASS gerçek import/load/inference**. Resmi llama.cpp b12248 Windows CPU asset SHA256 `bb306bfd390f8782c0acaba490505d12f07392791b48f1dd039387a5bacf1181`; ggml-org/tiny-llamas sabit `99dd1a73db5a37100bd4ae633f4cfce6560e1567` commit'inin `stories15M-q4_0.gguf` SHA256 `6151b1929d7f5aa3385d9ddef3393e55587c0a55de661562322bc51dfda93a04`, 19.077.344 byte. 51 karakter/15 token üretildi; context kapanınca runtime socket/process kapandı. Windows Job Object worker kaybında çocuğu kapatır, bellek/tek süreç sınırı ve credential/proxy/plugin env süzmesi uygulanır. Log `out/ai-local-runtime/real-inference.log`. Küçük base model yalnız gerçek inference kanıtıdır, ürün kalite tavsiyesi değildir; ağırlık/binary ignore edilen `out/` dışında paketlenmez.
- OpenAI/Claude gerçek anahtarlı harici çağrı: **NOT_RUN**, anahtar verilmedi. Resmi [OpenAI function calling](https://developers.openai.com/api/docs/guides/function-calling) ve [Claude tools](https://platform.claude.com/docs/en/agents-and-tools/tool-use/define-tools) biçimleri fixture'larda denetlendi. API anahtarı SQL/transcript/log'a taşınmaz.

Build5 kabulünde bulunan release engelleri: QUALIFY alias/source önceliği, pipeline derived table için null kolon-listesi crash'i, AI option Item_string borrowed buffer ömrü ve stored procedure içindeki native catalog RPC frame/protocol desteği. Düzeltmeler hazır/derleniyor; bu testler geçmeden stable release yayımlanmaz. Native capability kabul kapıları hâlâ false tutulur.

## Son kabul - build8

Tam derleme exit0: `out/ai-native-check/build-eighth.log`. Değişmez runtime SHA256 `DA43F51284AC4E855E92B5C1671CDEAA88A09119F5A41B805FF12F971757CAC2`. İlk runtime kopyasında `libprotobuf-lite.dll` eksikliği loader `0xC0000135` üretti; runtime ve library DLL dizinlerinin tam kopyasıyla düzeltildi. Bu loader başarısızlığı ürün kabulü sayılmadı.

- `out/ai-native-check/build8-local-llm-numeric.log`: **PASS**, gerçek yerel model ve native SQL senkron COMMIT/capture/kilit kabulü. Önceki denemede katalog `COALESCE(JSON_EXTRACT(...),4096)` ile context_tokens'ı string yapmıştı; SQL'de açık UNSIGNED cast ile gerçek JSON integer korundu, worker tip denetimi gevşetilmedi.
- `out/ai-provider-tests/build8-ai-response-diagnostic.log`: **PASS**, supplied JSON null/string/uint64/object, boş/UTF8 TEXT, SQL NULL reddi, AUTO/manual yanıt, native metadata JSON, pending okuma ve ASYNC capture reddi. Shared parser Item iki kez itemize edilmesiyle oluşan cleanup free-list cycle düzeltildi.
- `out/hist-native-build8-full-dll-acceptance.log`: **PASS**, tam HIST temel kabulü; mutating argüman AST denetimi kaynak/geçmişi değiştirmeden reddeder, güvenli SP-local/PS/CONCAT/REPEAT kabul edilir. VARCHAR schema String buffer taşması, typed binary görüntü ve internal publisher frame düzeltmeleri gerçek çalıştırmada doğrulandı.
- AI katalog grant/account fingerprint, native hook, numeric model setting, schema/ACL/lease/replay kabulü **PASS**; log `out/ai-provider-tests/build8-catalog-model-numeric.log`.
- Provider/worker/schema/owned-process **46 unit PASS**, `out/ai-provider-tests/provider-current-api.log`. Güncel Claude API sampling seçenekleri ve Bearer auth, araç batch ordinal sırası denetlenir.

Bazı Classic CLI scalar sorguları rc0 ile boş stdout üretti. Mutasyon tekrarı/otomatik retry eklenmedi; raw rc/stdout/stderr ve bağımsız readonly connector/PFS tanısı kaydedildi. Yeni fresh çalıştırmalar geçti; sapmanın client, harness veya sunucudan hangisine ait olduğu henüz kanıtlanmadı.

## İki sunucu remote FETCH ve paket kabulü

`out/ai-native-check/build8-hist-remote-recovery.log`: **PASS**, iki gerçek disposable TLS native sunucuda kaynak DML'den oluşan immutable revision closure export/chunk/SHA doğrulaması, parent-first DAG, global UUID içerik değişmezliği, owner+alias üyeliği, remote ref ve ortak JOB atomik yayını. FETCH hedef MAIN başlığını veya business verisini değiştirmez. Eksik ebeveyn ve UUID/içerik çakışması tüm metadata yayını geri alır. Lease kaybında yarım parçalar temizlenir ve aynı durable JOB yeni attempt ile tamamlanır; iptal sonraki staging'i reddeder. Aynı katalog reinstall bounded worker EXECUTE grant'lerini korur. SYNC schedule mevcut JOB clock ve overlap tablolarını kullanır; PAUSE schedule'ı durdurur. Bu sonuç native branch/MERGE APPLY kabulünün yerine geçmez.

Üretim RemoteClient/Python SDK ile iki native wire gateway üzerinden büyük kayıt kabulü ilk denemede fixture'ın TEXT sınırı1406 ile durdu; LONGTEXT düzeltmesinden sonra source DML'deki internal publisher1317 hatasını ortaya çıkardı. `sql_lex.cc` büyük sorguda Protocol_local'in olmayan ağ bağlantısını kapalı sayıyordu. Yalnız bu protocol için network probe dışarı alındı; `is_killed` ve gerçek client disconnect denetimleri korundu. Native syntax PASS, yeni binary kabulü bekliyor; veri bütçesi düşürülmedi.

Python wheel/sdist ve Node tarball CI, native API dahil ayrı kurulmuş tüketicide **PASS**: `out/sdk-native/python-package-native-ci.log`, `node-package-native-ci.log`. Paket sürümleri henüz değiştirilmedi. Native worker'lar yeni server'daki one-way session RPC guard'ını açar; manifest pinleri her kısa katalog çağrısında tutulur ve provider/network beklemesine taşınmaz. AI/HIST toplam64 unit **PASS**, `out/ai-provider-tests/protected-worker-unit.log`.

Birleşik build9, üç SQL-next ailesi, short-lived native RPC guard ve büyük Protocol_local query düzeltmesini sınamak için başlatıldı; eski build8 sonuçları yeni binary'nin kabulü sayılmaz.

Cloud credential egress sınırı: SQL model endpoint'i artık tek başına stored key'i başka HTTPS host'una yönlendiremez. Varsayılan resmi sağlayıcı URL'si; custom endpoint yalnız trusted credential bağındaki exact `endpoints` listesiyle kullanılabilir, optional `providers` de doğrulanır. AI/HIST unit toplam65 **PASS**, `out/ai-provider-tests/cloud-endpoint-boundary.log`. Workbench endpoint query/port doğrulaması ve owner-bound anahtarsız worker config parçası17 backend kontrolden geçti.

Classic scalar çıktı sapması için ayrı readonly probe, stdin sonu newline olan/olmayan ve `--execute` yollarında40'ar çağrı yaptı:120/120 doğru çıktı, `out/ai-native-check/cli-batch-probe-san.log`. İlk probe kendi otomatik sertifikasının IP SAN içermemesi nedeniyle TLS identity reddi aldı; explicit test SAN sertifikasıyla doğrulandı. Kaynak eski sapmanın nedeni bu probe'da tekrarlanmadığı için hâlâ kesinleştirilmedi.

## Native bütünleştirme ve SDK ilerlemesi (önceki etap kaydı)

İlk native derleme, ek token kimlikleri için digest generator kapasitesinin 1251 ile sınırlı olduğunu gösterdi. Kapasite 2048'e çıkarıldı; mevcut token, hint ve digest kimlikleri yeniden numaralandırılmadı. İkinci birleşik derleme AI seçenek tekrar denetiminde protected `Item::str_value` erişimini yakaladı; public `Item_string::val_str(nullptr)` yolu kullanıldı. Üçüncü birleşik derlemede parser ve `sql_main` kütüphanesi derlendi; tam `mysqld` bağlaması ve canlı native kabul henüz bekleniyor. Bison temel `%expect 37` sayısı artırılmadan geçti.

AI worker artık ortak `ai_import` işini de alır. Kaynak, yönetici yapılandırmasındaki `local_runtime.import_roots[actor]` klasörüne göre çözülür; parent kaçışı/link/junction ve açılmış dosyanın gerçek handle yolu denetlenir. SHA-256/boyut/GGUF kontrolünden sonra sahip olunan llama.cpp sürecinin modeli yükleyip sağlıklı duruma gelmesi gerekir. Dosya başlığının geçmesi tek başına modeli aktif yapmaz. Lease yenileme ve iptal kontrolü kopyalama sırasında yapılır. Provider/worker/schema toplam **36 unit PASS**. Runtime fixture'ı kullanan testler gerçek inference değildir.

`compatibility/ai/native/sdp_ai_commit.h`, kalıcı yerel commit sonrasında monotonic süreli senkron bekleme, kısa autocommit durum okumaları, native-only recursion guard, JSON sonuç metadata'sı ve run kimlikli 511003 bekleme hatasını uygular. İzole gerçek MSVC `/Zs` kontrolü PASS; devam eden derlemeye henüz eklenmedi. SQL `after_commit`/`poll_commit` API'si beş metin sütunu ile bağlanır. Canlı bekleme/transaction/lock kabulü beklenir.

Yeni opt-in `native_sql_v1` aktarımı mevcut wire CRUD/makbuz yolundan ayrı tutulur. `native.open` sonrasında oturum tek yönlü native yaşam döngüsü kullanır. Parametreli komutlar doğrudan MySQL prepared protocol'e, parametresiz tek ifade doğrudan COM_QUERY'ye gider; gateway SQL sınıflandırması/marker rewrite yoktur ve MULTI_STATEMENTS kapalıdır. JSON metni, UNSIGNED BIGINT ve 65 basamak DECIMAL korunur. Büyük sonuçlar 16 MiB toplam sınır içinde SHA-256 doğrulanan 256 KiB parçalarla aktarılır; mevcut 1 MiB wire çerçevesi değiştirilmez.

Native aktarımda negatif TIME binary sign dönüşümü düzeltildi; Python'da temsil edilemeyen sıfır tarihler SQL NULL'a sessizce çevrilmez. Prosedür COMMIT edip sonra SIGNAL verebildiği için salt statement hatası önceki etkilerin geri alındığı şeklinde raporlanmaz. **11 aktarım sınır kontrolü PASS**; gerçek TLS `native_fork.py` kabulü yeni binary'yi bekliyor.

Python SDK'nın temiz checkout'unda `codex/ai-sql-hist-release` dalı açıldı. Ayrı `connect_native()`/`NativeConnection`, exact `NativeJson`, typed sonuç kümeleri, async/sync commit seçenekleri, yapılandırılmış hata alanları ve otomatik tekrar yapmayan native transport eklendi. **7 SDK unit PASS**; paketli tüketici/native TLS kabulü bekleniyor. SDK sürümü henüz değiştirilmedi. Diğer üç SDK'nın yeni aktarım tüketicileri henüz tamamlanmadı.
