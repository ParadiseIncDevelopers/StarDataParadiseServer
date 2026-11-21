# Stable release - 21 Kasım 2026

Bu kayıt son release kapılarını toplar. Önceki denemeler ve başarısızlıklar
[etap kaydında](AI_SQL_HIST_RELEASE.md) korunur; eski HIST/JOB planları güncel
kapsam değildir.

## Kapsam

Server 0.5.0, Workbench 0.3.0; Python 0.3.0, Node 0.2.0, Java 0.3.0,
StarDataParadiseDrive 2.1.0 ve .NET Framework sağlayıcısı 2.0.0 hazırlanır.
Yayınlar stable ve `prerelease: false` olur. Native motorun ayrı uyumluluk
sürümü `26.10.0-StarDataParadise`, Classic Workbench tabanı 8.0.47 kalır.

Kullanıcının son kararıyla yeni SQL HIST/Git ve public SQL JOB komutları,
ekranları ve schedule genişlemeleri ertelendi. Legacy compute/history API ve
kullanıcı verisi korunur. AI inference/import dahili dayanıklı kuyruğu kullanır;
bu, yeni public JOB özelliğinin yayımlandığı anlamına gelmez. Ertelenen kaynaklar
yalnız yerel WIP dalı ve hash doğrulanmış ignore edilen arşivlerde saklanır.

Workbench sekiz native sayfa içerir. Knowledge bağımlılıkları diyagram,
sürümleri timeline olur. Konuşma formundan sonra düzenlenebilir ayrı AI SQL
ekranı açılır. Parametre/sonuç başlıkları ` - ` ayıracını kullanır. Modal işlem
düğmeleri koyu petrol yeşili `#043133`, metin beyazdır; disabled/focus ve native
erişilebilirlik durumları korunur. Onay ekranı gözlenen action kimliğine bağlı
SQL önizlemesi üretir; hedef veya kimlik değişince önizleme geçersizleşir.

AI için OpenAI anahtarı, Claude/Anthropic anahtarı veya kullanıcının GGUF
modelini içe aktarması desteklenir. Anahtarlar release'e/log'a eklenmez. Canlı
OpenAI/Claude çağrıları **NOT_RUN**: gerçek anahtar sağlanmadı. Resmi API
sözleşmesi fixture'ları gerçek bulut kabulünün yerine sayılmaz.

## Tamamlanan kabul

- Native13: kısa SQL/PLSQL altı gerçek sunucu kabulü PASS; projection/SP/FOR,
  QUALIFY/BY NAME/pipeline, inspection/PS/DD, named INSERT/view/ODKU,
  ORDER/volatile/continuation ve extended FULL OUTER JOIN.
- Native12c: Knowledge + FULL OUTER JOIN autocommit, tekrar kullanılan prepared
  statement, unsigned metadata ve boş kaynak birleşimi PASS.
- Native14: tam tipli Knowledge/Behavior/schema edit roundtrip, üç model kayıt
  biçimi, Unicode native rol/yordam yetkileri ve restart/revoke PASS.
- Native14: raw binary/text/NULL prepared bağları, VARCHAR OUT'un binary girişe
  rağmen tanımlı metin türünü koruması ve sonraki prepared çağrı PASS. Güncel
  kurulu Python, packed Node, external-JAR Java ve .NET net8/net10/net48 TLS
  tüketicileri PASS.
- Native14: core catalog/UTC, response catalog, native RPC guard ve COMMIT/DD
  guard PASS. Provider beklerken business kilitlerinin bırakılması ve hata
  durumunda commit sonucunun korunması doğrulandı.
- Native15: tam araç + kurulu Python SDK kabulü PASS; typed araçlar, Decision
  SET/DENY, definer cache izolasyonu, owner revoke, worker identity/timezone
  restore, gerçek 61085 deadline hata zinciri, waiting approval/approve/deny,
  atomik tool/action receipt ve continuation dahil.
- Native15: Knowledge aynı oturumda handled hata sonrası tekrar CREATE,
  uint64/Decimal/UTF8/NULL, prepared rebind, metadata ve ACL kabulü PASS.
- Native15: kurulu Python SDK ve tam AI/actions başlangıç profilinde gerçek
  GGUF import/load/inference, sync TEXT/JSON capture, provider beklerken business
  COMMIT/kilit bırakma ve ASYNC capture reddinde atomik rollback PASS
  (`build15-local-full-profile.log`, exit0). İlk yeni fixture denemesi startup-only
  değişkeni session scope ile okuduğu için1238 verdi; sorgu global scope ile
  düzeltilip test bütünüyle tekrarlandı.
