# SQL / PLSQL ve editör entegrasyonu — 20 Kasım 2026

Bu kayıt mevcut kullanıcı isteğinin üç özellik ailesinden kısa SQL/PLSQL ve
Workbench dil desteğini kapsar. Sürüm artırımı, commit, push veya release
yapılmadı. Native kabul geçmeden bu çalışma release desteği olarak sunulmaz.

## Native kaynak

Canonical çalışma ağacı: `out/mysql-migration/server-upstream-baseline`.
Teslim: `compatibility/upstream/sql-short-native.patch`, 27 native dosya,
LF satır sonları; eski `sql-short-next.patch` tek patch içinde birleştirildi.
AI/Knowledge/HIST ve VERSION() değişiklikleri ayrı ailelerin sahipliğinde korundu.
Özel Classic errno 63001–63005 sabit tutuldu; 10 native hata mesajı ve patch'teki
karşılıkları 630001–630005 public prefixleriyle senkronlandı. Yerleşik 1054,
1052 ve 1060 gibi hatalar değiştirilmedi.

Gerçek parser/binder/SP/optimizer yollarında EXCLUDE/REPLACE, LET, WHEN DO,
AS RETURN, AS...END, :local/:row.field, FOR sorgu imleci, SHOW EXPANDED,
QUALIFY, INSERT BY NAME ve FROM pipeline AST desteği yazıldı. Gateway,
regex derleyici, COMPACT keyword veya mod eklenmedi. Ayrıntılı davranış ve
sınırlar `compatibility/upstream/sql-short-native.md` içindedir.

## Workbench

- AS...END gövdesini kaynak byte aralıkları korunarak tek komut gönderen
  gerçek statement splitter genişletildi; nested kontrol, CASE, string/comment,
  Unicode, AS RETURN ve mevcut DELIMITER davranışları kapsanır.
- Gerçek Scintilla keyword seti ve keyword_list80 metadata'sı güncellendi.
- Gerçek lexer token akışından AI/Knowledge, HIST ve kısa SQL/PLSQL bağlam
  önerileri üretilir. EXCLUDE/REPLACE mevcut kolon cache'ini kullanır.
- AI nesne adları native SHOW listelerinin ACL filtreli sonuçlarından, son 100
  HIST revision kimliği native HIST LOG'dan schema metadata yenilemesinde
  cache'lenir. Cache her schema yenilemesinde yeniden kurulur; eski metadata
  cache'leri gibi yeni nesne/yetki değişiklikleri yenileme gerektirir.
- Capability sözleşmesi: `native_language.ai_sql`, `sql_short`, `hist`;
  bilinmeyen/eski sunucuda tüm yeni tamamlama ve doğrulama kapıları kapalıdır.
- Eski ANTLR grammar'ının bu doğrulanmış native aileler için yanlış işaretleri
  bastırılır; yürütme doğrulaması sunucuya aittir. Gerçek lexer quote/comment
  hataları ve normal SQL hataları korunur.
- Grammar üretiminin PATH üzerinden Java bulamayıp eski generated C++ ile
  başarılı görünmesi düzeltildi: çözümlenen Java executable'ı doğrudan batch'e
  aktarılır, başarısız üretim native build'i başarısız yapar.

## Doğrulama ve sorun kaydı

- Combined native Bison `%expect 37`: PASS. Tek başına grammar kabulü runtime
  kabulü değildir.
- Native helper ve etkilenen resolver/runtime/optimizer kaynaklarında MSVC
  `/Zs`: önceki dilimlerde PASS; Knowledge/HIST ile son tam build root'a aittir.
- Konsolide patch'in izole feature-free snapshot'a temiz uygulanması ve bütün
  entegre dosyaları birebir yeniden üretmesi: PASS.
- `packaging/classic-workbench/tests/sdp_language_native_acceptance.py`:
  gerçek splitter 12 script PASS; gerçek parser DLL lexer/completion 47 kontrol
  PASS. Grammar üretim hatasının nonzero dönmesi de PASS. Gerçek Java grammar
  yeniden üretimi sonrası aynı 47 kontrol tekrar PASS. Normal EXCLUDE/QUALIFY
  kolon adlarının veya uzun yordam AS alias'larının yanlış biçimde native aile
  sayılıp gerçek sözdizimi hatalarını gizlememesi de kapsanır.
- Workbench parsers, forms, wbpublic, wbprivate, db.sdp.parser.grt native
  targetları izole `out/classic-language-native-acceptance` çıktısında PASS.
  Son grammar gerçek üretimi sonrası parser/module tekrar PASS.
- Önceki `out/classic-sdp-names-ui` wbpublic link denemesinde LNK1114 / Windows
  hata 5 görüldü. İzole doğru runtime seed'li output üzerinden aşıldı.
  Yanlış eski seed'li ilk izole denemede sdp.canvas.lib / base::translate_ui
  bağımlılık eksikleri görüldü; mevcut doğrulanmış seed ile düzeldi.
- `compatibility.tests.sql_short_native_acceptance` ve
  `compatibility.tests.sql_short_relational_acceptance` import/CLI: PASS.
  Yeni initialize edilebilir mysqld bekleniyor; canlı native fixture sonucu
  henüz kaydedilmedi. Mevcut üçüncü binary VERSION() regresyonu root tarafından
  onarıldı; beşinci combined native build root sahipliğindedir. Dördüncü build
  HIST yetki düzeltmesi için root tarafından durduruldu.
- Backend hizmetindeki server-authoritative doğrulama için yeni C++ suite
  vakaları yazıldı; tam Workbench test suite henüz çalıştırılmadı.
