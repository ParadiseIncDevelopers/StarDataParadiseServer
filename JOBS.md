# Kalıcı işler

Kimliği doğrulanmış Server oturumları Engine açık olduğunda `jobs_v1` ve `sdp_job_command` kullanabilir. Etap 23’ün worker’ı parametreli `compute` işini yürütür. Sonuç gerçek iterasyon sayısını ve ondalık 64-bit checksum’ı içerir. Hazır iş türleri `job_capabilities` ile bildirilir.

`job_submit` bir request token, iş türü ve tipli yapılandırma alır. Açık transaction içinde talep önce oturuma özeldir. Commit, kuyruğa alınan işi WAL ile yayımlar. Rollback worker başlatmaz. Autocommit, aynı token ve aynı yapılandırma ile tekrarlandığında aynı iş kimliğini bulur. Başka oturumdan iş sahibi veya güncel yönetici status ve event akışını okuyabilir.

İşler `queued`, `starting`, `running`, `succeeded`, `failed` ve `cancelled` durumlarına geçebilir. Gerçek iş miktarı bilindiğinde worker PID ve `done/total` ilerlemesi gösterilir. `job_cancel` talebi kalıcı olarak kabul eder. Terminal `cancelled` durumu worker sona erdikten sonra yayımlanır. Server kesildiğinde tamamlanmış sonuçlar korunur. Önceden aktif olan ve sonucu yayımlanmayan iş yeniden başlatmada `interrupted` olur.

Worker aynı Server EXE’sinin kontrollü bir alt sürecidir. Windows Job Object süreç sayısı, bellek ve işlem yaşam döngüsü sınırlarını uygular. CPU oranı uygulanabildiğinde sonuç işaretinde görünür. Duvar saati ve çıktı limitleri parent süreç tarafından denetlenir. Temp aktarımı tek, sınırlı bir çalışma dizini ve sabit dosyalar kullanır. İşlemden sonra kapatılan temp alanı temizlenir, restart temizliği canlı workspace kilitlerine saygı duyar.

Zamanlayıcı, sabit saniye aralığı veya Windows zaman diliminde günlük saat alır. DST politikası [takvim sözleşmesinde](jobs/SCHEDULE.md). Kaçırılan pencere için `skip` ve `run_once` vardır. Aktif işlemle çakışan pencere ileri taşınır. İş kimliği ve sonraki pencere tek transaction’da yazılır. Yapılandırılmış retry aynı iş kimliğinde sınırlı attempt ve backoff kullanır. Etap 23’te 128 iş ve 32 schedule tutulur. Worker sayısı instance başına birdir. İşler için checkpoint/resume bu etapta kapalıdır.