- Workbench backend 28 kontrol PASS; gerçek lexer/completion/hover/capability
  ve metadata 140 kontrol, splitter 12 kontrol ve failed-generator kapısı PASS.
- AI/provider/worker/transport son birim kontrolü: 72 PASS.
- Server yönetim uygulaması Release build ve help/version/invalid_option üç
  CTest PASS. Mevcut macro redefinition derleme uyarıları loglarda korunur.
- Dahili queue-only kurulum ve legacy compute regression gerçek sunucuda PASS.

Native16 tam Action kabulü **PASS, exit0** (`build16-actions.log`). Parametreli
JSON CAST'in ilk kez EXECUTE sırasında type resolution yapması gerçek bir
hata olarak ayrıştırıldı: ordinary native SELECT doğru JSON verirken yeni
komutta opaque/string oluşuyordu. Custom command `prepare()` artık saf AST'yi
değerleri çalıştırmadan hazırlıyor; CAST türü binary binding'den önce çözülür.
Testin JSON beklentileri gevşetilmedi. Tekrarlı prepared çağrıda JSON null,
nesne, SQL NULL, BLOB/boş BLOB, Decimal ve uint64 değerleri doğru korundu.

Tam Action testi ayrıca onay/ret/expiry/stale/replay, SIGNAL rollback, güncel
owner/approver ACL, SP local RETURNING, pending business transaction, RR tam-PK
phantom/UPDATE kilitleri ve concurrent DROP sıralamasını doğruladı. INSERT-only
fixture'a konservatif `FOR UPDATE` kaynak snapshot'ının gerektirdiği native
kilitleme yetkisi açıkça verildi; UPDATE revoke negatifinde1142, sıfır insert ve
başarılı receipt yokluğu ayrıca sınandı. Üretimde örtük grant yapılmaz.

Native rol kimliği rename işlemini3532 ile reddeder. Fixture bu native kuralı
atlatmak yerine reddin ve mevcut grant'in korunmasını doğrular; geçerli USER
rename sırasında pending action'ın stale olması ayrıca geçti. UTF8/host-case,
DROP/recreate, değiştirilmiş private helper, bakım actions-OFF ve AI syntax-OFF
profillerindeki onay denetimi de geçti. İlk eksik fixture ledger grant'i, typed
locking privilege ve role-rename beklentisi denemeleri ayrı loglarda korunur.

Knowledge girişinde aynı geç PS type resolution hatası ayrı native tanıyla
tekrarlandı ve17'de aynı somut komutun prepare aşamasında düzeltildi. Native17
tam Knowledge kabulü **PASS, exit0**
(`build17-knowledge-full-json.log`): JSON null/nesne/integer/bool/Unicode string/
array, kesin büyük integer, aynı prepared handle ile rebind ve SQL NULL ayrımı
geçti. PREPARE sırasında yazan yordam argümanı reddedildi; eski typed metadata,
uint64/Decimal, readonly AST, caller rollback, invoker ACL, nesne kimliği ve
cached validator kapıları da bütünüyle geçti. İlk yeni nullable testinde NULL
varsayılanı yanlış varsayılmıştı; Knowledge gramerinin NOT NULL varsayılanı
korunarak amaçlanan parametre ve iki sonuç sütununa açık NULL eklendi. İlk hata
`build17-knowledge-nullability-fixture.*` altında korunur; JSON/değer
beklentileri gevşetilmedi.

## Final18 binary ve paket kabulü

Son derleme exit0;94 kaynak dosyasının normalize SHA256 değerleri snapshot ile
eşleşti. Native17'ye göre yalnız kabul edilen capability metadata fonksiyonu
değişti; diğer93 dosya aynı kaldı. Birleşik yama temiz pinned upstream'e
uygulanıp aynı kaynak byte'larını üretti.

- Yama SHA256: `5e684401b41e36432c841edd2fc04c7dc79cae56efbbf85469c0459b4640ae7e`.
- `mysqld.exe` SHA256: `16dd394190591af66f39171d86227cf74ac3cfce96f01490389d3b1f9cb1f346`.
- Sekiz gerçek TLS başlangıç profili ve mysql/legacy oturumlarında16 capability
  çıktısı, incelenmiş C++ formatter ile birebir eşleşti.32 SESSION/GLOBAL
  değiştirme denemesi1238 ile reddedildi ve değerler değişmedi. HIST/jobs false.
- Tam Action ve Knowledge testleri bu son binary üzerinde tekrar PASS verdi.
- Taze Python/Node/Java ve paket DLL hash'i doğrulanan net8/net10/net48 native
  TLS tüketicileri PASS. Eski wire CRUD API'si de korundu.
