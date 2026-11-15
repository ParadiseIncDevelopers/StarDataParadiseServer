# M2 istemci bağlantıları ve dayanıklı depolama

**PASSED · Windows x64 · 06–10**

| Etap | Sonuç | Kayıt |
| --- | --- | --- |
| 06 | CLI, paketlenmiş native DLL ile TLS üzerinden Server’a bağlandı. Etkileşimli komutlar, JSON durum çıktısı, sertifika ve kimlik doğrulama hataları sınandı. | [06](STAGES/06.json) |
| 07 | Workbench aynı DLL paketiyle iki bağımsız TLS oturumu açtı. Bir sekme kapatıldıktan sonra diğer oturum çalıştı. Windows Credential Manager ve sır içermeyen profil dışa aktarımı doğrulandı. | [07](STAGES/07.json) |
| 08 | Sayfa ve satır biçimi v1, heap ve BLOB zincirleri, UTF-8, decimal ve NULL tekrar açmada doğrulandı. Checksum, kısa I/O ve bozuk overflow referansı testleri geçti. | [08](STAGES/08.json) |
| 09 | WAL v1 ile dayanıklı commit, iç içe savepoint ve satır/katalog değişiklik kaydı doğrulandı. 32 eşzamanlı transfer toplam bakiyeyi korudu. | [09](STAGES/09.json) |
| 10 | Disk sayfaları ve değişiklik kaydı committed WAL’dan kurtarıldı. Checkpoint, segment rotasyonu, bozuk veriyle güvenli duruş ve byte düzeyinde idempotent replay doğrulandı. | [10](STAGES/10.json) |

06–10 arasındaki 16 zorunlu kabul kontrolü ve M1’den gelen 16 zorunlu kontrol PASS durumunda. Etaplar sırayla doğrulandı. Yedi deponun etap öncesi kaynak arşivleri ve SHA-256 değerleri, çalıştırılan komutlar, çıkış kodları, loglar ve paket hash’leri etap kayıtlarında.

## Son regresyon

| Proje | Kontrol ve sonuç |
| --- | --- |
| StarDataParadise | 2/2 CTest, Workbench başlangıcı ve gerçek TLS bağlantı akışları. [Log](../../../StarDataParadise/out/sdp-m2/regression/ctest-final.log) |
| StarDataParadiseCli | 4/4 CTest, ayrı istemci dizini, TLS, terminal ve JSON. [Log](../../../StarDataParadiseCli/out/sdp-m2/regression/ctest-final.log) |
| StarDataParadiseServer | 11/11 CTest. Storage, WAL ve recovery süitleri MSVC AddressSanitizer altında ayrıca 4/4 geçti. [Release](../../out/sdp-m2/stage10/ctest.log) · [AddressSanitizer](../../out/sdp-m2/stage10/asan-final/ctest.log) |
| StarDataParadiseDLLler | 2/2 CTest. Yayıma hazır ZIP ve AddressSanitizer test ZIP’i ayrı paket tüketicileriyle 11’er bağlantı senaryosunu geçti. [Release](../../../StarDataParadiseDLLler/out/sdp-m2/regression/integration-final.log) · [AddressSanitizer](../../../StarDataParadiseDLLler/out/sdp-m2/regression/asan-final.log) |
| StarDataParadise.SDK.NET | NuGet paketi ve assembly yükleme testi geçti. [Log](../../../StarDataParadise.SDK.NET/out/sdp-m2/regression/test.log) |
| StarDataParadise.SDK.Java | JAR paketi ve paket yükleme testi geçti. [Log](../../../StarDataParadise.SDK.Java/out/sdp-m2/regression/test.log) |
| StarDataParadise.SDK.Python | Wheel paketi, kurulum ve sürüm doğrulaması geçti. [Log](../../../StarDataParadise.SDK.Python/out/sdp-m2/regression/test.log) |

CLI, Workbench ve native DLL bağlantı testleri güncel Server ile Windows loopback üzerinde çalıştı. Visual Studio çözümleri de değiştiği etaplarda derlendi. İstemciler, M1’deki Windows x64 native ZIP’i C ABI 1 ile tüketiyor. [Paket ve SHA-256](STAGES/05.json)

## Çökme ve biçim uyumluluğu

Yedi işlem kesintisi noktasında commit ve recovery davranışı ölçüldü. Commit işareti yazılmadan kesildiğinde önceki commit korundu. WAL diske yazıldıktan sonra kesildiğinde satırlar ve değişiklik kaydı geri geldi. İkinci replay dosyaları byte düzeyinde aynı bıraktı. Küçük Release fixture’larında ölçülen recovery süreleri 1,16–29,65 ms aralığında. [Release çökme matrisi](../../out/sdp-m2/stage10/build/recovery-evidence/crash-matrix.json) · [AddressSanitizer çökme matrisi](../../out/sdp-m2/stage10/asan-final/build/recovery-evidence/crash-matrix.json)

Etap 08 disk fixture’ı ve Etap 09 disk/WAL çifti önceki biçimleriyle açıldı. Kaynak SHA-256 değerleri doğrulandı. Fiziksel güç kaybı ve disk donanım önbelleği ölçülmedi. [Fixture doğrulaması](../../out/sdp-m2/stage10/milestone-evidence.log)

M2’de tam değişiklik geçmişi WAL segmentlerini pinler. 64 MiB WAL kotası tüm segmentler için geçerlidir. CLI ve Workbench bağlantı, durum ve yetenek sorgularını destekler. SQL ve transaction işlemlerinin ağ erişimi sonraki etapların kapsamındadır. [Recovery ve checkpoint](../../RECOVERY.md) · [Uyumluluk matrisi](COMPATIBILITY_MATRIX.md) · [Sıradaki etap](NEXT_TASK.md)