- Root onayıyla build3 mysqld/DLL dosyaları `out/sql-short-native/build3-runtime`
  içine kopyalandı. Datadir eski çalışan spike ile initialize edilip build3
  kopyasıyla ayrı portta TLS açıldı; bfast binary kilitlenmedi. Projection,
  metadata, ACL, PS, DDL; AS RETURN/WHEN; LET scalar/DECIMAL/string/NULL ve
  nested shadowing ilk kesitleri geçti. Testteki FIRST_VALUE reserved adı
  first_local/new_qty olarak düzeltildi. Sonra bilinen eski parent-after-child
  LET slot hatası ERROR1406 ile doğrulandı; build5'teki düzeltme hâlâ gerçek
  kabulü bekler. Ayrı küçük smoke'ta FOR typed row, ITERATE/LEAVE, boş cursor,
  bound SQL/output metadata ve routine compiler IR geçti; temiz shutdown PASS.
  Bu sonuçlar fresh init veya yeni QUALIFY/BY NAME/pipeline/son fix kabulü değildir.

## Gerçek native kabul — build6 ve build7

- `out/hist-build6-runtime/mysqld.exe` ve DLL'ler immutable kopyadır;
  fixture'lar aynı yeni binary ile fresh initialize yaptı. `bfast` executable
  kilitlenmedi; her fixture ayrı datadir/port ve TLS kullandı.
- `compatibility.tests.sql_short_native_acceptance`: build6 PASS ve temiz
  shutdown. Projection/ACL/metadata/PS/DDL, typed LET, scope/scalar/kontrol,
  FOR typed row/cleanup/error ve native compiler expansion kabul edildi.
  SHOW EXPANDED artık kaynak metnini de döndürdüğü için testte EXCLUDE kontrolü
  yalnız `bound_sql` satırına daraltıldı; kaynak metni yanlışlıkla reddedilmez.
- `compatibility.tests.sql_short_inspection_acceptance`: build6 PASS;
  gerçek TLS COM_STMT_PREPARE/EXECUTE üç kolon metadata, parametre gizliliği,
  AST byte kaynak aralıkları, execution yapılmaması, ilk execution öncesi ve
  execution'lar arası DDL reprepare, SHOW_ROUTINE ACL, aynı datadir üzerinde
  yeniden başlatma sonrası DD language version ve routine source-map kalıcılığı.
- Relational fixture build6: QUALIFY gerçek plan/sıra/alias/PS ve BY NAME
  defaults/generated/function defaults/ACL/rollback PASS. Pipeline'ın eski
  null column-list constructor crash'i giderilmişti; bu kez GROUP BY projection
  adının boş gelmesi son ORDER'da1054 verdi. SHOW EXPANDED `AS `` ` çıktısı
  kök nedeni doğruladı: `EMPTY_CSTR` explicit boş alias gibi davranıyordu.
  Alias `{nullptr,0}` yapılarak native kendiliğinden alan adı korundu.
- `compatibility.tests.sql_short_relational_acceptance`: immutable
  `out/ai-native-check/build7-runtime/mysqld.exe` ile tamamı PASS ve temiz
  shutdown. QUALIFY WINDOW→filter→DISTINCT→sort/LIMIT planı; BY NAME;
  pipeline EXTEND/AGGREGATE/JOIN/ORDER/LIMIT, volatile değerlerin tek
  değerlendirilmesi, prepared parametreler ve canonical view SQL reparse geçti.

## Sonraki bağımsız native dilim — henüz runtime kabulü değil

- `sql-short-by-name-next.patch`: single-base updatable/nested view ve
  explicit ON DUPLICATE KEY UPDATE normal native insert/update yollarına
  bağlanır. Multi-table/JSON duality view için tahmin ederek leaf seçilmez.
  Bison37 ve sql_insert/generated parser `/Zs` PASS; ayrı edge fixture hazır.
- `sql-short-order-next.patch`: gerçek native ROW_NUMBER ile ordinal
  materialize edilir, order-preserving stage'ler bunu ayrı compiler alanıyla
  taşır ve açık ORDER uygular. Final projeksiyon ordinali kaldırır;
  DISTINCT/AGGREGATE/JOIN bu sıralama özelliğini düşürür. Native ORDER BY
  pozisyonları alanlara bağlanır. Star binder compiler alanını kaynak kimliğiyle
  gizler; aynı adlı sağ JOIN alanı silinmez. İki native TU `/Zs` PASS;
  `sql_short_order_acceptance` runtime fixture'ı hazır, yeni build bekler.
- `ai-local-capture-next.patch`: local capture marker'ı yalnız native SP
  parsing context'ten index/type/name alır; katalog JSON'unda kind/name bulunur.
  Pending kayıtlar frame'e aittir, global raw frame pointer map yoktur.
  JSON gerçek Field_json'a, TEXT native character field'a yazılır. Pending
  val/send/save guard'ları ve expired scope precommit kontrolü vardır.
  AI statement'lar native SP instruction yoluna alınır; FETCH cursor grammar
  factoring mevcut NEXT/FROM boş-prefix conflict'ini kaldırdığı için birleşik
  Bison beklenen conflict sayısı37'den36'ya iner. Beş TU `/Zs` PASS.
  `ai_local_capture_native` supplied-response runtime fixture'ı hazırdır;
  provider inference yaptığı iddia edilmez. Yeni binary kabulü henüz yoktur.
  Request capture hem `INTO :target AS JSON` hem `AS JSON INTO :target`
  sırasını kabul eder. APPEND/FINISH/CANCEL normal SQL identifier olarak kalır;
  statement başlatabildikleri için SP label adında quote gerektirirler.
- AI capture build7 hard hang'i shared format `Item*` iki JSON parent'a
  itemize edilmesiyle native intrusive free list cycle oluşturuyordu. Root
  doğrulanmış owned process'ten StackWalk64 ile `cleanup_items` döngüsünü
  kanıtladı; `Item::do_itemize` her parser node'u `THD::add_item` listesine ekler.
  Redundant capture.format AST çifti kaldırıldı; katalog check_capture formatı
  zaten normalize eder. Local overlay ve before base aynı fix'e eşitlendi;
  ikinci parent'ta aynı Item tekrar kullanılmaz. Yeni binary kanıtı beklenir.
- Üç next patch güncel canonical source'a clean `git apply --check` PASS.
  Canonical büyük overlay apply/build slotu root'a aittir. Native Knowledge
  collector/cast düzeltmeleri hunk merge ile korunmalıdır.
