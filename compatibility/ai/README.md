# Native AI katalog ve worker protokolü

Bu dizin üretim SQL katalog tanımlarını ve gerçek provider worker'ını içerir. Yeni motorun kabulü tamamlanmadan bir özelliğin capability değeri `true` yapılmaz. SQL katalog sınaması, native parser veya dış sağlayıcı kabulünün yerine geçmez.

## Kurulum ve yetki sınırı

Güvenilir şema yöneticisi önce `compatibility.jobs.compute_jobs.install_queue(session)` ile yalnız ortak iç kuyruğu kurar/yükseltir, sonra `catalog.sql` dosyasını uygular. CLI karşılığı mevcut `install --queue-only` komutudur; bu seçenek schedule/occurrence tabloları veya yeni public SQL JOB arayüzü kurmaz. Kuyrukta `job_kind`, `payload`, `attempt`, `events` alanları ve 576 karakterlik `owner` gerekir. Katalog tabloları InnoDB kullanır; kullanıcı tabloları ve mevcut kuyruk işleri silinmez.

Kaynak dağıtımının kökünde aşağıdaki komutlar çalıştırılır. `--password` veritabanı parolasını etkileşimli ister; API anahtarı değildir. TLS sertifikasındaki DNS adı `--host` ile eşleşmelidir. Her komut başarısız olursa kurulum durdurulur; `--force` kullanılmaz.

```powershell
$cli = 'C:\Program Files\StarDataParadise\CLI\bin\mysqlsh.exe'
$tls = @('--mysql', '--host=db.example.internal', '--port=3306',
    '--ssl-mode=VERIFY_IDENTITY', '--ssl-ca=C:\ProgramData\StarDataParadise\ca.pem')
& $cli @tls --user=deployment_admin --password --py `
    --file compatibility/jobs/compute_jobs.py install --queue-only
if ($LASTEXITCODE -ne 0) { throw 'AI queue installation failed.' }
& $cli @tls --user=deployment_admin --password --sql --file compatibility/ai/catalog.sql
if ($LASTEXITCODE -ne 0) { throw 'AI catalog installation failed.' }
```

Araç/onay profili kurulum sırası `catalog.sql` → `tool_catalog.sql` → `action_catalog.sql` olur. İlk kurulum ve yükseltme, açıkça **bakım profili `--sdp-ai-actions=OFF`** ile yapılır. Üç dosya başarıyla kurulduktan sonra sunucu temiz biçimde yeniden başlatılır: **`--sdp-ai-sql=ON --sdp-ai-actions=ON`**. Actions başlangıçta ayarlanan readonly global profildir; session AI seçeneğini değiştirmek ordinary CALL onay kontrolünü atlatmaz. Actions ON iken eksik veya değiştirilmiş policy helper kesin hata üretir; installer için fail-open istisnası yoktur.

Tam araç/onay profilinde, aynı bakım penceresinde yukarıdaki katalog kurulumundan sonra:

```powershell
& $cli @tls --user=deployment_admin --password --sql --file compatibility/ai/tool_catalog.sql
if ($LASTEXITCODE -ne 0) { throw 'AI tool catalog installation failed.' }
& $cli @tls --user=deployment_admin --password --sql --file compatibility/ai/action_catalog.sql
if ($LASTEXITCODE -ne 0) { throw 'AI action catalog installation failed.' }
```

Son iki modül native araç/rol/onay kabulüyle birlikte paketlenir; yalnız SQL storage fixture'ının geçmesi bu profili desteklenir yapmaz. Yeni fonksiyon/prosedürler eklenirse binary'nin sabit DD manifest'i ve SQL dosyaları aynı sürümden alınmalıdır. UI/API yetenekleri ilgili native kabul tamamlanana kadar açılmaz.

Rutinlerin explicit definer'ı locked `sdp_ai_internal` hesabıdır. Hesap yalnız sabit katalog tabloları, ortak kuyruk tablosu ve sabit helper rutinleri için gerekli per-object yetkileri alır; global/schema wildcard, routine DDL, FILE, role veya proxy yetkisi almaz. Kurulum mevcut aynı adlı hesabı sahiplenmez: managed ATTRIBUTE marker, locked durum ve beklenen grant listesi eşleşmezse ilk hatada durur. Mevcut hesapların parolası, lock durumu, attribute'ları veya grant'leri otomatik değiştirilmez; installer hata sonrası devam modunda çalıştırılmaz.

Uygulamaya yalnız gerekli prosedürlerin `EXECUTE` yetkisi verilir. Native AI giriş noktası `sdp_ai.dispatch`; transaction bağlantıları `before_commit`, `after_commit`, `rollback_turn`, senkron bekleme bağlantısı `poll_commit`, native Knowledge okuma bağlantısı `knowledge_document` prosedürleridir. Bu bağlantılar çağıranın oturumundan çalışır; uygulama hesabının kullandığı bağlantılar için `EXECUTE` yetkisi gerekir. İç yardımcılar DEFINER yetkisi altında çalışır. Uygulamalara `sdp_ai` veya `sdp_jobs` tablolarında doğrudan DML yetkisi verilmez. Çağıranın kimliği native `STARDATAPARADISE_ACTOR()` ile alınır; DEFINER hesabı çağıranın yetkisi sayılmaz. İsimlerdeki kaçışlar dahil bu değerin aynısı worker kimlik bilgisi bağlarında kullanılır.

Worker hesabının native actor değeri yönetici tarafından `sdp_ai.worker_accounts` tablosuna kaydedilir. Worker prosedürleri bu kaydı, autocommit durumunu ve açık kullanıcı transaction'ı bulunmadığını doğrular. Uygulama hesabı worker prosedürlerine erişse bile bu kayıt olmadan iş alamaz veya yayımlayamaz.

Temel profilin örnek per-procedure yetkileri aşağıdadır. Hesapların önceden güvenilir yönetici tarafından oluşturulduğu varsayılır; örnek mevcut hesapların parolasını veya rollerini değiştirmez. Knowledge/table SELECT ve iş tablolarının DML hakları ayrıca olağan MySQL yetkileriyle verilir.

```sql
GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO 'app_user'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.before_commit TO 'app_user'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.after_commit TO 'app_user'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.poll_commit TO 'app_user'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.rollback_turn TO 'app_user'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.knowledge_document TO 'app_user'@'%';
GRANT EXECUTE ON FUNCTION sdp_ai.knowledge_value TO 'app_user'@'%';

GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO 'ai_worker'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.renew_lease TO 'ai_worker'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.publish_run TO 'ai_worker'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.claim_model_import TO 'ai_worker'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.renew_model_import TO 'ai_worker'@'%';
GRANT EXECUTE ON PROCEDURE sdp_ai.publish_model_import TO 'ai_worker'@'%';
```

Worker hesabında `SELECT STARDATAPARADISE_ACTOR();` çalıştırılarak gerçek principal alınır. Deployment yöneticisi çıktıyı aynen `sdp_ai.worker_accounts(actor)` alanına kaydeder. `USER()` veya istemcinin host adıyla tahmini principal üretilmez. Yetki örneğindeki `ai_worker@%` ile bağlantıda dönen principal aynı olmayabilir; native actor gerçek authenticated account eşleşmesini kullanır.

## Worker yapılandırması ve başlatma

Workbench'teki maskeli **Kimlik bilgisi kaydet** işlemi yalnız mevcut Windows kullanıcısının Credential Manager kaydını oluşturur. **Worker bağını hazırla**, authenticated SQL owner için anahtarsız yapılandırma üretir. Ayrı makinedeki veya farklı Windows identity ile çalışan worker bu kasaya kendiliğinden erişemez; güvenilir worker yöneticisi ilgili kimliğe/config'e bağı kurmalıdır. UI'de kasa kaydı başarılı olduğunda provider erişimi doğrulanmış sayılmaz.

Örnek `C:\ProgramData\StarDataParadise\ai-worker.json`:

```json
{
  "credentials": {
    "app_user@%": {
      "openai_main": {
        "source": "windows",
        "target": "StarDataParadise/ai/openai_main",
        "providers": ["openai"]
      },
      "claude_main": {
        "source": "environment",
        "name": "SDP_CLAUDE_API_KEY",
        "providers": ["anthropic"]
      }
    }
  },
  "local_runtime": {
    "import_roots": {"app_user@%": "D:/StarDataParadise/import/app_user"},
    "model_store": "D:/StarDataParadise/models",
    "executable": "C:/Program Files/StarDataParadise/llama/llama-server.exe"
  }
}
```

`app_user@%` yerine uygulama bağlantısında dönen native actor aynen yazılır. Credential alias modeldeki `credential_ref` veya `secret_env` değeriyle eşleşir. Ortam değişkeninin gerçek değeri worker sürecinin güvenilir ortamından okunur; JSON dosyası API anahtarı içermez. Özel cloud endpoint gerekiyorsa ilgili trusted credential kaydına tam URL `endpoints` allowlist'i eklenir. Dosya ve import/model dizinlerinin erişimi worker yöneticisinin denetimindedir.

Worker ayrı bir TLS SQL oturumuyla çalışır; mysqlsh'nin mevcut bağlı session'ını kullanır. Kaynak dağıtımının kökünde, uygulamanın SQL editöründen bağımsız bir terminalde:

```powershell
$entry = @'
import sys
from pathlib import Path
sys.path.insert(0, str(Path.cwd()))
from compatibility.ai.worker import main
main(['--config', 'C:/ProgramData/StarDataParadise/ai-worker.json'])
'@
& $cli @tls --user=ai_worker --password --py --disable-plugins --execute $entry
```

Tek claim/işleme turu için `main([...,'--once'])` kullanılır. Sürekli modda Ctrl+C süreci durdurur; kaydedilmiş iş/receipt'ler veritabanında kalır. Worker TLS, autocommit, boş AI staging ve native dedicated-RPC guard koşullarını doğrular. Log yalnız iş/model kimliği ve durumu içerir; credential, transcript veya provider cevabı yazılmaz. Worker başlatılmadan queued AI işi provider'a gönderilmiş sayılmaz.

Katalog mutasyonları mevcut kullanıcı transaction'ına katılır. Transaction yoksa kısa bir transaction açar. Başarısız bir katalog mutasyonu kendi yazılarını geri alır. SQL prosedüründeki `__sdp_ai_dispatch_v1_46ac5bf2` savepoint adı katalog için ayrılmıştır; uygulama bu adı kullanmamalıdır. AI staging açıkken katalog mutasyonu için açık kullanıcı transaction'ı gerekir.

Native AI komutlarının argüman AST'si procedure hazırlığı, `fix_fields` ve değer değerlendirmesinden önce denetlenir. Parametre, kullanıcı değişkeni ve local değer okumaları kabul edilir; UDF/stored function, değişken yazımı, subquery ve desteklenmeyen yan etkiler reddedilir. Prepared parametre metni SQL kaynağı olarak yeniden yorumlanmaz. Doğrudan `CALL sdp_ai.dispatch(...)` uyumluluk yolu ordinary MySQL CALL argüman davranışına sahiptir; bu yola native argüman atomikliği garantisi atfedilmez. Katalog kendi kısa transaction'ını açmadan önce mevcut fiziksel yazıları ayrıca denetler.

## Konuşma ve kalıcı çalıştırma

`START` ve `APPEND` işlemleri oturuma ait staging verisi hazırlar. Bu veri güvenilir kabul edilmez: commit sırasında actor, nesne erişimi, revision, memory epoch, model sürümü, prompt türleri, bütçeler ve seçenekler yeniden doğrulanır. Native commit bağlantısı turn, run, ortak JOB kaydı ve outbox olayını kullanıcı SQL yazılarıyla aynı transaction'da yayımlar. Staging ancak başarılı commit sonrasında temizlenir. Rollback staging'i temizler.

`FINISH` ve aktif konuşma değişimi bütün staging bileşenleri ile commit edilmemiş fiziksel SQL yazılarını kontrol eder. Yanıt isteği, context ve turn seçenekleri sessizce atılmaz. Okuma yapan açık transaction tek başına engel sayılmaz. `APPEND RESPONSE` seçenekleri başarılı turn sonunda temizlenir.

32 hexadecimal karakterden oluşan `request_key`, aynı actor ve aynı AI turn içeriği için run oluşturmayı tekrar etmez. Aynı anahtarla farklı staging içeriği reddedilir. Bu koruma AI run kaydı içindir; uygulamanın genel iş SQL'lerini yeniden çalıştırmasını otomatik olarak tekilleştirmez.

Provider çağrısı sırasında SQL transaction'ı veya satır kilidi tutulmaz. `claim_run(worker_id, lease_seconds)` tek `document` sütununda sabit bir model/mesaj/seçenek snapshot'ı döndürür. `renew_lease` iptal bilgisini de döndürür. `publish_run` lease sahibini, süreyi, revision ve epoch değerlerini doğrular; typed response, konuşma mesajı, kuyruk durumu ve outbox olayını atomik yazar. Aynı publication hash'iyle tekrar deneme ikinci olay veya mesaj üretmez. Süresi dolan bir çalışan iş, belirsiz dış sonuçla başarısız olur; kendiliğinden yeniden sağlayıcıya gönderilmez.

Model tanımları `openai`, `anthropic` veya `local_gguf` provider'ını taşır. SQL yalnız `secret_env` veya `credential_ref` başvurusu içerir. API anahtarı SQL, transcript veya katalog belgesine yazılmaz. Windows kasa hedefi `StarDataParadise/ai/<credential_ref>`, blob kodlaması UTF-8'dir. Başvuru worker'ın güvenilir actor yapılandırmasına bağlıdır; kullanıcı başka bir ortam değişkenini key adı vererek okuyamaz.

Claude Messages bağlantısı `anthropic-version: 2026-10-21` ve `Authorization: Bearer` kullanır. Bu sürümde kaldırılan `temperature`, `top_p` ve `top_k` seçenekleri gönderilmez; model kaydı sampling yeteneği iddia etse de adapter bunları reddeder. `stop` seçeneği `stop_sequences` olarak aktarılır. Sözleşme 21 Kasım 2026'da resmi [API sürümleri](https://platform.claude.com/docs/en/api/versioning) ve [Messages başvurusu](https://platform.claude.com/docs/en/api/messages/create) ile doğrulandı. Bu kontrol gerçek anahtarlı harici çağrı kabulünün yerine geçmez.

Cloud credential bağı varsayılan olarak yalnız seçilen sağlayıcının resmi sabit endpoint'ine gönderilir. SQL'deki model endpoint'i tek başına anahtarı başka bir HTTPS servisine yönlendiremez. Özel proxy için trusted credential kaydına `endpoints: ["https://approved-proxy.example/v1/responses"]` tam URL allowlist'i eklenir; gerekirse `providers: ["openai"]` aynı anahtarı sağlayıcıya da bağlar. Bu ayarlar SQL model belgesinden alınmaz. HTTPS/TLS, query/userinfo/redirect reddi ayrıca uygulanır. GGUF runtime'ın sahip olunan loopback token yolu bu cloud credential kuralından ayrıdır.

`IMPORT MODEL` yolu worker'ın yapılandırılmış import köküne göredir. İstemci bilgisayarındaki dosya kendiliğinden sunucuya aktarılmaz. İçe aktarma ayrı `ai_import` işi üretir. `claim_model_import` kaynak ve beklenen SHA-256 snapshot'ını verir; `publish_model_import` eşleşen doğrulanmış artifact yayımlanana kadar modeli kullanılabilir yapmaz. Runtime başlatma ve GGUF bütünlük kontrolleri worker'da yapılır.

Response seçenekleri ve hata numaraları `contracts/sql/ai-catalog-v1.json` sözleşmesinden üretilmiştir. SQL ve worker, desteklenen JSON Schema 2020-12 alt kümesini doğrular. `$ref`, bilinmeyen anahtarlar ve desteklenmeyen seçenekler reddedilir. SQL validator, MySQL'in farklı taslaktaki genel schema doğrulayıcısına dayanmaz.

## Typed Stored Knowledge

Temel komut biçimi aşağıdaki gibidir. Örnekte `app.orders` kullanıcının mevcut tablosudur; komut bu tabloyu oluşturmaz.

```sql
CREATE KNOWLEDGE app.order_lookup(p_id BIGINT UNSIGNED NOT NULL)
RETURNS TABLE(order_id BIGINT UNSIGNED NOT NULL, total DECIMAL(18,2) NULL)
READ ONLY SQL SECURITY INVOKER AS
SELECT id,total FROM app.orders WHERE id=:p_id;

