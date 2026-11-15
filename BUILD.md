# StarDataParadiseServer derleme ve test

Windows x64, Visual Studio 2026 (v145), Windows SDK 10.0.26100.0, C++20 ve CMake 4.2.2 kullanılır. Uygulamalar dinamik MSVC çalışma zamanına bağlanır. Aşağıdaki OpenSSL statik kütüphaneleri sağlayıcının `no-shared` derleme ayarlarını kullanır.

## Bağımlılıklar

| Bileşen | Sürüm | Lisans ve kaynak |
| --- | --- | --- |
| CMake | 4.2.2 | BSD-3-Clause. [Kitware](https://github.com/Kitware/CMake/tree/v4.2.2) |
| MSVC, Windows SDK | v145, 10.0.26100.0 | Microsoft lisans koşulları. [MSVC çalışma zamanı](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) |
| nlohmann/json | 3.12.0 | MIT. [Sürüm ve SHA-256](https://github.com/nlohmann/json/releases/tag/v3.12.0) |
| Asio | 1.38.2 | Boost Software License 1.0. [Sürüm](https://github.com/chriskohlhoff/asio/tree/asio-1-38-2) |
| OpenSSL | 3.5.9 | Apache-2.0. [Kaynak ve SHA-256](https://github.com/openssl/openssl/releases/tag/openssl-3.5.9) |
| Python | 3.12+ | PSF. Sözleşme ve loopback testleri için. |

OpenSSL 3.5.9 x64 geliştirme paketinin `include/`, `lib/` ve `bin/openssl.exe` dizin/dosyalarını hazırla. Örneğin doğrulanmış OpenSSL kaynak arşivinde x64 Native Tools konsolu ve Windows Perl ile:

```bat
perl Configure VC-WIN64A no-shared no-asm no-tests no-module --prefix="C:\deps\openssl-3.5.9" --openssldir="C:\deps\openssl-3.5.9\ssl"
nmake
nmake install_sw
```

## CMake

```powershell
cmake --preset windows-x64 -DOPENSSL_ROOT_DIR="C:/deps/openssl-3.5.9"
cmake --build --preset windows-x64
ctest --preset windows-x64
```

Yerel CI için `powershell -NoProfile -File .\ci.ps1 -OpenSslRoot "C:/deps/openssl-3.5.9"` kullan. CMake Asio ve JSON arşivlerinin SHA-256 değerini doğrular. Çevrimdışı derlemede doğrulanmış dosyaları `-DSDP_JSON_ARCHIVE=...` ve `-DSDP_ASIO_ARCHIVE=...` ile geçir. Test `127.0.0.1` üzerinde geçici portlar ve ayrı bir Windows konsolu kullanır. EXE `out/build/windows-x64/Release/` altındadır.

## Mevcut Visual Studio çözümü

`SDP_JSON_ROOT` için nlohmann/json arşiv kökünü, `SDP_ASIO_ROOT` için Asio arşiv kökünü ve `OPENSSL_ROOT_DIR` için OpenSSL kurulum kökünü Visual Studio ortamına tanıt. `StarDataParadiseServer.slnx` içinde x64 seç. VS ve CMake aynı kaynakları ayrı çıktı dizinlerinde derler.

Yerel çalıştırma, ilk yönetici ve TLS ayarları [RUN.md](RUN.md) dosyasında.
