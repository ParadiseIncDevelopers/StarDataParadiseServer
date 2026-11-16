# Çevrimdışı Windows kurulumu

Arşivi yeni bir klasöre çıkar. Paket, üç ayrı x64 MSI ve ortak Setup sihirbazı içerir. İmzasız geliştirme paketlerini kullanırken sihirbazdan bu seçimi onayla veya komuta `-AllowDevelopment` ekle.

```powershell
pwsh -NoProfile -STA -File ./Setup.ps1 -Profile workbench-only -AllowDevelopment
pwsh -NoProfile -File ./Setup.ps1 -Profile workbench-only -AllowDevelopment -Silent
```

Workbench veya CLI tek başına kullanıcı kapsamına kurulur. Uzak Server TLS profili uygulamadan girilir. Bir Server instance’ı hazırlamak istediğinde konum, benzersiz isim, TLS sertifikası ve özel anahtar dosyalarını seç. Program ve veri dizinleri ayrıdır. WAL ve modeller veri dizini altında saklanır. Yerel Windows hizmeti ve tekil firewall kuralı seçilirse işletim sistemi yönetici yetkisi gerekir. Windows hizmeti kendi sanal hizmet hesabıyla kaydedilir ve başlatma ayrı yaşam döngüsü adımıdır.

Modify özellik ekleyip kaldırır. Repair kayıtlı MSI dosyalarını onarır. Reconfigure, aynı Server verisini koruyarak instance ayarlarını günceller. Yüklü üründe uninstall yalnızca ilgili MSI ve bu sihirbazın sahip olduğu doğrulanmış entegrasyonlara uygulanır. Server verisi, WAL, modeller, History ve backup dizinleri korunur. Aktif Decision kuralları varken Decision özelliği kaldırılmaz. Aday ayarların denetlenmesi için ilgili instance’ın verisini kullanan süreç kapalı olmalıdır.

Taşınabilir Workbench’i yeni bir konuma açmak için şunu çalıştır.

```powershell
pwsh -NoProfile -File ./Setup.ps1 -Operation Portable -PortableDirectory 'C:/Apps/SDP-Portable' -AllowDevelopment -Silent
```

`-ResponseFile` JSON dosyası `schema: 1`, `operation`, `profile` veya `componentIds` ve gerektiğinde `installRoot`, `stateDirectory`, `instance` ve `allowDevelopment` alanlarını alır. Yeni instance parolasını etkileşimli olarak gir veya PowerShell çağrısında bir `SecureString` olarak sağla. Parola, komut satırı metnine, response dosyasına ve kurulum günlüklerine yazılmaz.

Başarı çıkış kodu `0`, yeniden başlatma gerektiğinde `3010`, iptalde `1602`, doğrulama veya işlem hatasında `2` döner. Bir işlem kesildiyse aynı paket seçimi ve setup durum diziniyle `-Operation Recover` çalıştır. Kurulumun hangi MSI adımlarını geri aldığı durum günlüklerinde görünür.
