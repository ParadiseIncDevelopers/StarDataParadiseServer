# 49. etap Windows x64 kabul raporu

Windows 11 Enterprise LTSC 2024 Evaluation x64, 10.0.26100.1742, VirtualBox 7.2.22. Ürün aktarımından önce Clean-Windows11-x64 disk görüntüsü, kabul sonrasında Stage49-Accepted görüntüsü alındı.

| Test | Sonuç | Doğrulanan akış |
| --- | --- | --- |
| S49-T01 | PASS | Bağımsız MSI, ortak Setup, sessiz ve çevrimdışı kurulum, standart kullanıcı, Portable Workbench, iki Server örneği ve ayrı uzak TLS oturumları. Modül ekleme ve etkin Decision kuralını koruyan kaldırma denetlendi. |
| S49-T02 | PASS | MSI kurulum kesintisi, doğrulama hatası sonrası geri alma, onarım, yeniden yapılandırma ve kaldırma. Server 0.3.0 kaynak verisinin karmaları korundu. Altı güncelleme kesintisi aynı günlükten devam etti. Kuyruk ve çalışan iş boşaltma, geri dönüş ve ayrı örneğe yedek geri yükleme geçti. |
| S49-T03 | PASS | Kurulu Python SDK ve CLI SQL ve PL/SQL çalıştırdı. Decision yasaklı satırı reddetti. History ile 32 satır geri alındı. Dışa aktarılan Java yordamı ve CPU model tahmini 58 üretti. Workbench normal kapandı ve yedek ayrı örnekte doğrulandı. |

Temiz VM’de Portable Workbench’in geçici klasör taşıması erişim hatası verdi. Yükleyiciye kısa bir yeniden deneme eklendi. Hedefin üzerine yazmayan atomik taşıma korundu. Aynı MSI dosyalarından üretilen ortak Setup paketiyle standart kullanıcı ve yönetici matrisleri geçti.

İlk güncelleme denemesinde geçici WAL yolu 261 karaktere ulaştı. Windows uzun yol ayarı kapalıydı. Aynı imzalı güncelleme kısa mutlak test yollarıyla geçti. Ham Server EXE’leri temiz Windows arama yolunda MSVC runtime gerektirdi. Paketli doğrulamada app-local runtime kullanıldı.

[Kurulum matrisi kanıtı](../../out/sdp-m10/stage49/guest-evidence/attempt-cb0db90e937642f69983eec9a0202ef9-676dc904.zip) · [Güncelleme kaydı](../../out/sdp-m10/stage49/guest-evidence/update-short-b/upgrade.stdout.log) · [İş boşaltma kaydı](../../out/sdp-m10/stage49/guest-evidence/update-short-b/job-drain.stdout.log) · [Etap kaydı](STAGES/49.json)