EXECUTE KNOWLEDGE app.order_lookup(42);
SELECT k.order_id,k.total FROM KNOWLEDGE app.order_lookup(42) AS k;
```

Parametreler kaynak metninde yer değiştirme ile birleştirilmez; native typed SP parametresi olarak bağlanır. Aynı typed tanım `ALTER KNOWLEDGE` ile yeni sürüm üretir. Native yürütme invoker'ın tablo SELECT yetkisini her çağrıda kontrol eder; Knowledge EXECUTE yetkisi tablo yetkisi sağlamaz. Saklanan tablo DD kimliği değişirse aynı SQL adının yeniden oluşturulması eski bağımlılık sayılmaz.

Knowledge parametre ve sonuç sütunları, `NULL` açıkça yazılmadığında non-null kabul edilir. SQL NULL geçirmek veya döndürmek için ilgili bildirime `NULL` ekleyin; örneğin `p JSON NULL` ve `RETURNS TABLE(value JSON NULL)`. JSON belgesinin `null` değeri SQL NULL'dan ayrıdır. Prepared çağrıda `CAST(? AS JSON)` JSON null/nesne/sayı/bool/metin/dizi türünü korur; `None`/`DBNull` gibi SQL NULL bağı yalnız nullable parametreye verilir.

Model, Knowledge, Behavior, Conversation ve Response Schema adları kendi AI kataloğunda saklanır; şema ve nesne adı büyük/küçük harf dahil kaydedilen biçimle eşleşmelidir. `fixture.model` ile `FiXtUrE.model` bu katalog için aynı ad değildir. SHOW/discovery çıktısındaki adları CREATE/DESCRIBE/GRANT işlemlerinde aynen kullanın. Native MySQL function/procedure hedefleri ise motorun DD kimliğini ve kendi case/accent kurallarını kullanır; action/onay ve araç snapshot'ları bu gerçek DD kimliğine bağlanır.

Knowledge bir readonly SELECT, CTE/derived alt sorguları, vet edilmiş builtin ifadeleri ve temel tabloları destekler. `SELECT INTO`, dosya yazımı, kilit/uyku işlevleri, stored function/UDF ve view'lar reddedilir. Sonuçlar declared native SQL türüne dönüştürülür; NOT NULL ve sütun sayısı doğrulanır. Bir yürütme en fazla 10.000 sonuç satırı gönderir. Bu sınır ve strict tür dönüşümü sessiz veri kaybına çevrilmez.

## Açık araç listesi ve action profili

Araç profili Knowledge, native function ve procedure hedeflerini ayrı türlerle kaydeder:

```sql
CREATE AI CONVERSATION app.support_chat USING MODEL app.assistant_model
WITH FUNCTIONS (KNOWLEDGE app.order_lookup, FUNCTION app.calculate_total, PROCEDURE app.siparis_iptal);
```

Liste dışında araç seçilmez. Bare `WITH FUNCTIONS` boş listedir ve tools capability'si olmayan bir modelle normal araçsız inference yapabilir. Dolu liste için modelin `tools` capability'si gerekir. Sağlayıcıya yalnız native snapshot'tan üretilen `tool_<id>` adı ve typed parametre şeması gider. Sağlayıcıdan SQL veya yürütülecek schema/routine adı alınmaz.

İlk routine profili `SQL SECURITY INVOKER`, IN parametreleri, vet edilmiş ifadeler ve bounded InnoDB kaynaklarını destekler. Function araçları readonly çalışır. Procedure yazıları ve receipt aynı yerel transaction'dadır; explicit COMMIT/ROLLBACK, implicit DDL, dynamic SQL, iç CALL, UDF, nontransactional ve geçici tablo yazıları reddedilir. DECIMAL araç argümanları exact ondalık metin olarak taşınır. Native MySQL tablo/yordam yetkileri, aktif rol snapshot'ı, açık allowlist, güncel Decision ve varsa onay birlikte doğrulanır. Worker'ın uygulama tablosunda yetkisi olması gerekmez.

Action biçimleri, önceden var olan routine ve native rol için şöyledir:

```sql
REQUIRE APPROVAL ON PROCEDURE app.siparis_iptal;
GRANT APPROVE ON PROCEDURE app.siparis_iptal TO ROLE order_approver;
PREPARE ACTION CALL app.siparis_iptal(42) RETURNING ID INTO @action;
APPROVE ACTION @action;
EXECUTE ACTION @action;
```

İşlem snapshot'ı descriptor/argüman, güncel policy/aktif yetkiler ve desteklenen bütün dependency tablolarının kaynak fingerprint'ini bağlar. Kaynak profili InnoDB, primary key ve desteklenen lossless kolon tipleri ister; FK/cascade kaynakları ilk profilde reddedilir. EXECUTE kendi REPEATABLE READ transaction'ında PK aralıklarını kilitleyip yeniden doğrular. İlgisiz bir dependency satırı değişimi de stale yapabilir. Ham kaynak satırları sağlayıcı transcript'ine eklenmez. Onay bekleyen model çağrısı `PREPARE ACTION FOR TOOL CALL '<call_id>' RETURNING ID INTO @action` ile aynı protected çağrıya bağlanır; uygulama/model keyfi bir hedef veya snapshot gönderemez.

## Doğrulama ve açık kapsam

```powershell
python -m compatibility.tests.ai_catalog_acceptance --routines-only
python -m compatibility.tests.ai_catalog_acceptance
python -m compatibility.tests.ai_catalog_security
python -m compatibility.tests.ai_keyword_native
python -m compatibility.tests.knowledge_native
python -m compatibility.tests.ai_context_native --full
python -m compatibility.tests.ai_commit_guard_native
python -m compatibility.tests.ai_routine_pin_native
python -m compatibility.tests.ai_tools_native
python -m compatibility.tests.ai_action_native
```

İlk komut eski motor üzerinde açıkça işaretlenmiş test kimliği/transaction shim'leriyle yalnız SQL sözleşmesini sınar. İkinci komut yeni derlenmiş native motoru ve transaction bağlantılarını kullanır. Fixture geçici veri dizini, ayrı TLS bağlantısı ve kendi hesaplarıyla çalışır; mevcut 3306 sunucusunu kullanmaz. Testler gerçek provider anahtarı kullanmaz. SQL fixture response publication, dış sağlayıcı smoke testi olarak raporlanmaz.

Immutable native12c üzerinde temel katalog/model import, senkron ve asenkron turn, typed TEXT/JSON capture, SQL/PLSQL yerel hedefleri, Knowledge derleyici/yürütme/tablo kaynağı, FULL JOIN ile iki yönlü prepared rebind, structured context, behavior sürüm bağlama ve conversation metadata kapıları geçti. Gerçek yerel GGUF yükleme/inference ve provider I/O boyunca business SQL kilidi tutulmaması da ayrı kurulu SDK kabulünde doğrulandı. View'lar Knowledge içinde açıkça reddedilir. OpenAI/Claude canlı anahtarlı smoke testi **NOT_RUN**; fixture publication veya yerel GGUF bu sağlayıcıların canlı kabulü değildir.

Immutable native15 üzerinde strict `ai_tools_native --sdk-python <kurulu-SDK-Python>` geçti: gerçek prepared Knowledge/function/procedure, exact Decimal/uint64, handled validator hatasından toparlanma, owner zaman dilimi, Decision SET/DENY, trigger owner/definer yetki ayrımı ve revoke, timeout rollback receipt ve allowlist sınırları doğrulandı. Gerçek sağlayıcı isteği publication fixture'ından gelen onay bekleyen araç çağrısı; PREPARE FOR TOOL CALL, ayrı aktif rol ile onay/ret, native yürütme, atomik action/tool receipt ve devam transcript'i boyunca sınandı. Kurulu Python SDK0.3.0, gerçek CPU-deadline hatasında public510005/native61085 ve committed/rolled_back/run/call alanlarını TLS üzerinden korudu. Bu test dış sağlayıcı çağırmaz.

`ai_action_native` ayrıca manual onay/ret/stale/role ve kaynak kilidi sınırlarını test eder; bu kapının sonucu ayrı kaydedilir. Binary ile katalog manifest'i birebir eşleşmelidir. `ai_routine_pin_native` aktif RPC boyunca transitive FUNCTION/PROCEDURE DD pinlerini ve çağrı dönüşünde bırakılmalarını immutable14 üzerinde doğruladı. Tam release kararı bütün gerekli kapıların matched binary ve SQL asset'leriyle kapanmasını gerektirir.
