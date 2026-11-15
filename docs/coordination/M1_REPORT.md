# M1 temel altyapı raporu

**Durum: PASSED (Windows x64)**

| Etap | Sonuç | Kayıt |
| --- | --- | --- |
| 01 | Dört mevcut Git deposu envantere alındı. 37 kaynak ve yerel ayar dosyasının kopyası SHA-256 ile doğrulandı. | [01](STAGES/01.json) |
| 02 | Dört C++ hedefi kendi kökünden derlendi. Üç yeni yerel SDK deposu NuGet, JAR ve wheel üretti. | [02](STAGES/02.json) |
| 03 | Wire v1 golden verileri, negatif çerçeve testleri, tekrar üretilebilir arşiv ve checksum pini doğrulandı. | [03](STAGES/03.json) |
| 04 | TLS ve auth, dosya/port sahipliği, Ctrl+C, aynı portta yeniden başlatma ve log redaksiyonu doğrulandı. | [04](STAGES/04.json) |
| 05 | Kurulan DLL ZIP paketini tüketen ayrı CMake projesi gerçek Server’a bağlandı. Native 11 senaryo Release ve MSVC AddressSanitizer altında geçti. | [05](STAGES/05.json) |

## Son regresyon

| Proje | Çalıştırılan kontroller | Sonuç |
| --- | --- | --- |
| Workbench | Qt disconnected pencere | PASS |
| CLI | Yardım, sürüm, geçersiz seçenek | PASS |
| Server | 7 CTest, wire sözleşmesi ve izole TLS yaşam döngüsü dahil | PASS |
| Native DLL | 2 CTest ve 11 ayrı paket-tüketicisi senaryosu | PASS |
| .NET SDK | Yerel NuGet paketinden assembly yükleme | PASS |
| Java SDK | Üretilen JAR’ın yüklenmesi ve manifest sürümü | PASS |
| Python SDK | Wheel kurulumu ve paket metadata sürümü | PASS |
| Yedi repo kaynak izolasyonu | Build öncesi ve sonrası 123 dosyanın SHA-256 karşılaştırması | PASS |

## Yerel çıktılar

| Depo | Çıktı |
| --- | --- |
| StarDataParadiseDLLler | `out/sdp-m1/stage05/package/StarDataParadiseDLLler-0.1.0-win-x64.zip`  (C ABI 1, DLL, import library, header ve CMake config) |
| StarDataParadiseServer | `out/sdp-m1/stage04/build/Release/StarDataParadiseServer.exe` |
| StarDataParadiseServer | `out/sdp-m1/stage03/build/contracts/sdp-contract-v1.zip` |
| StarDataParadise.SDK.NET | `out/packages/StarDataParadise.SDK.NET.0.1.0.nupkg` |
| StarDataParadise.SDK.Java | `out/build/libs/StarDataParadise.SDK.Java-0.1.0.jar` |
| StarDataParadise.SDK.Python | `out/packages/stardataparadise_sdk-0.1.0-py3-none-any.whl` |

Native ZIP SHA-256 `9a9e7409c3670e38c1a54bca1bcdc2b9cf9e3f68c87286500c745bdbea1cfd4c`. Test komutları, log hash’leri ve ürün SHA-256 değerleri etap kayıtlarında. Yedi depo ayrı Git ve derleme köklerine sahip. Var olan Visual Studio çözüm dosyaları korunup yeni hedeflerle derlendi. SDK depoları yerel Git başlangıcı ile oluşturuldu.

Native TLS bağlantısı için PEM CA paketi ve sertifikanın doğrulanacak DNS/IP adı açık girdi olarak verilir. CLI ve Workbench’in Server bağlantısı 06 ve 07 kapsamındadır. SQL ve diğer uygulama özellikleri capability matrisinde unavailable. [Uyumluluk matrisi](COMPATIBILITY_MATRIX.md)

Son regresyon sırasında yeniden paketlenen SDK çıktıları için güncel SHA-256 değerleri [Etap 05 kaydında](STAGES/05.json). Etap 02 kaydındaki değerler o etapta üretilen paketleri gösterir.
