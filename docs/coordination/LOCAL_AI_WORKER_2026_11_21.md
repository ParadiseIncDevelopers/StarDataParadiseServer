# Yerel AI worker ve üç sağlayıcılı Workbench akışı

Kullanıcı isteği: worker'ı yapılandırıp devreye almak; kullanıcı OpenAI veya
Claude anahtarını girerek ya da kendi yerel modelini import ederek kullanabilsin.
Bu çalışma mevcut Server 0.5.0 ve Workbench 0.3.0 yerel kurulumunu tamamlar.
GitHub yayınları/etiketleri değiştirilmedi; SDK wire sözleşmesi değişmedi.

## Devreye alınan kapsam

- `sdp_ai_worker_local@localhost` ayrı hesabı, REQUIRE SSL, managed account
  attribute ve tam dokuz per-procedure EXECUTE grant ile kuruldu. Yordamlar:
  claim_run, renew_lease, publish_run, claim_model_import, renew_model_import,
  publish_model_import, publish_tool_requests, publish_tool_failure,
  tool_call_status. Doğrudan tablo veya global/schema wildcard yetkisi yok.
- Bu kalıcı yetkiler ilk otomatik incelemede reddedildi. Kullanıcı alıcı hesabı,
  dokuz yordam kapsamını ve dosya ACL'sinin kendi Windows hesabı/SYSTEM ile
  sınırlanmasını açıkça onayladı; ardından kurulum gerçekleşti.
- SQL parolası Windows Credential Manager'da; config/argv/JSON/log içinde
  açık parola yok. Worker dosyalarının ACL'si onaylanan iki principal'a sınırlı.
- Worker ayrı TLS VERIFY_IDENTITY oturumuyla 127.0.0.1:41754'e bağlanır;
  UUID ve gerçek authenticated worker actor her yeniden bağlantıda doğrulanır.
  Actor `sdp_ai.worker_accounts` içine kaydedildi.
- Supervisor trusted config'i işler arasında yeniler, health heartbeat üretir,
  SQL bağlantısını geri kurar. Belirsiz provider/RPC işi yeniden gönderilmez;
  native lease ve durable queue sonucu belirler. Windows dosya kilidi aynı
  kurulumun ikinci worker'ını engeller. Llama süreçleri mevcut owned job sınırında.
- Mevcut llama.cpp b12248 CPU runtime tam DLL setiyle ayrı worker dizinine
  kopyalandı. İzinli import dizini ve content-addressed model deposu hazır.
- Kullanıcıya özel Startup kısayolu denemesi otomatik incelemede ayrıca
  onaylanmamış kalıcılık etkisi nedeniyle reddedildi; kısayol oluşturulmadı.
  Daha dar alternatif uygulandı: çalışan arka plan worker ve Workbench'te
  **Worker’ı başlat** manuel düğmesi. OS otomatik başlangıcı yok.

## Kullanıcı ekranı

AI modellerinde OpenAI bağla / Claude bağla, şema/SQL model adı/sağlayıcı model
kimliği/kasa başvurusu/maskeli API anahtarı formunu açar. Anahtar Windows
kasasında kalır. Provider bağı yalnız resmi uç noktaya ve kurulu SQL owner'a
eklenir; başka owner/UUID/port veya özel proxy otomatik güvenilir sayılmaz.
Ardından anahtarsız CREATE MODEL SQL'i kullanıcı incelemesine açılır.

GGUF dosyası seç, normal GGUF v2/v3 dosyasını arka plan thread'inde kopyalayıp
hash hesaplar; UI ilerleme gösterir, kaynak korunur. Pencere kapanırsa kısmi
kopya temizlenir. IMPORT SQL'i ayrıca incelenip çalıştırılır; worker doğrulama
ve gerçek yüklemeyi tamamlamadan active modeli bildirmez. İleri kayıt formu
kaydırılabilir sekmeye taşındı; dar merkez panelinde düğmeler iki sütundur.
Worker durumu gerçek ve güncel supervisor kaydını gösterir. Workbench teslim
EXE'sinin modules dizini yedeklenerek güncellendi; aynı test profili açıldı.

## Kontroller

- PASS: 60 Server unit testi (service, worker, provider adapter, response schema,
  owned process, native manifest). İlk komuttaki var olmayan test_ai_secrets
  modül adı düzeltildi; son komutun tamamı başarılı.
- PASS: 28 Workbench workspace testi ve 7 yerel worker testi: owner/UUID/port
  sınırı, config'i koruyarak alias ekleme, provider/endpoint reddi, gerçek hash,
  kaynak koruma, geçersiz/kesilen import temizliği, health freshness ve manuel
  başlatıcı yol sınırı.
- PASS: Aynı Windows identity ile gerçek Credential Manager yazma/okuma;
  OpenAI ve Claude bağları, farklı owner/provider/endpoint reddi. Rastgele test
  değerleri dış ağa gönderilmedi ve oluşturulan test credential'ları temizlendi.
- PASS: Gerçek GGUF dosyası (SHA256
  6151b1929d7f5aa3385d9ddef3393e55587c0a55de661562322bc51dfda93a04)
  UI ile aynı staging API'sinden kopyalandı. Native IMPORT kuyruğunu arka plan
  worker aldı, gerçek llama.cpp runtime yükledi ve model active oldu.
- PASS: SQL AI konuşması → durable run → ayrı worker → gerçek yerel LLM →
  sync TEXT yanıtı ve local_gguf provider metadata. Sahte provider yok.
  Kabul model/konuşması owner'ın kataloğunda kayıtlı; kimlikleri yerel raporda.
- PASS: Yalnız boş worker SQL oturumu KILL CONNECTION ile kesildi; aynı worker
  süreci yeni fiziksel TLS oturumuna geçti ve ready durumuna döndü. İş tabloları
  checksum ve satır sayıları aynı.
- PASS: Gerçek Workbench/GRT: on düğme, maskeli cloud form iptal akışları,
  GGUF dosya seçici imzası, yerel owner doğrulama, sağlık okuma ve idempotent
  manuel başlatma. Screenshot-only kontrolünde düğmeler görünür ve worker çalışıyor.
- PASS: Kurulu hesabın SHOW GRANTS çıktısı tam dokuz yordam ile USAGE içeriyor.
- NOT_RUN: Kullanıcı gerçek OpenAI/Claude anahtarı vermediği için dış sağlayıcıda
  ücretli inference çağrısı yapılmadı; adapter sözleşmeleri ve yerel kasa/bağ
  doğrulaması bu eksik kabulün yerine sunulmaz.
- İlk başlatıcıda CLI için bir fazla üst dizin ve eksik mysqlsh config dizini
  hataları düzeltildi. Son süreç ready; stdout/stderr boş. UI probe ilk beklenen
  düğme sayısı 11 yerine 10 olarak düzeltildi; son probe başarılı.

Yerel config, kullanıcı manifesti, başlatıcı, health ve test raporları
`out/deployment-20261121/` altında veya Workbench kullanıcı profilinde bulunur;
Git'e sır/yerel config eklenmez. Worker çalışma dizini veri/runtime'dır; temp
temizliğinde silinmez. Önceki veri yedekleri korunur. Daha önceki yerel Server
aktivasyon kaydındaki worker NOT_RUN durumu bu etapta tamamlanmıştır.
