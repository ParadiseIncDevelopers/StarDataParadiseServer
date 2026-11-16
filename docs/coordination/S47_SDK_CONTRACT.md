# Etap 47 SDK sözleşmesi

Testler Windows x64’te yeni veri dizini, anlık TLS sertifikası ve her çalıştırma için ayrı kimlik bilgileriyle yürütüldü. Server, CLI ve Workbench ayrı SHA-256 doğrulanmış kaynak kopyalarından derlendi. Native, .NET, JDBC ve Python kendi paketleme giriş noktalarından derlendi ve paketlerinden yüklenen tüketicilerle çalıştırıldı. [Paket ve kaynak karmaları](S47_RELEASE_MANIFEST.json)

## Ortak sonuçlar

Aynı seed SQL kurulumu dört SDK’ya yüksek hassasiyetli DECIMAL, NULL, UTC öncesi ve saat dilimi ofsetli zaman, NUL içeren metin ve BLOB döndürdü. Her tüketici tipli CRUD, PL/SQL OUT ve named result set, transaction/savepoint, kimlik doğrulama ve Decision reddini sınadı. Decision hatası 1006 / SQLSTATE 42501 verdi ve reddedilen satır kalıcılaşmadı. Aynı tablonun History gözlemi 2, aynı etkin model girdisinin tahmini 58 oldu. SDK'lar History değerini kendi destekledikleri zaman noktasından okuma veya geçmiş olaylarını sayma API’siyle elde etti.

- Başarılı commit’in yanıtı kesildi. İlk işlemin token ve LSN değeri gerçek Server defterinden uzlaştırıldı ve yazılan satır doğrulandı.
- Test yardımcı programı, kapalı ve boş bir test store’una geçmiş tarihli kalıcı defter girdisi üretti. Server gerçek saatle açıldığında dört SDK süresi geçmiş sonuç için unknown gördü.
- .NET fiziksel havuzdan alınan oturumun temizliğini ve taze bağlantıyla okuma kurtarmasını sınadı. JDBC ve Native açık oturumu sıfırladı ve okuma kesintisinden bağlantıyı yeniden açarak döndü. Python uygun okumada otomatik tekrar ve bağlantı kapanışında açık işlemin rollback davranışını sınadı.
- Wire major 2 gerçek TLS Server’da 1003 hatasıyla reddedildi. Native paket testi ABI uyuşmazlığının ayrı hatasını sınadı. Workbench çalışma zamanı Qt 6.11.3 ve uygulama yerel DLL’leriyle bağımsız MSI payload’ından doğrulandı.

## Uyumlu paketler

| Paket | Güncel Server karşısındaki kapsam |
| --- | --- |
| Native 0.4.1 temiz ZIP ve önceki 0.4.0 ZIP | C ABI 1, wire v1 ve tam ortak fixture. |
| Native 0.1.0 | TLS, auth, capabilities ve health. |
| StarDataParadiseDrive ve StarDataParadiseNetFramework 2.0.0-preview.1 | Temiz .NET 10 ve .NET Framework 4.8 paketleri tam ortak fixture’da çalıştı. Saklanan NuGet yükleme girdileri ile NuGet.org’dan indirilen depo imzalı dosyalar ayrı ayrı kuruldu. |
| JDBC 0.2.0 | Temiz Maven JAR ve önceden yayımlanan JAR ile JDK 17 üzerinde gerçek TLS, SQL ve işlem sonucu. |
| Python 0.2.0 | Temiz wheel ve önceden yayımlanan wheel ile tam ortak fixture. sdist ayrıca temiz ortamda kuruldu. |
| Eski .NET/Java/Python 0.1.0 | Baz paketler sürüm bilgisini sağlar. Mevcut veritabanı bağlantısı sözleşmesi daha sonraki paketlerde başlar. |
| Önceki CLI 0.2.0 EXE ve uygulama yerel SDK | TLS, DNS, profil, JSON, SQL, kimlik doğrulama reddi ve bağlantı kaybı regresyonu. |

CLI’nin temiz CMake derlemesi Native 0.4.0 pinini kullanır. MSI tarifi her istemcinin kendi kurulum dizinine Native 0.4.1 koyar. Workbench temiz derlemesi ve MSI’sı Native 0.4.1 kullanır. İstemci sözleşmelerinin pinlediği wire v1 arşivi ve çalışma zamanı SHA-256 değerleri manifestte yer alır.

İlk Python tüketici uyumsuzluğu testte OUT küme adını ilk satırdan önce okumaktan kaynaklandı. Test gerçek yüklenmiş paketin ilk fetch sırasında doldurduğu metadata’yı sınayacak şekilde düzeltildi. Uzun Windows geçici dizininde sdist→wheel paketlemesi durdu. Aynı kaynak ve sabit girdilerle kısa geçici dizinde yeniden üretim ve bağımsız kurulum geçti. JDBC okuma kesintisi adaptöründe TLS proxy için fixture zaman aşımı düzeltildi ve taze bağlantı doğrulaması eklendi. Bu değişiklikler test düzeneğine ve çalıştırma ayarlarına uygulandı.

Tekrar çalıştırma için test giriş noktaları [ortak fixture](../../tests/cross_sdk_fixture.py), [Native ve Python](../../tests/sdk_contract/run_native_python.ps1), [yönetilen SDK derleme](../../tests/sdk_contract/build_managed.ps1) ve [yönetilen SDK tüketimi](../../tests/sdk_contract/run_managed.ps1). Gerçek komutlar, exit code ve eşleşen günlükler [etap kaydında](STAGES/47.json) saklanır.
