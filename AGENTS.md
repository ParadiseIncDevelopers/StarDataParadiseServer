# StarDataParadise Server

Bu depo bağımsız bir Visual Studio çözümüdür. CMake ve test çıktıları depo kökündeki ignore edilen `out/` dizinine gider. Kaynak, yerel ayar ve kullanıcı değişikliklerini koru.

Ortak proje kapsamı ve etap kayıtları `StarDataParadiseServer/docs/coordination/` altındadır. Uygulanacak değişiklik için geçerli kullanıcı isteğini ve ilgili etap kaydını oku. Çalıştırılan testleri, başarısızlıkları ve eksik önkoşulları kaydet.

Port seçiminde kayıtlı kullanıcı tercihini koru. Mevcut sunucunun kalıcı yapılandırması ve ilgili bağlantı profili esas alınır; seçilmiş 3197, 3334 veya başka bir portu varsayılanla ya da geçici test portuyla değiştirme. İlk kurulumda henüz seçim yoksa varsayılan önerilebilir. Başlatıcı portu kalıcı yapılandırmadan okumalıdır; güncelleme ve yeniden başlatma bu seçimi korumalıdır. Aynı sunucu için Workbench ve worker portlarının uyumunu doğrula; izole testlerin geçici portlarını gerçek kuruluma yazma.