- Tam araç/onay/Decision/ACL/timezone ve kurulu Python SDK61085 hata zinciri
  PASS. Gerçek GGUF import/load/inference ve sync capture/rollback tam profil
  testi PASS; hiçbir diagnostic skip kullanılmadı.
- Server portable ZIP'inin CRC ve bütün payload hash'leri doğrulandı; doğrudan
  arşivden çıkarılan dosyalarla fresh initialize, TLS, bakım kurulumu/restart,
  accepted capability, typed Knowledge ve kısa SQL fonksiyonu PASS.

Son testler `out/ai-native-check/build18-*`, `out/sdk-native/build18-*` ve
`out/release/server-archive-acceptance.*` altında exit0 ile kayıtlıdır. İlk arşiv
fixture'ı yanlış geçici kökte durduğu için sınır doğrulamasında durdu; sahip
olunan `out/compatibility-tests` köküne alınarak aynı ZIP bütünüyle tekrarlandı.

Workbench son arşivi2644 dosya/61.600.443 byte, SHA256
`afa79e7254e9c0310918efc79d76ca7e845bf86ec39318e4b9c93edfb79999cc`;
Server arşivi94 dosya/34.631.157 byte, SHA256
`b206d84afc4c80b039c297220082186968ddbb125f213b95ec36dee0b4e2d8ef`.
Kaynak ve paket içindeki AI kılavuzu, onay sözleşmesi, SDK manifesti ve release
metadata dosyaları hash olarak eşleşir.

### Canlı Workbench ve bağlantı sürekliliği

Gerçek UI kabulü ilk kez ROLE NONE → aktif role geçişini ve CALL tabanlı SHOW
sonuçlarını aynı ana bağlantıda birleştirdi. Önce test başlatıcısının eksik
TEMP/TMP ortamı, rol etkinleşmeden başlangıç şemasına erişme denemesi ve satır
döndürmeyen SET ROLE için yanlış GRT query metodu düzeltildi. Bu fixture
başarısızlıkları ve kontrollü karşılaştırmalar korunur; kullanıcı yetkileri
genişletilmedi ve sorgular hatadan sonra yeniden yürütülmedi.

Ardından gerçek istemci hatası yakalandı: SHOW KNOWLEDGES sonrası fiziksel
connection ID16→17 değişiyordu; yeni bağlantıda aktif rol NONE oluyordu.
CLI/TLS aynı SQL sırasını, varsayılan DB olmadan da aynı oturumda doğru
yürüttü. Workbench `exec_main_query`/`exec_management_query` ilk sonucu alıp
kalan CALL sonuçlarını ve final OK'yi tüketmiyordu; sonraki bağlantı doğrulaması
yeniden bağlanıyordu. Native C++ helper artık ilk scrollable/buffered sonucu
koruyup kalan sonuçları tamamen tüketir. Native SQL metni değiştirilmez,
otomatik tekrar eklenmez ve geç gelen SQL hatası çağırana iletilir.

Exact Connector/C++9.4.0 kaynaklarıyla lifetime kontrolü yapıldı. Gerçek GRT
regresyonu anaID16/auxID15 sabitken tekrarlı SHOW/DESCRIBE, rol/tablo yetkisi,
sonraki sorgudan sonra ilk sonucun okunması, uint64/Decimal/NULL/UTF8, boş
sonuç, iki-resultset CALL ve geç SIGNAL1644 kapılarını PASS verdi. Session
counter1, komutun ikinci kez çalıştırılmadığını doğrular. Kaynak/native C++
byte'ları eşleşti; yeni DLL SHA256
`8062fd7763e799cdbd71cf99778164f97957143abd3c50664662a8c10984c6d4`.

Güncel tam runtime ile sekiz sayfa, dört gerçek katalog, Knowledge diyagramı/
timeline, aktif ana SQL rolü, ana bağlantıda Action PREPARE/DESCRIBE ve üç
kimliğe bağlı SQL önizlemesi, ayrı AI SQL ekranı ve yeni SQL sekmesi PASS verdi.
Fixture business satırı değişmedi; provider I/O ve credential write yapılmadı.
Kanıtlar kardeş Workbench deposunda `out/call-result-completion/` altındadır.
Yenilenen arşivin içindeki runtime ile aynı canlı kapı **PASS, exit0** verdi:
kardeş Workbench `out/classic-provider-form-acceptance/live18-result.json` ve
`live18.exit`. ZIP CRC/payload hash kontrolü de PASS; son binary/modül dosyaları
yayımlanacak arşivle aynıdır.

