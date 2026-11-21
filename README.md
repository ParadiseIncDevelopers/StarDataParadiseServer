# StarDataParadise Server

StarDataParadise Workbench, CLI ve uygulama SDK’larının bağlandığı veritabanı sunucusu. Bu depo, Windows x64 wire v1 motorunu ve MySQL 26.10.0 tabanlı Classic motorunun kaynak yamasını, uyumluluk servislerini ve kabul testlerini içerir.

Güncel ürün sürümü **0.5.1**, kanal **stable** olur. Yerel AI worker supervisor,
güvenilir kullanıcı/sağlayıcı bağları ve kullanıcı onayıyla Windows oturum
başlangıcı desteği içerir. Classic taban sürümü
26.10.0 olarak korunur. AI/Knowledge, kullanıcıya ait OpenAI/Claude anahtarı
veya içe aktarılmış yerel GGUF modeliyle çalışır; SDK'lar ayrı native SQL
bağlantısı sunar. [Sürüm bilgisi](release.json) · [AI kurulumu](compatibility/ai/README.md)
· [Native SQL aktarımı](contracts/sql/native-sql-v1.md).
HIST/Git ve yeni SQL JOB özellikleri kullanıcı kararıyla sonraki etaba ertelendi.

| Sunucu yolu | İstemci uç noktası | Kılavuz |
| --- | --- | --- |
| Windows wire v1 motoru (`StarDataParadiseServer.exe`) | Varsayılan `127.0.0.1:7443`, TLS | [Yerel çalıştırma](RUN.md) ve [Windows kurulumu](packaging/SETUP.md) |
| MySQL 26.10.0 tabanlı Classic motoru (`mysqld.exe`) | Yapılandırılan MySQL Classic TCP portu | [Kaynak yaması](compatibility/upstream/README.md) ve [geçiş durumu](docs/coordination/MYSQL_MIGRATION.md) |
| MySQL tabanı için wire v1 uyumluluk servisi | Yerel TLS dinleyicisi. Örnek komutta `7401` | [Servis, yetenekler ve doğrulama](compatibility/wire_v1/README.md) |

Classic motoru için `3197` örnek portunda ortak loopback dinleyicisi yapılandırabilirsiniz. Bu kipte Classic Workbench ve StarDataParadiseDrive 2.1.0 aynı örneğe bağlanabilir. .NET bağlantısı `TLS Mode=Disabled` ile şifresizdir ve loopback ile sınırlıdır. SDK özelliği seçerken [uyumluluk matrisine](docs/coordination/COMPATIBILITY_MATRIX.md) ve bağlı sunucunun capability yanıtına bakın. [MySQL uyumluluk kapsamı](compatibility/README.md)

## Windows wire v1 ile ilk bağlantı

1. Çevrimdışı Windows paketini kullanmak için [kurulum kılavuzunu](packaging/SETUP.md) izleyin. Kaynaktan derlemek için aşağıdaki CMake adımlarını kullanın.
2. İzole bir test örneği için ayrı veri, log ve geçici dizinler açın. TLS sertifikası, özel anahtar ve yolları içeren `server.json` dosyasını [yerel çalışma örneğine](RUN.md) göre hazırlayın. Bağlanırken kullanılacak ad sertifikanın SAN alanında bulunmalı.
3. EXE dizininden ilk yöneticiyi oluşturup sunucuyu başlatın. Bootstrap parolayı iki kez ister.

```powershell
.\StarDataParadiseServer.exe --config C:\SDP\lab\server.json --bootstrap-admin local-admin
.\StarDataParadiseServer.exe --config C:\SDP\lab\server.json
```

`LISTENING` satırında bildirilen portla, ayrı bir konsolda wire v1 CLI dizininden bağlantıyı sınayın. İstemciye sunucuyu doğrulayan CA PEM dosyasını verin.

```powershell
.\StarDataParadiseCLI.exe --host localhost --port 7443 --user local-admin --ca C:\SDP\lab\ca.pem --status
```

[SQL, PL/SQL, History ve model örnekleri](docs/coordination/S50_QUICKSTART.md) · [Windows hizmeti ve sağlık](ADMIN.md) · [Yedekleme ve geri yükleme](BACKUP.md)

## Windows wire v1 motorunu derleme

Gereksinimler CMake 4.2.2, Visual Studio 2026 C++ (v145), Windows SDK 10.0.26100.0, statik OpenSSL 3.5.9 x64 ve entegrasyon testleri için Python 3.12+.

```powershell
cmake --preset windows-x64 -DOPENSSL_ROOT_DIR="C:/deps/openssl-3.5.9"
cmake --build --preset windows-x64
ctest --preset windows-x64
```

Çalıştırılabilir dosya `out/build/windows-x64/Release/StarDataParadiseServer.exe` yolundadır. [OpenSSL, çevrimdışı bağımlılıklar ve Visual Studio çözümü](BUILD.md) · [MSI paketleme](packaging/README.md) · [Sürüm notları](https://github.com/ParadiseIncDevelopers/StarDataParadiseServer/releases)

## İstemciler ve SDK’lar

| Bileşen | Kullanım |
| --- | --- |
| [Workbench](https://github.com/ParadiseIncDevelopers/StarDataParadise) | Windows SQL editörü, tablo araçları ve yönetim |
| [CLI](https://github.com/ParadiseIncDevelopers/StarDataParadiseCli) | Terminal, SQL betikleri ve yapılandırılmış otomasyon çıktısı |
| [Native SDK](https://github.com/ParadiseIncDevelopers/StarDataParadiseDLLler) | Windows x64 C ABI 1 ve C++ sarmalayıcıları |
| [.NET SDK](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.NET) | ADO.NET, .NET 8/10 ve .NET Framework 4.8 |
| [Java SDK](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Java) | Java 17 JDBC sürücüsü |
| [Python SDK](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Python) | Python 3.12+ DB-API 2.0 |
| [Node.js SDK](https://github.com/ParadiseIncDevelopers/StarDataParadise.SDK.Node) | Node.js 22+ asenkron ESM istemcisi |

[Wire v1 sözleşmesi](contracts/v1/PROTOCOL.md) · [SQL RPC](contracts/sql/SQL_RPC_V1.md) · [İşlemler ve WAL](TRANSACTIONS_AND_WAL.md) · [Bilinen sınırlamalar](docs/coordination/KNOWN_LIMITATIONS.md)