- Workbench yeni language runtime
  `out/classic-language-next-native-acceptance`: parsers/forms build PASS;
  gerçek splitter12 script ve actual parser DLL lexer/completion57 kontrol
  PASS, grammar Java failure nonzero PASS. JSON/TEXT response formatı, format
  öncesi/sonrası capture, METADATA, LOCAL_GGUF provider ve HIST REMOTE endpoint /
  SYNC AUTO interval/conflict devamları gerçek öneri listesine eklendi. Var olan
  GUI/runtime değiştirilmedi. Yeni HIST grammar'ın kendi native kabulü ayrı
  gate'tir; editör öneri testi tek başına server implementation kabulü değildir.

## Açık kabul ve kapsam

ORDER pipeline'ın sonu veya hemen LIMIT öncesi olmalıdır; diğer devamlar
63004 ile reddedilir. BY NAME ilk dilimi base table/query insertion içindir;
view hedefi ve açık ODKU halen release binary'de reddedilir; sonraki patch bunları
uygular, fakat henüz derlenip kabul edilmemiştir. SHOW EXPANDED binary
prepared-result metadata/kaynak eşlemesi/durable language-version kabulü
build6'da tamamlandı. ORDER devamı, local capture, FULL OUTER, Knowledge table
source/context akışı ve ileri hover/local completion tüm hedef tamamlandı diye
sunulamaz. FULL_OUTER enum'u mevcut olsa da hash iterator ve cost model'de
assert/not implemented yolları vardır; native destek varsayılmamalıdır.

## Build9 hazırlığı ve FULL izolasyonu — 21 Kasım

- Üç NEXT patch canonical native source'a hunk merge ile uygulandı. Combined
  Bison36 ve dokuz native TU/parser `/Zs` PASS. View/ODKU, ORDER devamı ve
  local capture için gerçek runtime fixture'ları build9 immutable binary'sini
  bekler; önceki build6/7 sonuçları bu yeni davranışların kabulü sayılmaz.
- Native worker RPC guard prepare ve her execute için kendi kısa MDL pinlerini
  alır; prepared handle ve provider beklemesi boyunca pin tutmaz. `sdp_ai` /
  `sdp_hist_remote` exact manifest ve trusted nested worker scope korunur;
  readonly arg kontrolü native AST ile yapılır. `sdp_ai_worker_rpc_guard` yalnız
  session ON opt-in, SQL OFF/DEFAULT kapatması reddedilir ve ACL yükseltmez.
  sql_call.cc ve sys_vars.cc `/Zs` PASS. AI agent gerçek RPC kabulünü sahiplenir.
  Blocked/impure AI error eşlemesinde iki yanlış kod build9'da bilinen kusurdur;
  doğru 61081/510001 ve 61006/502006 düzeltmesi ayrı build10 patch'idir.
- `native-hist-call-origin.patch` bağımsız native HIST marker'ını ve readonly
  routine-name getter'ını sql_call.h/.cc'ye ekler. HIST helper graph guard'ı
  prepare/execute local RAII'dir; ordinary session ON şartına bağlı değildir.
  Birleşik header/sql_call.cc `/Zs` PASS; canonical build9 freeze korunur.
  Eski `native-call-readonly-target.patch` bununla birlikte uygulanmaz.
- `native-full-outer-next.patch` canonical kaynakları değiştirmeyen ayrı
  izolasyondur. Bison36 ve sekiz native TU/parser `/Zs` PASS; clean apply check
  PASS. Gerçek parser/binder/AccessPath bağlantısı, çift NULL complement,
  post-WHERE/sort/LIMIT ve canonical view printer içerir. FULL identifier/alias
  anlamı korunur; yalnız explicit FULL OUTER JOIN compound token üretilir.
  Native `HashJoinChunk` dosyaları fiziksel sağ satır match flag'lerini korur;
  kaynaklar bir kez, her ON adayı bir kez değerlendirilir. BLOB/JSON native
  pack_rows kullanır, read/write buffer ayrıdır ve non-BLOB kapasitesi ayrılır.
  Driver'ın derlenmiş 689 graph/NULL/duplicate/once/error/reinit testi PASS;
  bu sonuç native SQL execution kabulü değildir. Yeni
  `full_outer_native_acceptance` fixture'ı integrated binary bekler.
- FULL ilk dilimi iki base table veya gerçek typed Knowledge table-function
  operandı içindir. Arbitrary view/CTE/derived/lateral/ek JOIN, grouping,
  DISTINCT/window, locking read ve legacy EXPLAIN şekilleri 1235 ile reddedilir.
  TREE / iterator JSON açıklaması ayrı gerçek FULL operatoru gösterir. Trusted
  Knowledge predicate API'si AI agent'a aittir; yalnız syntax declaration
  kullanılan izole stub patch'e dahil değildir. Knowledge runtime kabulü gerçek
  materializer entegrasyonundan sonra yapılacaktır. Yeni iterator yolu
  `sql/iterators/sdp_full_outer_iterator.h` snapshot export'a dahil edilmelidir.

## Build9 gerçek kabul ve dar build10 düzeltmeleri

Immutable build9 SHA256:
`C269DE2144938ED2EB967970FA1C7963D914CA3D3AAAA96586895226957DE908`.
Her fixture aynı binary ile fresh init, ayrı TLS datadir/port ve temiz shutdown
kullandı. Native projection/SP/FOR suite, text/binary inspection + iki DDL
reprepare + DD restart persistence suite ve QUALIFY/BY NAME/pipeline relational
suite tekrar tamamı PASS.

- Yeni view/ODKU suite PASS: aliased/nested single-base views, CHECK OPTION,
  source aliases, explicit insert/update, prepared/UNION, native triggers,
  rollback ve column ACL. İlk fixture'da SELECT(amount) eksikti. Hem sıradan
  INSERT ODKU hem BY NAME VALUES(amount) aynı1143 ile reddedildiği doğrulandı;
  gerekli dar grant sonrası başarı, diğer caption sütunu için red korundu.
  Native ACL gevşetilmedi.