Bağlı editörün CLI `--quit-when-done` kapanışı çalışma alanı açılmayan kontrol
koşusunda da bekliyordu. Test parent'ı yalnız tamamlanmış ve doğrulanmış JSON
sonucundan sonra kendi PID'sini kapatır; eksik/başarısız sonuç ve sonuç öncesi
çökme başarısız kalır. Normal GUI kapanışı doğrulandı diye raporlanmaz;
`*.lifecycle.json` gerçek exit/cleanup durumunu ayrı kaydeder.

## Kanıt yolları

Test çıktıları kaynak ağacına eklenmez; `out/` altında tutulur. Native14/15
logları `out/ai-native-check/build14-*`, `build15-*`; SDK çıktıları
`out/sdk-native/`; native SQL sonuçları `out/sql-short-native/`; Workbench
çıktıları kardeş depoda `out/classic-provider-form-acceptance/` altındadır.
Başarılı komutun `.exit` dosyası 0 olmalıdır. Native snapshot, temiz pinned
upstream'e uygulanabilirliği ve normalize kaynak byte'larının aynılığını denetler.

## Temizlik ve yayın

Kaynak altındaki yalnız yeniden üretilebilir `__pycache__`, `.pytest_cache`,
`.pyc` ve `.pyo` girdileri çözülmüş depo sınırları doğrulanarak temizlenir.
Kullanıcı ayarları, tasarım görselleri, build bağımlılıkları ve ertelenen WIP
korunur. Release arşivleri kullanıcı profili/credential/cache/debug çıktısı
içermez; dosya SHA256 ve ZIP CRC doğrulaması yapılır.

Üç temizlik turunda12+3+2 cache dizini temizlendi; son salt okunur envanter0
girdi buldu. Üç turun manifestleri `out/release/` altında korunur;
`source-cache-final.exit`0'dır.

Birleşik native yamanın yerine veya üstüne uygulanmaması gereken14 untracked
ara geliştirme yaması kaynak klasöründen çıkarıldı. Önce tamamı ignore edilen
`out/future-wip/ai-sql-intermediate-20261121.zip` içine özgün byte'larıyla
arşivlendi; CRC ve her dosyanın SHA256 değeri doğrulandı. Arşiv SHA256:
`a38c552c633fcb02ab895dd02a0727a35b6acebc625fa59957a69b4da975d5db`.
Tracked veya tanınmayan dosya silinmedi. Birleşik yama manifest'inin byte
hash'ini korumak için bu dosya Git'te satır sonu dönüşümünden muaf tutulur.

SDK paketleri kontrol edilen kaynak commit'iyle eşleşir. Commit hash'i taşıyan
NuGet paketleri temiz SDK commit'inden sonra yeniden üretilir ve ayrı kurulu
tüketicide test edilir. Commit/push/tag ve GitHub stable yayınları tamamlandıktan
sonra MySQL installation wizard yalnız inceleme için açılır; ürün/servis/veri
güncellemesi otomatik uygulanmaz.

## Kararlı yayın hedefleri

Altı origin/master ref'i fresh fetch ile kontrol edildi; release commit'lerinin
atasıdır. Hedef etiketlerin yerel/uzak mevcut yayını yoktur; geçmiş etiketler ve
yerel HIST WIP ref'i değiştirilmez. Dört SDK temiz kaynak commit'inden üretildi:
Python `2bccf1306e401955304f5bd5f9c415250c6a55a4`, Node
`c49c7c53ca938bfdc32c4f503962828d9f8aa6c7`, Java
`280ab197ec099db1838b04a6fd86d92dc88e42d6`, .NET
`979b3f9905be3e08a6fac5d85198dd97aa180302`.

| Depo | Stable etiket |
| --- | --- |
| Server | [v0.5.0](https://github.com/ParadiseIncDevelopers/StarDataParadiseServer/releases/tag/v0.5.0) |
| Workbench | [v0.3.0](https://github.com/ParadiseIncDevelopers/StarDataParadise/releases/tag/v0.3.0) |
| Python | [v0.3.0](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Python/releases/tag/v0.3.0) |
| Node | [v0.2.0](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Node/releases/tag/v0.2.0) |
| Java | [v0.3.0](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Java/releases/tag/v0.3.0) |
| .NET / Framework | [v2.1.0](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.NET/releases/tag/v2.1.0), paketler2.1.0/2.0.0 |

Yayın aracı temiz source HEAD/tag, exit0 kapıları ve dosya SHA256 değerleri
eşleşmeden çalışmaz. Asset'ler önce draft'a yüklenir; sunucunun döndürdüğü
SHA256 doğrulandıktan sonra `draft=false`, `prerelease=false` ve latest durumu
tekrar okunur. Son API/Git teyitleri ignore edilen `out/release/` kaydında kalır.
