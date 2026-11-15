# StarDataParadise 31–36 kabulü

Windows x64 üzerinde Sunucu 0.4.0 CMake ve Visual Studio ile derlendi.
Sunucu CTest 47/47 geçti. Kurulu Native SDK, iki .NET paketi, JDBC, Python ve
Node.js paketleri gerçek 0.4.0 TLS sunucusunda tekrar çalıştırıldı. Mevcut
Workbench pencere ve iki TLS oturumu testlerinden geçti.

| Etap | Sonuç |
| --- | --- |
| [31 Native SDK](STAGES/31.json) | 0.4.0 Windows x64, ABI 1, kurulu C ve C++ tüketicileri ve ASan |
| [32 .NET SDK](STAGES/32.json) | 2.0.0-preview.1, .NET 10 ve .NET Framework 4.8, ayrı temiz NuGet kurulumları |
| [33 JDBC](STAGES/33.json) | 0.2.0, JDK 17, Gradle 9.3.1 ve kurulu Maven JAR |
| [34 Python](STAGES/34.json) | 0.2.0, temiz wheel ve sdist, dokuz canlı TLS testi |
| [Node.js SDK](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Node/releases/tag/v0.1.0) | 0.1.0, Node.js 22, temiz npm tarball kurulumu |
| [35 Java export](STAGES/35.json) | Tipli IR, 21 standalone karşılaştırma, bağlı JDBC ve immutable sürüm denetimi |
| [36 CLI](STAGES/36.json) | 0.2.0, Native 0.4.0, çok satırlı SQL/PLSQL ve gerçek otomasyon testleri |

## Paketler

İki .NET paketi eski paket kimlikleriyle üretildi. 2.0 ön sürümü ADO.NET
API’sini ve yeni çalışma zamanı hedeflerini içerir. Yayın adaylarında
derleme makinesinin yolunu taşıyan debug kaydı kapatıldı, iki paket temiz
ortamda yeniden kuruldu ve test edildi.

| Paket | SHA-256 |
| --- | --- |
| StarDataParadiseDrive 2.0.0-preview.1 | `b8396597bb4edfa7226e6d3993b7206e1d018753bdf6438d37397c664d7b046c` |
| StarDataParadiseNetFramework 2.0.0-preview.1 | `d1d1925c1ce572a18a26874b4b732930faf450d30fa87d7e297bb32bf8ce3913` |
| stardataparadise-sdk 0.1.0 | `5831f4e456e397bdaf3c477dad160477bd8ba2e23eeafb80ee203dee2e104ad6` |
| StarDataParadiseServer 0.4.0 Windows x64 | `14626931425aeaf57eab3aa4831bb4b22e88be8987bf1d2158f50c9a7ebb7896` |

## Kaynak ve derleme

Sunucu yedekleme uygulamasının iki kaynak dosyası eski Git dışlama
kuralı nedeniyle v0.3.0 kaynak etiketinde eksikti. v0.3.0 çalıştırılabilir
dosyası bu kaynaklarla derlendi ve Windows servis testinden geçti.
v0.4.0 kaynakları gerekli dosyaları ve Git istisnasını içerir.

Java örneği sınırlı taş ve konum puanı ile ayrı bir arama yordamı
içerir. Bağlı üretim, SQL ve rutin yürütmesini Sunucuda bırakır.
Bağımsız Java üretimini denetlemek ve yeniden oluşturmak için kılavuzu
kullanın. [PL/SQL Java export](../../JAVA_EXPORT.md)