- ORDER continuation suite'in ilk on bir sonuç/schema karşılaştırması geçti.
  Volatile sort3587 verdi: compiler ordinal ROW_NUMBER için RANGE frame native
  determinism sınırına takılıyordu. ROW_NUMBER frame bağımsızdır; compiler frame
  ROWS'a değiştirildi. Canonical + NEXT patch + ayrı frame fix patch eşitlendi.
  Sonraki prepared/view/collision kabulü ve bu fix'in runtime kanıtı build10'u
  bekler; suite PASS diye raporlanmaz.
- Local capture production fixture native catalog identity61081 ile durdu.
  Gerçek `has_pending_turn()` INVOKER iken manifest tüm function'ları DEFINER
  varsayıyordu. AI agent exact per-routine security manifestini düzeltir.
  Yalnız disposable tanı run'ında fonksiyon security'si eski build9 manifestine
  eşitlendi; bu run üretim catalog kabulü değildir.
- Bu diagnostic native bridge'e ulaştı ve readonly SET fallback hatasını buldu:
  sp_instr_set ilk61025'ten sonra pending slotu NULL'a resetlemeye çalışıyor,
  ikinci red generic ME_FATALERROR işaretiyle CONTINUE55000 handler'ını
  atlıyordu. Pending capture / readonly FOR slotlarında fallback atlanır;
  reddedilen yazı ve slot state'i korunur. Dar sp_instr.cc fix canonicale
  uygulandı, `/Zs` PASS. Gerçek local suite tekrar build10'da koşulacaktır.
- Build10'a yalnız RPC ABI, ROWS frame, sp_instr fallback ve AI exact security
  manifesti alınır. FULL/origin/Knowledge/branch büyük overlay'leri bu build'e
  uygulanmadı. FULL resolver/optimizer syntax artık AI agent'ın gerçek native
  Knowledge Table_function/materializer header'larıyla da `/Zs` PASS; önceki
  isolated declaration stub üretim patch'e hiçbir zaman dahil edilmedi.

## Build10 bulguları ve dar build11

- Build10 production catalog ile native local JSON/metadata publication gerçek
  OBJECT tipini, unsigned64 hassasiyetini ve Türkçe Unicode'u korudu. Dört
  pending guard yerine3 görüldü. Ayrı SQL ara sayaçları send=1,copy=1,insert=2,
  write=3 kanıtladı: standalone SET expression hazırlığı sp_prepare_func_item
  içinde root Item_splocal'ı underlying Field'a unwrap ederek bütün value/save
  override'larını atlıyordu. Unwrap öncesinde yalnız pending splocal için
  native read guard eklendi; sp.cc `/Zs` PASS. Test artık dört ayrı ara sayacı
  assert eder. Üretim local suite henüz PASS değildir.
- ROWS frame ile ORDER3587 kalktı, sonuç sırası doğru kaldı. Volatile sort
  ifadesi hâlâ4 yerine8 kez değerlendirilmişti. İkinci düzeltme her sort key'ini
  ayrı materialized native scalar stage'de bir kez hesaplar, ROW_NUMBER ORDER
  yalnız bu typed field'lara bağlanır. Compiler key adları bind sırasında
  collision kontrolünden geçer ve sonraki projection'dan çıkarılır; kullanıcı
  positional schema'sına eklenmez. sdp_pipeline.h `/Zs` ve Bison36 PASS;
  canonicale alındı, büyük FULL/QB/branch overlay'leri uygulanmadı. Tek ve çoklu
  volatile key, PS/view/collision runtime kabulü build11'i bekler.
- Workbench typed local runtime
  `out/classic-language-typed-native-acceptance`: parsers ve wbpublic native
  build PASS; gerçek DLL75 lexer/completion/hover kontrolü ve splitter12 PASS.
  Parameter/LET/DECLARE isimleri, inner scope shadow, FOR projection alias ve
  cached gerçek source columns, explicit type/precision ve Unicode hover gerçek
  editor dwell event'ine bağlıdır. Inferred server type uydurulmaz. DOT_IDENTIFIER
  lexer'ın split identifier token'ında byte index artarken column artmıyordu;
  gerçek token konumu düzeltildi, qualified field completion ve hover geçer.
  Knowledge object call signature cache'i bu kabulün parçası değildir.

## Build11 kabul ve güncel AI+SQL release kapsamı

- Immutable build11 SHA256
  `D885DC19BB10D6AC24205E1C59707593CA344958C27B21974D9D6CF986972DC4` ile
  gerçek ORDER continuation suite ve üretim catalog local-capture suite tamamı
  PASS; eski SQL/SP/FOR/projection suite de tekrar PASS. Tüm fixture'lar fresh
  init/TLS/ayrı datadir ve temiz shutdown kullandı. ORDER'da tek ve çoklu sort
  key'leri tam bir kez değerlendirilir; PS/view/collision kontrolleri geçti.
  Local send/copy/insert/write sayaçları1/2/3/4, JSONuint64/Unicode/TEXT,
  expired/ASYNC/type/format precommit kontrolleri ve cursor regressions geçti.
- Kullanıcı SQL git/HIST ve yeni public SQL JOB sistemini sonraya aldı. Native
  HIST hook/grammar/sysvar/CMake kaynakları adaydan çıkarıldı; FULL/Knowledge
  context ve bütün SQL/PLSQL iyileştirmeleri korunur. Native JOB grammar yoktur;
  AI'nin özel durable queue/worker'ı public SQL JOB özelliği değildir.
- Engine HIST bağlama WIP snapshot'ı
  `out/future-wip/engine-hist-20261121T103818/sha256.json` ile korundu. Modal
  agent core HIST için ayrı archive üretti. Workbench eski HIST editör yüzeyi
  `out/future-wip/hist-job-editor-20261121T105004/sha256.json` ile korundu.
