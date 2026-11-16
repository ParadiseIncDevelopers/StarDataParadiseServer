# Windows x64 paket bağımlılıkları

Bu MSI’lar imzasız geliştirme çıktılarıdır. WiX Toolset 5.0.2, sürüm kimliği `5.0.2+aa65968c`, MSI üretiminde kullanılır. Uygulamalar WiX’i çalışma zamanında yüklemez. [WiX 5.0.2](https://github.com/wixtoolset/wix/releases/tag/v5.0.2) [Microsoft Reciprocal License](https://github.com/wixtoolset/wix/blob/v5.0.2/LICENSE.TXT)

Server ve Native istemci TLS için OpenSSL 3.5.9, JSON için nlohmann/json 3.12.0 kullanır. Server ağ erişiminde Asio 1.38.2 kullanır. Paketlerde ilgili lisans metinleri yer alır. [OpenSSL](https://github.com/openssl/openssl/tree/openssl-3.5.9) [nlohmann/json](https://github.com/nlohmann/json/tree/v3.12.0) [Asio](https://github.com/chriskohlhoff/asio/tree/asio-1-38-2)

Workbench Qt Base 6.11.3’ü dinamik yükler. Ekteki SPDX envanteri dağıtılan Qt tabanını tanımlar. [Qt lisansları](https://doc.qt.io/qt-6.11/licensing.html) [Qt Base kaynak arşivi](https://download.qt.io/official_releases/qt/6.11/6.11.3/submodules/)

MSVC v145 CRT, sürüm 14.51.36231, her ürüne kendi çalışma dizininde kopyalanır. Windows UCRT işletim sistemi bileşenidir. Qt’nin Direct3D bağımlılıkları Workbench dağıtımındadır. [MSVC uygulama yerel dağıtımı](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)

PostgreSQL ve MySQL sürücüleri operatörün sağladığı isteğe bağlı kaynak bağımlılıklarıdır. SDK arşivleri ayrı sürümlerdir. Java ve Python aktarım araçları seçildiğinde ilgili yerel çalışma zamanı kullanılır.
