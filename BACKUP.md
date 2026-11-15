# Bağımsız yedekleme ve geri yükleme

Yedekleme için Server yapılandırmasına mevcut bir `backup` dizini ekle. Bu dizin data, log ve temp dizinlerinden bağımsız olmalıdır. Disk arızasına karşı saklanacak yedekler için ayrı disk veya ayrı depolama kullan. Yapılandırma yoksa `backup_v1` kapalıdır.

`backup_submit` bir `request_token` ve isteğe bağlı `max_bytes` ile kalıcı iş oluşturur. `backup_status` ve `backup_cancel` `job_id` alır. `job_schedule_create` içinde `kind:"backup"` ile aynı Scheduler’da interval veya günlük iş oluşturulabilir. Bağlantı kapansa da iş Server’da devam eder. Bu işlemler Server administrator yetkisi gerektirir.

Arşiv `backup/<backup_id>` altında `store.sdp`, bağlı WAL segmentleri, snapshot checkpoint’i, `logical.json` ve `manifest.json` dosyalarından oluşur. Storage lease altında tutarlı fiziksel snapshot ve aynı revizyonun tipli SQL export’u alınır. Logical export katalog, kolonlar, tablo satırları, routine kaynağı ve History/model referanslarını içerir. Fiziksel arşiv veritabanındaki WAL ve model artifaktlarını da kapsar. O anki LSN, checkpoint, dosya boyutları, SHA-256 ve doğrulama zamanı manifestte saklanır. İş ancak arşiv geçici instance’a geri yüklendikten, fiziksel bütünlük ve tipli SQL içerik eşitliği doğrulandıktan sonra hazır olur.

`backup_verify` bir `backup_id` ile aynı geçici restore doğrulamasını yeniden çalıştırır. `backup_restore` arşiv kimliği ve `target_name` ile bağımsız bir `temp/restores/<target_name>` dizini yaratır. Hedef mevcutsa işlem reddedilir. Model ve routine kayıtları, History ve SQL satırları taşınır. TLS anahtarı ve bootstrap doğrulayıcısı ayrı yerel kimlik kurulumuna tabidir. Geri yüklenen hedefe yeni bir Server profili ve TLS sertifikası yapılandırarak bağlan.

`backup_catalog` arşiv listesini ve doğrulama durumunu gösterir. `backup_delete` seçilen arşivi kaldırır ve mevcut bir restore dizinine dokunmaz. Yetersiz alan, bozuk manifest, eksik dosya veya SHA-256 uyuşmazlığı durumunda hazır yedek ya da restore hedefi yayımlanmaz.

PITR capability kapalıdır. Aynı arşiv içinde tutarlı fiziksel WAL saklanır. Bağımsız, kesintisiz arşiv WAL zinciri üretilmez.