- AI-only native worker RPC artık remote/HIST manifesti içermez. Native COMMIT
  producer prepare kadar execute'da da exact catalog pin alır. Borrowed scope
  fiziksel COMMIT boyunca korunur, provider polling öncesi bırakılır ve stored
  dispatcher devamı öncesi yeniden doğrulanır. Her hook'un ayrı kısa pin'i de
  korunur. Bu güvenlik diliminin gerçek kabulü yeni binary'yi bekler.
- AI+SQL/FULL+gerçek Knowledge Table_function/context ve HIST temizliği birlikte
  Bison36 +11 native TU/generatedparser `/Zs` PASS. Generated parser'ın ilk
  kontrolünde yalnız output header include-path eksikti; düzeltildi ve geçti.
  Canonical snapshot/build slotu root'a devredildi. FULL native runtime kabulü
  henüz yapılmadı; source/driver syntax kabulü SQL feature PASS değildir.
- AI+SQL-only Workbench kabulü gerçek parser DLL üzerinde 86 kontrol ve 12
  statement splitter senaryosuna genişletildi. Son yedi regresyon UNION ilk
  operand çıktı adlarını ve CTE shadow/derived/pipeline için yanlış physical
  source alanlarının önerilmemesini doğrular. Parser hedefi yeniden PASS;
  forms/wbpublic/wbprivate önceki aynı kapsam build'inde PASS. İlk build girişimi
  sandbox içinde `subst` sürücü alias oluşturamadı; aynı script yalnız kendi boş
  alias'ı için yükseltilmiş çalıştırılarak başarılı oldu. Server build alias'ına
  dokunulmadı. Loglar Workbench `out/sdp-language-native-tests/` altında
  `build-for-projection-editor.log` ve `result-for-projection-editor.log`.
- Sonraki 13 dilimi için native `TO ROLE` / `FROM ROLE` ON-object GRANT/REVOKE
  alıcı faktörlemesi izole `out/role-grant-next/` altında hazır: Bison36 ve
  authorization/lexer/generated parser `/Zs` PASS. LEX marker/reset ile mevcut
  grant mutation ACL WRITE lock'ı altında tüm alıcıların native authID varlığı
  ilk yazmadan önce doğrulanır; ordinary TO user ve ROLE identifier yolu korunur.
  `role_grant_native_acceptance.py` fixture'ı AST PASS, runtime henüz NOT_RUN.
  Custom AI object grant intercept ve ordinary CALL approval hook aynı isolated
  dilimde derleniyor; canonical 12 freeze korunur.
- İncelemede `is_role_id` fonksiyonunun CREATE ROLE kökenini göstermediği
  bulundu: ACL_USER.is_role yalnız mevcut role-graph incoming edge üyeliğidir;
  taze ve henüz atanmamış CREATE ROLE için false döner. İlk izole type-check bu
  geçerli akışı yanlış reddedecekti. Native MySQL birleşik account/role authID
  modeli korunarak exact `find_acl_user` varlık kontrolüne geçildi. Custom AI
  grant principal'ları ayrı role namespace'inde yalnız ACTIVE role closure için
  geçerlidir; doğrudan login actor kimliğinden bu grant'lar devralınmaz. Yeni
  CREATE ROLE provenance/lifecycle tipi varmış gibi gösterilmez.

## Build12 gerçek kabulü ve 12a dar düzeltmeler

- Immutable12 `4ec267cb8444f8ecfe5f4c314d4422af69f1769159198e234a10c1216634b8e9`
  ile altı production/native suite PASS: SQL/SP/projection/FOR, relational
  QUALIFY/BY NAME/pipeline, inspection binary metadata/source map/DDL reprepare
  ve restart persistence, BY NAME view/ODKU, ORDER once-only ve production
  local capture/SP. Her biri fresh init/TLS ve temiz shutdown kullandı.
  Loglar `out/sql-short-native/build12-*.log`.
- FULL gerçek suite ilk scalar/bag/NULL/16 graph/volatile ON/post-filter/sort/
  LIMIT/prepared aşamalarını geçti; materialized view SELECT'inde
  `JOIN::clear_corr_derived_tmp_tables()` null QEP_TAB ile0xc0000005 verdi.
  FULL-only iterator planı için bu legacy correlated-derived loop atlandı;
  child iterator Init/owned table-function cleanup yolu korunur. View reopen,
  prepared view tekrar/empty-intermediate/reset/deallocate kabulü eklendi.
  `out/full-outer-next/build12-native.log` bu başarısız kabulün kaydıdır;
  partial ilerleme FULL feature PASS anlamına gelmez.
- AI post-commit DD drift guard doğru61081 verdi ve helper ALTER provider wait
  sırasında tamamlandı; mevcut DA plaintext olduğundan structured committed
  metadata kayboldu. Pin failure/resume boundary artık mevcut errno/SQLSTATE/
  message'ı kopyalar, error status temizlendikten sonra phase-aware condition
  üretir. before_commit not_committed, after/poll committed/unknown=false;
  rollback_turn yanlış committed etiketi almaz. İki dar kaynak fix 12a batch'ine
  canonical hunk ile alındı; gerçek yeniden kabul yeni binary'yi bekler.
- Workbench ayrı Action/Role capability maskeleri8/16 (HIST4 boş) gerçek native
  JSON yollarına bağlıdır. Core-only bağlantıda yeni öneri/validation bypass
  yoktur. Parser/forms/wbpublic/wbprivate build PASS; gerçek DLL109 lexer,
  completion ve hover kontrolü ile splitter12 PASS. Native Action/Role13
  runtime kabulü bu frontend kontrolünün parçası değildir.
