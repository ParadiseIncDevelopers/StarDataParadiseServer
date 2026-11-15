# M5 kabul raporu

Etap 21–25 Decision, kalıcı jobs, dataset ve denetimli CPU model kapsamını tamamladı. Test komutları, çıkış kodları, artifact ve SHA-256 değerleri [Etap 25 kabul kaydında](STAGES/25.json).

| Etap | Gerçek kabul kanıtı |
| --- | --- |
| [21](STAGES/21.json) | Ödenmemiş sipariş için SQL, PL/SQL, trigger ve History restore ortak kuralı uyguladı. |
| [22](STAGES/22.json) | WHYNOT değerlendirilen ve atlanan düğümleri gösterdi. WHATIF veri ve WAL dosyalarını değiştirmedi. |
| [23](STAGES/23.json) | TLS istemcisi kapandıktan sonra worker sürdü. İş, restart ve kota senaryoları SQL hizmetini korudu. |
| [24](STAGES/24.json) | Kaynak değişse bile dataset snapshot sabit kaldı. Train-only fit, typed job ve güncel erişim hakları doğrulandı. |
| [25](STAGES/25.json) | Linear, logistic, CART ve forest gerçek fit ve ayrık metrik üretti. Worker immutable version yayımladı ve restart doğru version ile tahmin etti. |

Server 37 Windows x64 CTest ve bağımsız Visual Studio Release derlemesini geçti. Native ABI 1 kurulu paket regresyonu, Workbench 2/2, CLI 4/4 ve .NET, Java, Python ayrı paket testleri geçti. Var olan Workbench ve CLI kullanıcı kaynakları korundu. Tüketici senaryoları, mevcut uyumlu uçları son M5 Server ve Native paketle çalıştırdı.

[Kural ve Decision sözleşmesi](../../DECISION.md) · [Jobs](../../JOBS.md) · [Dataset](../../DATASET.md) · [Modeller](../../MODELS.md)
