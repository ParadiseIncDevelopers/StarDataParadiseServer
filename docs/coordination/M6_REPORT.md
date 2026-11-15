# M6 kabul raporu

Etap 21–30 tamamlandı. [Etap 30 kabul kaydı](STAGES/30.json) komutları, test sonuçlarını ve artifact SHA-256 değerlerini içerir.

| Bileşen | Windows x64 sonucu |
| --- | --- |
| Server 0.3.0 | 47/47 CTest, Visual Studio Release ve tam/minimal TLS testleri |
| Native SDK 0.3.0 | 2/2 CTest, Visual Studio Release ve 24 kurulu paket çalışma zamanı kontrolü |
| Workbench ve CLI | Mevcut kaynaklardan Release derleme, Workbench 2/2 ve CLI 4/4 |
| .NET, Java ve Python SDK | Ayrı yerel paket ve smoke testleri |

Gerçek SCM testinde ayrı Windows hizmet hesaplarıyla iki instance kuruldu. İlk hizmet durdurulup kaldırıldığında ikinci hizmet aynı PID ve SQL verisiyle yanıt verdi. İki geçici hizmet ve test dizinleri temizlendi. Health Center başlangıçta UNKNOWN döndürdü ve gerçek ölçümden sonra güncellendi. Config önceki dosyayı korudu. Doğrulanmış backup, destek paketi redaction ve üç bakım işi geçti.

GPU eğitimi ve PITR kapalıdır. Linux ve macOS çalışma zamanı test edilmedi.
