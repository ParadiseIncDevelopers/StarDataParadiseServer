# Mevcut yerel verilerle Server 0.5.0 devreye alma

Kullanıcı isteği: mevcut verileri koruyarak güncel Server'ı çalıştırmak,
Workbench bağlantısını ona yönlendirmek ve AI/Knowledge ekranlarını açmak.
MySQL Installer bu çalışmanın kapsamında değildir. HIST/Git ve public JOB
özellikleri kapalı kalır.

## Uygulanan geçiş

- Eski model-only süreç, aynı MySQL tabanı olan stable Server 0.5.0 ile
  değiştirildi. Ayrı MySQL80 servisine müdahale edilmedi.
- Eski sunucu UUID'si ve mevcut veri dizini korundu. Native port 41754,
  loopback bind, GTID/binlog ve TLS ayarları korundu.
- Temiz SQL SHUTDOWN sonrasında veri dizinindeki 185 dosya soğuk yedeğe
  kopyalandı; kaynak ve yedek dosyalarının SHA256 değerleri karşılaştırıldı.
  TLS dosyaları ve Workbench kullanıcı profili ayrıca yedeklendi.
- Release paketinin tamamı ayrı çalışma dizinine kopyalandı. mysqld SHA256:
  `16dd394190591af66f39171d86227cf74ac3cfce96f01490389d3b1f9cb1f346`.
- Bakım açılışı: `--sdp-ai-sql=ON --sdp-ai-actions=OFF`. Production
  `install_queue` ile yalnız iç AI kuyruğu kuruldu. Ardından paket içindeki
  `catalog.sql`, `tool_catalog.sql`, `action_catalog.sql` sırayla ve ilk hatada
  durma kuralıyla uygulandı. Yeni kullanıcı veri şeması başlatılmadı.
- Son temiz açılış: `--sdp-ai-sql=ON --sdp-ai-actions=ON`. Tüm AI capability
  anahtarları true; `short_sql_v1` true; HIST ve public JOB false.
- Workbench `test` profili 3197 yönlendiricisinden doğrudan 127.0.0.1:41754'e
  geçirildi. TLS VERIFY_IDENTITY ve mevcut CA kullanılıyor. Parola, mevcut
  şifreli Workbench kasasında yeni endpoint'e aktarıldı; SQL/argv/log içine
  yazılmadı. Gerçek native TLS kimlik kontrolünün ardından profilin endpoint
  doğrulama kaydı ve ana bağlantı listesi yenilendi.

## Workbench katalog düzeltmesi

Canlı şema ağacında ilk SHOW CONVERSATIONS çalışıyor, sonraki dört katalog
"Kullanılamıyor" görünüyordu. Native SHOW komutunun kullandığı CALL son OK
paketi, ağaç denetleyicisinde tüketilmiyordu. `wb_sql_editor_tree_controller.cpp`
buffered result kullanıp her katalogdan sonra bütün ek sonuçları/OK paketini
tüketir. Rol veya fiziksel bağlantı değiştirilmez; sorgu tekrar edilmez.

wbprivate.be.dll Release_OSS olarak derlendi ve yerel teslim runtime'ına
yerleştirildi. Eski DLL yedeklendi. Yeni DLL SHA256:
`e68d3c8a1febbf1e7c110f3945c7f0495e135eaa6448cd281fca05e19bdc9384`.
Bu yerel bakım düzeltmesi mevcut yayın etiketlerini veya GitHub release
asset'lerini değiştirmez.

## Doğrulamalar ve karşılaşılan hatalar

- PASS: Üç mevcut iş tablosunun satır sayısı ve CHECKSUM TABLE EXTENDED
  değerleri aynı. `new_db_for_ai.ai_exam_demo` içindeki 8 kayıt korundu.
- İlk envanterde bir SHOW CREATE çıktısı eksikti. Yedeğin ayrı doğrulama
  kopyası eski motorla yalnız geçici loopback portunda açıldı. Üç tablonun
  bütün DDL'leri yeni motorla birebir eşleşti. Doğrulama süreci temiz kapandı;
  asıl yedek değiştirilmedi.
- PASS: Canlı TLS oturumunda typed Knowledge CREATE/EXECUTE, model/behavior/
  response schema CREATE ve beş SHOW kataloğu. Tek transaction ROLLBACK
  sonrasında bütün deneme nesneleri kaldırılmış; katalog nesne sayısı 0.
- PASS: Workbench'in gerçek GRT API'siyle beş katalog aynı fiziksel ana
  oturumda üç tur okundu. Sekiz çalışma alanı sayfası açıldı. Kullanıcı
  şemasında beş AI klasörünün hepsinde Kullanılamıyor etiketi kalktı;
  Knowledge ekranı açık bırakıldı.
- PASS: REGEXP_LIKE sağlık sorgusu; native UUID/port/TLS kontrolleri.
- İlk incremental derleme eski output dizininde `sdp.forms.lib` bulunamadığı
  için LNK1181 verdi. Kabul edilmiş typed runtime output diziniyle tekrar
  derleme başarılı. Yalnız ATL debug PDB LNK4099 uyarıları var.
- Sandbox CIM erişimi reddedildi; salt okunur host incelemesi ve devreye alma
  yetkilendirilmiş yükseltilmiş araç çağrılarıyla yapıldı.
- Workbench embedded Python'da `_ssl` import edilemediği için eski recovery
  yardımcılarını yükleyen ilk profil script'i başarısız oldu. İşlem Workbench'in
  kendi şifreli parola API'siyle tamamlandı; kullanıcı parolası değişmedi.
- Windows erişilebilirlik ağacı okuması `WBAccessibleObject.get_Value()`
  içinde AccessViolationException üretti. Sunucu etkilenmedi. Workbench
  yeniden açıldı; son doğrulama screenshot-only ve kendi GRT API'siyle yapıldı.
  Bu erişilebilirlik sorunu bu değişiklikte düzeltilmedi.
- Script ile eşzamanlı schema fetch sırasında GRTDispatcher message handler
  count mismatch kaydı görüldü. Son GRT sorgu ve görünür ekran kontrolleri
  geçti; sonuç raporunda başarı yalnız doğrulanan davranışlar için verilir.
- NOT_RUN: Kullanıcıya ait API anahtarı veya yerel model sağlanmadığı için
  gerçek dış sağlayıcı inference çağrısı yapılmadı. Ayrı provider worker bu
  çalışmada devreye alınmadı; inference için README'deki güvenilir worker
  yapılandırması ve kullanıcı model/credential bağı gerekir. Katalog, Knowledge
  ve UI kullanılabilirliği API anahtarına bağlı değildir.

## Yerel işletim kayıtları

Gizli/yerel ayrıntılar Git'e eklenmez. Yedekler, hash manifestleri, eski süreç
argümanları, veri karşılaştırmaları, SQL/GRT probe raporları ve
`Start-Server.ps1` yeniden başlatma yardımcısı Server deposunun ignore edilen
`out/deployment-20261121/` dizinindedir. Veri dizini, TLS, yedekler ve bu runtime
geçici dosya temizliğinin hedefi değildir. Windows servisi/otomatik açılış
kurulmadı; çalışmakta olan kullanıcı süreci güncellendi.