- Immutable12a
  `abf9efef4352cb4bb3ea25187091f1406495c1f65bae309933347db20fbfc8e4`
  ile base FULL bütün native suite PASS ve temiz shutdown: view reopen, repeated
  prepared view empty/reset/deallocate, classic SP tekrar ve DD language1,
  scalar/NULL/physical bags/16 graph/volatile ON/filter/sort/LIMIT, BLOB/JSON,
  wire nullability, ACL/explicit unsupported shapes ve empty inputs. Log
  `out/full-outer-next/build12a-native.log`. Explicit legacy EXPLAIN negative
  testi FORMAT=TRADITIONAL kullanır; bare EXPLAIN default TREE olduğundan geçerli
  iterator planıdır. Knowledge table-source/FULL child kabulü ayrı context
  suite'indeki callerLEX cleanup çökmesi çözülene kadar hâlâ bekler.
- 12a commit guard kabulü de PASS: prepared private helper drift nontransactional
  ledger0, provider wait sırasında ALTER'nin tamamlanması, phase=after_commit /
  commit_state=committed / unknown_outcome=false JSON ve sonrasında claimable /
  publishable durable run. `out/ai-native-check/build12a-commit-guard.log`.
- SDK native binary binding incelemesinde ikinci gerçek ABI sorunu bulundu:
  BLOB252 parametre değerinin row'u binary kalırken `SELECT ?` result metadata
  resolved UTF8 charset255/flags0 gönderiyordu. Yeni raw Classic PS fixture
  build12a'da aynı hatayı bağımsız üretti; payload00/01/FE/FF değişmedi fakat
  metadata yanlış text iddiası taşıdı. `out/sql-short-native/build12a-binary_parameter_native.log`.
  `Item_param::make_field` yalnız değeri atanmış STRING/LONGDATA slotlarında
  actual collation charset/BINARY flag kullanacak biçimde dar düzeltildi;
  NO_VALUE prepare metadata ve declared routine OUT override korunur. item.cc
  `/Zs` PASS, canonical hunk isolated syntax source ile exact eşleşir.12b actual
  empty/nonUTF8/validUTF8bytes/text/NULL/mixed/repeatedPS/longdata/OUT kabulünü
  bekler. Client invalid UTF8 tahmini veya SQL metni rewrite yapılmaz.

## Build12c bütünleşik FULL/Knowledge ve binary wire sınırı

- Build12b raw PS fixture'da bytes doğru ve BINARY_FLAG128 olmasına rağmen
  charset255 kaldı. Protocol_classic, Send_field.charsetnr yerine ayrı
  Item::charset_for_protocol resolved collation argümanını kullanıyordu. Dar
  wire fix yalnız explicit descriptor charset63 + BINARY_FLAG + simple string
  için binary charset'i tercih eder; payload tahmini, CAST/numeric/JSON/ENUM/SET
  veya UTF8 _bin yeniden sınıflandırması yapmaz. `/Zs` PASS; yeni fixture bu
  sınırları unbound prepare/OUT/mixed/repeatedPS/longdata ile denetler.
- FULL/KT12b çökmesinin tam console'u
  `out/full-outer-next/kt12b-full-console.log` ile korundu. Gerçek assert
  trx0trx.cc2014 `trx->lock.trx_locks==0`: önce RIGHT base rows okunup AC-NL-RO
  transaction başlıyor, ardından LEFT Knowledge metadata FOR SHARE kilitleri
  ekleniyordu. Aynı değişmemiş binary/sorgu explicit TX'de geçerek nedeni
  doğruladı; bu diagnostic workaround final kabulde kullanılmaz.
- Native FULL iterator artık iki child'ı ilk base-row okumasından önce Init
  eder; sonra RIGHT'ı bir kez spool eder ve LEFT'i okur. InnoDB rnd_init yalnız
  cursor kurar, row/trx başlatmaz. Row/metadata lock'ları ve owned iterator
  cleanup korunur; unlock/validation atlanmaz. Canonical + template exact,
  access_path/protocol `/Zs` PASS.
- 12c snapshot79files338248bytes
  `30630c5ab1707d585a8afac9e75dc54e80ef3e20e466c5a2d05cb4538e2408d2`
  clean isolated-index apply + normalized source reproduction PASS. Immutable
  runtime `a5d465ddc8580090418f870d28eb05c8e2d9257e21907a589a94a1820c463d66`.
- 12c `ai_context_native --full` ve `full_outer_native_acceptance` tamamı PASS,
  fresh init/TLS ve temiz shutdown. KT sol/sağ-spool PS param2→uint64MAX→2→empty99
  →2, exact unsigned wire metadata ve unmatched bags; ayrıca base FULL view/SP/
  PS/empty/reset/nullable metadata/volatility/BLOB/JSON/ACL kapıları geçti.
  Loglar `out/ai-native-check/build12c-context-full.log` ve
  `out/full-outer-next/build12c-native.log`. Backend13 Action/Role/Tool kabulü
  bu kayıttan ayrı kalır.

## Native13 Action/Role entegrasyonu

- AI tools/policy ve normalized routine MDL guard dilimi canonical'e alındıktan
  sonra native ON-object `TO ROLE` / `FROM ROLE`, custom AI object grants,
  ordinary CALL prepare/execute approval preflight ve native authID DROP/RENAME
  lifecycle sekiz dosyalık own-hunk patch ile latest source üzerine clean apply
  edildi. Eski stage dosyaları canonical'in üzerine kopyalanmadı.
- Native roller unified MySQL authID modelini kullanır; yeni, henüz kimseye
  atanmamış CREATE ROLE için ACL_USER::is_role köken kanıtı olarak kullanılmaz.
  Recipient listesi mevcut ACL WRITE lock altında ilk mutation'dan önce tümüyle
  doğrulanır. Custom AI role principal yetkisi yalnız aktif role closure'da
  uygulanır; DROP/RENAME cleanup kurulmuş module için actions OFF bakımında da
  native ACL transaction'ına katılır.
- Bu dilim sonrası Knowledge/FULL/capture/commit/pipeline dahil on protected12c
  SHA ve root'un declared OUT `sql_class.cc` düzeltmesi exact PASS. Native auth
  ve lex TU `/Zs` PASS; combined Action grammar/header syntax ve gerçek13
  runtime kabulü henüz tamamlanmadı. Native role fixture AST PASS; release
  capability bu ara kayıttan hareketle açılmaz.
