# M9 Workbench ve bağımsız kurulum

M9 kabulü Windows x64 üzerinde tamamlandı. Son Server Release derlemesinde 50 Server ve 15 Workbench testi geçti. Kurulum testleri ayrı Windows Installer ürün kimlikleri ve geçici instance dizinleri kullandı.

| Etap | Doğrulama |
| --- | --- |
| [41](STAGES/41.json) | Workbench Dataset, ML ve Job Studio. Gerçek CPU eğitimi ve iki süreçli MLP devamı. |
| [42](STAGES/42.json) | Native SDK 0.4.1, SQL betiği ve yeniden eğitim zamanlaması. Workbench kapandığında Server’da yürütme, doğrulanmış backup ve restore. |
| [43](STAGES/43.json) | İki TLS Server ile şema/veri karşılaştırması ve revizyon koruması. PostgreSQL 18.1 ve MySQL 8.0.43 üzerinden kesinti sonrası doğrulanmış aktarım. |
| [44](STAGES/44.json) | Bağımsız Server, Workbench ve CLI MSI. Paket SHA-256, boyut, bağımlılık, çakışma ve uygulama yerel DLL sahipliği denetimleri. |
| [45](STAGES/45.json) | Gerçek çevrimdışı MSI kurulum, modify, repair, reconfigure, iptal, recover ve uninstall. Standard kullanıcıyla uzak Workbench ve portable açılış. |

Windows Installer akışında mevcut instance verisi, model ve History korundu. Aktif Decision kuralları, modül ve Server kaldırmayı engelledi. Kurulum öz testi yalıtılmış gerçek SQL, rollback, commit ve History doğrulaması çalıştırdı.

## Yerel çıktı

- [Windows x64 çevrimdışı Setup ZIP](../../out/sdp-m9/stage45/StarDataParadise-Setup-M9-win-x64-development.zip)
- [Taşınabilir Workbench ZIP](../../out/sdp-m9/stage45/StarDataParadise-Setup-M9-win-x64-development/Workbench-portable-win-x64.zip)
- [Kurulum ve bakım kullanımı](../../packaging/SETUP.md)
- [MSI yaşam döngüsü kaydı](../../out/sdp-m9/stage45/lifecycle-packaged.log)

Çevrimdışı ZIP SHA-256 `6aba65afa59c333b788ad0e9f093141f18a94793bf1c42a51feb206969d20242`.

Paketler imzasız geliştirme çıktılarıdır. Windows hizmeti, sanal hesap ACL ve güvenlik duvarı kontrolleri ikinci kabul turunda yönetici oturumunda doğrulandı. Düzeltilmiş çevrimdışı paketin hash’i ve test kapsamı ayrı kabul kaydındadır. [Yönetici kabulü](M9_ADMIN_ACCEPTANCE.md)

GPU ve Windows dışı paketler bu çıktıda kullanılamaz.

