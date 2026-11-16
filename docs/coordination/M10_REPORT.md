# M10 Windows x64 teslim raporu

16 Kasım 2026’da yedi bağımsız kaynak görüntüsü alındı. Kaynak arşivlerinin Git HEAD değeri, yakalama anındaki çalışma ağacı durumu ve SHA‑256 değeri aday manifestinde yer alır. Derleme bu görüntülerden yapıldı. Bu rapor ve 50. etap sonuç kaydı, yakalamadan sonra oluşturuldu. Arşivler derleme girdilerinin zamanını korur.

## Adaylar ve test kapsamı

| Kontrol | Kullanılan aday ve kanıt |
| --- | --- |
| 49. etap Windows x64 kabulü | Çevrimdışı Setup ZIP `a67b97d0cc712452cde70a33ee9241b17937d3b42795263ae74d0725f2d4872f`. Kurulum matrisi, çevrimdışı uçtan uca senaryo, altı güncelleme kesintisi, iş boşaltma ve kurtarma geçti. [49. etap kabulü](S49_ACCEPTANCE.md) |
| 50. etap temiz Release | Server 50, Workbench 16, CLI 8 ve Native 2 CTest geçti. Kurulmuş Native SDK tüketicileri canlı TLS üzerinde geçti. |
| 50. etap SDK kabulü | Modern .NET, .NET Framework 4.8, JDBC ve Python paketleri final Server EXE’siyle TLS üzerinden doğrulandı. Native ABI ve wire protokolü sürüm 1 olarak kaldı. |
| 50. etap paket ve lisans denetimi | Üç bağımsız MSI, taşınabilir ZIP’ler ve çevrimdışı Setup, temiz Release çıktılarından üretildi. Denetim 37 artefakt, 26 SPDX paket kaydı ve 737 metin girdisi kapsadı. Kimlik bilgisi taraması 0 bulgu verdi. |
| 50. etap temiz konuk kabulü | Yeniden oluşturulan MSI ve Setup dosyalarının uçtan uca sonucu [50. etap kaydında](STAGES/50.json) tutulur. |

## Teslimin sınırları

49. etapta Windows uzun yol desteği kapalıyken 261 karakterlik geçici WAL yolu açılışı durdurdu. Aynı imzalı güncelleme kısa mutlak yollarla doğrulandı. Temiz Windows’ta Server çalışma zamanı DLL’leri uygulama dizininden yüklenir. MSI veya tüm çalışma zamanı dosyalarını içeren binary ZIP kullanılabilir. CPU eğitim kapsamı, GPU sınırları ve imzasız geliştirme kanalı ayrı ayrı belgelenmiştir. [Bilinen sınırlamalar](KNOWN_LIMITATIONS.md)

Kurulum, bağlantı ve SQL, PL/SQL, Decision, History, CPU ML ve Java export örnekleri çevrimdışı hızlı başlangıç belgesindedir. [Türkçe hızlı başlangıç](S50_QUICKSTART.md)


## Nihai teslim

50. etap Setup ZIP SHA‑256 değeri `c7696aae32959045e9ad4f68760dd320f7b73a1a5905e538b6efdbd051862344`. Kurulum matrisi ve uçtan uca test Microsoft Windows 11 Enterprise LTSC Evaluation x64, build 26100 üzerinde çıkış kodu 0 verdi. Kurulumdan sonra Server, CLI ve Workbench çalıştırılabilir dosyalarının SHA‑256 değerleri teslim manifestiyle eşleşti.

Temiz Windows görüntüsü ile 49. ve 50. etap kabul anlık görüntüleri `SDP-QA49-W11` sanal makinesinde saklanıyor. Sanal makine güvenli biçimde kapatıldı.

İlk paket birleştirmesi Setup kanalının ve imza politikasının farklı değerlerini eşit saydığı için durdu. Kontrol düzeltildi ve değişmeyen kurulum paketleriyle yeniden çalıştırıldığında geçti.

Standart kullanıcıda Install, Portable, Uninstall işlemleri tamamlandı. Yönetici matrisinde Install, Modify, Portable, Reconfigure, Recover, Repair, Uninstall işlemleri tamamlandı. ZIP ile açılmış teslim dizinindeki 121 dosya karşılaştırıldı.

- [Yerel Windows x64 teslim ZIP’i](../../out/sdp-m10/stage50/delivery/StarDataParadise-M10-win-x64.zip)
- [Teslim manifesti](../../out/sdp-m10/stage50/delivery/StarDataParadise-M10/release-manifest.json)
- Arşiv SHA‑256 `4c21bdd3533bc0f444d799702099e08fdfd5b73556b7d3a89eab3f68c698f790`