- Modal'ın üç authoritative Action/principal/identity headerı ve startup
  enforcement flag'i latest source üzerine birleşti. Taze canonical Bison36,
  generated parser, sql_call/sql_parse/sql_user/sys_vars MSVC `/Zs` tamamı PASS;
  native authorization/lex de PASS. Son kaynak hash kaydı
  `out/role-grant-next/combined13-freeze.json`; role/grant/CALL/lifecycle runtime
  kabulü yeni13 binary'sini bekler.
- Workbench exact13 eşlemesi sonrasında gerçek parser DLL probe140 ve splitter12
  PASS. Parser/forms yanında server-authoritative syntax kontrolünün gerçek
  consumer'ı db.mysql.parser modülü de yeniden derlendi. İlk probe'un bulduğu
  `@id` tek-token lexer biçimi dar düzeltmeyle kapandı; ordinary DESCRIBE
  table/column yolu korunur. Loglar Workbench
  `out/sdp-language-native-tests/result-exact13-profile-editor.log` ve
  `build-exact13-profile-editor.log`. Ara tool önerileri, release'in konservatif
  AI+Actions profiline mevcut Action bit'iyle bağlandı. Core AI'daki bare
  WITH FUNCTIONS inference seçeneği korunur; explicit typed tool listesi,
  metadata ve execute önerisi/validation bypass core-only bağlantıda açılmaz.
- Final14 capability formatter yalnız `out/capability14/` altında izole patch
  olarak hazırlandı. Exact C++ formatter16 startup/legacy/model kombinasyonu,
  gerçek native TU `/Zs`, clean apply check ve canonical değişmedi kanıtı PASS.
  Gerçek13 kabulü ve root review tamamlanmadan acceptance gate canonical'de
  açılmaz; gerçek14 wire/UI kabulü bu statik hazırlıktan ayrı kalır.
- Workbench object edit yalnız body göstermeyi bırakıp nesneye bağlı gerçek
  ALTER/dispatch SQL'i oluşturur: Knowledge tüm tip/nullability metadata'sını,
  Behavior güvenli literal'i ve Response Schema version+1 + typed JSON CAST'ini
  kullanır. Tanım editoründeki bounded deterministic JSON encoder Decimal
  sayıları string'e çevirmez. Önceki21 test korunarak27 unit PASS.
- Bu çalışma native Knowledge text tipinin canonical COLLATE bilgisini kendi
  create/alter grammar'ının geri kabul etmediğini gösterdi. Metadata sessiz
  düşürülmedi. İki column production'a mevcut opt_collate nonterminal'ini ve
  kaynak aralığını ekleyen dar14 patch/template yalnız out/knowledge-edit14
  altında hazır; Bison36 + generated parser `/Zs` ve clean apply check PASS.
  Canonical13 değişmedi. Yeni workspace_edit_native_acceptance fixture'ı
  Knowledge/Behavior/Schema sürüm2 + charset/nullability/uint64/Decimal ve üç
  UI model SQL biçimini gerçek sunucuda denetleyecek; full14 runtime NOT_RUN.
- Immutable13 (`9a16886bd3b846f3c45f6af39cbd2de9455b64e16706c68b2b899d38cccf64c4`)
  altı ordinary SQL ailesinin tamamında PASS: projection/SP/FOR, relational,
  inspection/DD persistence, BY NAME view/ODKU, ORDER ve extended FULL. Özet
  `out/sql-short-native/build13-regression-summary.json`; her biri fresh TLS
  fixture ve temiz shutdown ile exit0. Default AI/actions OFF profili yeni
  native hook'ların normal SQL davranışına etkisini sınar.
- 13 UI builder `--skip-collation` kesiti PASS: numeric Knowledge/Behavior/Schema
  sürüm2, SQL NULL/uint64/Decimal/JSON-number, iki cloud CREATE MODEL expression
  ve GGUF pending_import metadata; provider/model I/O NOT_RUN. Explicit UTF8
  client charset ile de PASS. Full-collation13 baseline aynı değişmemiş
  fixture'da tam COLLATE noktasında1064 ile RED; dar14 grammar kabulü bekler.
- Root'un gerçek managed SDK testleri legacy EOF OUT result terminator'ının
  MORE_RESULTS bit'ini kaybettiğini gösterdi. THD final status'u korunarak yalnız
  bu EOF'a PS_OUT/MORE bitlerini ekleyen izole14 patch hazır; native
  protocol_classic `/Zs` ve clean apply check PASS. Exact production terminator
  C++ kodu/gerçek flag'lerle8 profil: baseline classic EOF RED, staged classic ve
  DEPRECATE_EOF GREEN; autocommit wire4106, final THD2. Gerçek post-OUT wire/SDK
  kabulü14 runtime'da root tarafından alınacak.
- Ordinary TO ROLE fixture'ın Unicode persistence kontrolü13'te RED oldu:
  mysql.procs_priv'de UTF8 kullanıcı adı Latin1 kaynak varsayımıyla çift
  kodlanıyordu. Ayrı disposable13 probe CREATE ROLE/USER ve schema/table/column
  quoted identifier yolunun doğru UTF8 bytes ürettiğini gösterdi; sorun
  sql_user_table.cc replace_routine_table'ın dört key field store'una daraldı.
  Bu dört kaynak native system_charset_info olarak düzeltilen izole14 patch
  `/Zs` ve clean apply check PASS. Eski satırlara conversion/deletion yapılmaz.
  Fiziksel UTF8 schema/routine/authID/host-case rows ve clean restart sonrası
  role/direct-user ACL/revoke kabulü genişletilmiş fixture'da14'ü bekler;
  başlangıçtaki dört-grant beklentisi değiştirilmedi.
