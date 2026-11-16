# M9 yönetici kabulü

16 Kasım 2026 tarihinde izole Windows x64 örnekleri için Windows PowerShell 5.1 yönetici oturumunda iki kabul testi çalıştırıldı. Final komutu `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File packaging/Test-AdminIntegration.ps1` çıkış kodu 0 verdi.

| Kontrol | Sonuç | Kanıt |
| --- | --- | --- |
| Windows Service Control Manager | PASS | Kurulum tam EXE ve instance eşleşmesiyle sanal hizmet hesabını kaydetti. Başlatma ve yeniden başlatma sonrasında TLS, oturum açma, kalıcı SQL kaydı ve History doğrulandı. Hizmet kaldırıldığında veri dosyalarının SHA-256 değerleri korundu. |
| Windows Defender Firewall | PASS | Tek kuralın program, hizmet, TCP port, yerel adres ve Domain/Private profil filtreleri okundu. Tekrar yapılandırma ikinci kural üretmedi. Test kuralı ve hizmet kaydı kaldırıldı. |

İlk denemede hizmet, korumalı `admin-v1.json` dosyasını okuyamadı. Kurulum şimdi o instance’ın sanal hizmet hesabına dosya için salt okuma izni veriyor. İkinci denemede hizmet çalıştı ve TLS bağlantısı kuruldu. Son kabul, test parolasını doğrudan sürecin stdin akışına göndererek kayıtlı doğrulayıcıyla eşleşmesini denetledi.

- Kabul günlüğü `out/sdp-m9/stage45/admin-run/admin-acceptance.log`
- Günlük SHA-256 `ed024303a2ed5f72b97fb60503fc5bd67cd7ce3c64714d1c233783da080409d2`
- Test edilen Server EXE SHA-256 `927596f1bff475600a039760da3b0d8f0038ca2c038ad7eeaa3c7943694d1032`
- Düzeltilmiş Setup ZIP SHA-256 `e734b33951e66a50d9403d8e3426904aa4851724acebbb29d7ede5d4ea9ceebb`

Geliştirme paketinin içindeki üç MSI aynı SHA-256 değerlerini korudu. Çevrimdışı ZIP güncel `InstancePlan.psm1` ile yeniden üretildi.
