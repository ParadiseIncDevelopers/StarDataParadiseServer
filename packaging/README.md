# StarDataParadise Windows paketleri

`Build-Packages.ps1` sabit WiX Toolset 5.0.2 ile Server, Workbench ve CLI için üç ayrı x64 MSI üretir. Her uygulamanın payload tarifi kendi deposunun `packaging/payload.json` dosyasındadır. Workbench ve CLI, sabit Native 0.4.1 ZIP’inden çıkarılan DLL’i kendi kurulum dizinine alır.

Bulunulan ortamda paketleri oluşturmak için derlenmiş Release klasörlerini, Native ZIP’ini, sabit MSVC CRT klasörünü ve Server bağımlılık dizinini parametre olarak ver.

```powershell
./packaging/Build-Packages.ps1 -Wix $Wix -ServerRelease $ServerRelease -WorkbenchRelease $WorkbenchRelease `
    -CliRelease $CliRelease -NativeArchive $NativeArchive -RuntimeDirectory $Crt -Dependencies $Dependencies `
    -OutputDirectory $Bundle
```

`components.json` paket ve özellik SHA-256, gerçek indirme ve kurulum baytları, bağımlılıklar, veri sahipliği ve imza politikasını taşır. Doğrulayıcı, seçilen özellikleri kendi MSI dosya tablolarından ölçer. `development` kanalındaki yerel MSI’lar imzasızdır. Üretim imza politikası yayın sertifikası izi ve geçerli Authenticode imzası gerektirir.

```powershell
Import-Module ./packaging/PackagePlan.psm1
Resolve-SdpPackagePlan -ManifestPath (Join-Path $Bundle components.json) -Profile workbench-only
Resolve-SdpPackagePlan -ManifestPath (Join-Path $Bundle components.json) -ComponentId server.deep-learning,cli.core
```

Server MSI makine kapsamındadır. Workbench ve CLI kullanıcı kapsamındadır. Her MSI tek başına açılabilir ve yalnız taşıdığı ürün dosyalarını kurar. Ek özellikler `ADDLOCAL=Core,Migration` gibi bir seçimle eklenebilir. Ham MSI kurulumunda instance yapılandırması ayrıca yönetilir. Server paketindeki `features/defaults.json` dosyası Core ayarlarını gösterir. CPU ML, CPU derin öğrenme ve Decision, kendi özellik seçimlerinin kurduğu ayar parçaları kullanılarak etkinleşir. Bu seçimler uygulama yapılandırmasını değiştirir ve mevcut database dosyalarını korur.

Workbench Migration özelliği sağlayıcı yardımcı programını kurar. Kaynak PostgreSQL veya MySQL için uyumlu üretici sürücüsü ve TLS CA ayrıca seçilir. Debugger Workbench çekirdeğindedir. Debugger belgeleri, Java export araçları ve SDK paketleri isteğe bağlı özelliklerdir. Windows x64 bu paketlerin desteklenen hedefidir. GPU bileşeni manifestte kullanılamaz durumdadır.

Lisans ve çalışma zamanı envanteri [THIRD-PARTY.md](THIRD-PARTY.md) dosyasındadır.