- Root onayıyla14 canonical batch sıralı birleştirildi: EOF status,
  UTF8 routine grant keys, Knowledge opt_collate+tracked grammar template;
  ardından AI Routine return-field buffer ve Modal bounded Action RPC/DCL
  düzeltmeleri. Son Bison36 ve generated parser/protocol_classic/sql_user_table/
  sql_call/sql_parse/item_create `/Zs` tamamı PASS. Tam93-file13 manifestine
  karşı yalnız6 planlı native source delta var, diğer88 hash exact.
  `out/engine14-merge/final-source-proof.json`. Capability gates değiştirilmedi;
  final metadata15, gerçek14 runtime kabulü sonrasında ayrı uygulanacak.
- Immutable14 (`d85abab043ee6fb4577a193707421389f96daf554fcdcf5223a30380456fb013`)
  full Workbench edit/model builder ve generic/Unicode role persistence
  fixture'ları exit0 PASS, temiz shutdown. Knowledge canonical charset/COLLATE
  ve nullable signature aynı kaldı; K/B/Schema sürüm2, actual JSON sayı türü,
  uint64/Decimal/NULL ve üç native UI model expression biçimi doğrulandı.
  Unicode schema/function/procedure/role/direct-user fiziksel grant HEX'leri,
  host-case, clean restart sonrası ACL ve targeted revoke geçti. Eski rows'a
  dönüşüm yapılmadı; test verileri disposable'dır.
- 14 native keyword quick slice da iki temiz fixture'da PASS: AI OFF/ON,
  her profilde30 unqualified ad, case/space ve VERSION() korunur. Toplu byte/log
  kanıtı `out/engine14-merge/runtime-gates.json`. Kaynak/capability yazımı yok;
  final metadata kararı diğer14 tool/action/SDK kapılarından ayrı açılmaz.
- 14 Decision trigger karşılaştırması owner grant'inin doğru olduğunu gösterdi:
  ordinary UPDATE (4,8), CALL (5,10) ve negative DENY1644 geçti; aynı native
  protected ToolScope SELECT(quantity)1143 ile rollback oldu. Kök neden
  definer→owner context geçişinde aynı current-version GRANT_INFO cache'inin
  tekrar kullanılmasıdır; native fill yalnız version mismatch'te grant_table
  yeniler. Sadece ToolScope için fresh owner lookup+original definer cache'ini
  success/error RAII restore eden15 fix canonical'e root onayıyla alındı.
  Owner table/column/active-role yetkileri normal ACL fonksiyonuyla doğrulanır;
  definer TRIGGER gate ve sonraki grant_version recheck korunur.
- 15 batch AI cached tools/Knowledge validator null-value resetleri ve Modal
  Action resource CAST/SP result flag düzeltmesiyle birleşti. Son Bison36 ve
  parser/item_create/sql_call/sql_parse/sp_head `/Zs` PASS; yalnız4 native delta,
  diğer90 source14 hash exact, capsfalse. Kanıt
  `out/decision-trigger15/final-source-proof.json`. Runtime15 tool recovery,
  ordinary/tool/restricted-definer cache restore ve revoke kapıları beklenir;
  başarı bu syntax hazırlığından çıkarılmaz.
- Immutable15 (`523f7a27037db44cd4e998d0bcbfc73282f08f85aa0a3eea163d09a99e249830`)
  hedefli Decision/trigger actual kabulü exit0 PASS, temiz shutdown. Mevcut
  gerçek invoke/publication helper'ı ile ordinary owner UPDATE/CALL/DENY önce ve
  sonra, scoped Decision SET/DENY, restricted DEFINER ordinary1143→owner tool
  başarı→ordinary1143 ve protected publication sonrası owner SELECT revoke ile
  rollback/no-write/regrant kontrol edildi. Worker identity/timezone/table ACL
  restore kontrolleri korunur. Bu hedefli driver unrelated full tool recovery,
  approval ve SDK/timeout kontrollerini başarılıymış gibi raporlamaz; onlar
  AI agent'ın ayrı strict suite kapılarıdır. Kanıt
  `out/decision-trigger15/runtime-gate15.json` exact exe/log/fixture SHA'larını
  içerir; kaynak veya capability yazımı yapılmadı.
- Immutable18 final capability kapısı gerçek sekiz startup ve mysql/legacy
  oturumlarıyla16 ayrı formatter eşleşmesini geçti. SESSION/GLOBAL toplam32
  değiştirme denemesi native1238 ile reddedildi, global değerler değişmedi ve
  sekiz disposable sunucu temiz kapandı. Kanıt
  `out/capability16/native-profile-proof18.json`.
- Son canlı Workbench kabulü CALL tabanlı SHOW sonrasında istemcinin fiziksel
  bağlantıyı16→17 yenileyip aktif rolü kaybettiğini gösterdi. Server ACL/grant
  değişikliği yapılmadı. Workbench'in main/aux query API'leri ilk scrollable
  buffered ResultSet'i tutup tüm ek rowset/OK'leri tüketir; geç SQL hataları
  aynen iletilir ve SQL tekrar çalıştırılmaz. Kurulu Connector/C++9.4.0 resmi
  kaynaklarında buffer/ownership ve getMoreResults davranışı doğrulandı.
  Tek wbprivate derlemesi PASS. Gerçek GRT kapısında main16/aux15 sabit kaldı;
  tekrarlı SHOW/DESCRIBE, ilk sonucun sonraki sorgulardan sonra okunması,
  iki sonuçlu CALL, geç SIGNAL1644 ve birer çağrı sayacı geçti. Ardından tam
  sekiz sayfalı canlı UI/rol/katalog/diagram/timeline/Action preview/AI SQL/TLS
  fixture'ı exit0 PASS. Provider I/O ve credential yazımı yapılmadı. CLI açılış
  betiğinin önceden ayrıca gösterilen kapanış beklemesinde yalnız tamamlanmış
  JSON sonrasında owned parent cleanup kullanılır; normal GUI shutdown olarak
  sunulmaz. WB `out/call-result-completion/buffered-query-proof.json` kaynak/DLL
  hash'lerini, ilk fixture1064 geçmişini ve iki gerçek kabul logunu içerir.
  Release staging/source arşivi yeni Workbench DLL'iyle root tarafından yenilenir.
