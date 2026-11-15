# M4 kabul raporu

Etap 16–20 Windows x64 üzerinde kabul edildi. M3 SQL ve güvenlik regressions aynı 0.2.0 Server ile tekrar geçti. Ayrıntılı komut, exit code, log ve artifact SHA-256 bilgileri [Etap 20 kaydında](STAGES/20.json), önceki SQL testleri [M3 raporunda](M3_REPORT.md).

| Etap | Doğrulanan davranış |
| --- | --- |
| [16](STAGES/16.json) | Tipli PL/SQL IR, caller transaction, sınırlandırılmış runtime ve kalıcı bağımlılık takibi. |
| [17](STAGES/17.json) | Named cursor sonuçları, IN/OUT, compound değerler, OLD/NEW ve trigger rollback. |
| [18](STAGES/18.json) | Canlı breakpoint, adım, salt okunur watch, duvar saati profili ve iki bağlantı ucu kapanışında rollback. |
| [19](STAGES/19.json) | Stable satır geçmişi, LOG/DIFF/READ, UTC filtresi, SHA-256 seal, güncel erişim yetkileri, kota ve retention pini. |
| [20](STAGES/20.json) | Aynı kimlikle satır ve tablo restore, routine source, bağımsız alan koruyan revert, PK/FK/şema çakışmaları ve revizyon tabanlı preview. |

| Proje | Son test |
| --- | --- |
| Server | Windows x64 CTest 26/26 ve resmi Visual Studio çözümü. İki Release derlemesinde gerçek TLS History restore. |
| Native DLL | ABI 1 için 39 export. Release ve AddressSanitizer kurulu paketlerinde 20’şer uçtan uca senaryo. |
| Workbench | Etap 20 Server ve Native 0.2.0 ile 2/2. Bağımsız iki TLS oturumu ve Credential Manager. |
| CLI | Etap 20 Server ve Native 0.2.0 ile 4/4. Paralel koşudaki bağlantı hatası kaydedildi, seri tekrar geçti. |
| .NET SDK | NuGet paketleme ve bağımsız assembly smoke. |
| Java SDK | Gradle 9.3.1 çevrimdışı JAR ve smoke. |
| Python SDK | Wheel paketleme, kurulum ve izole import smoke. |

Gerçek süreç kesintileri hem WAL marker öncesi hem durable WAL sonrasında uygulandı. Geri yükleme yanıtını alamayan TLS istemcisi yeni oturumdan önceki sonucu aldı. Sunucu yeniden başlatıldıktan sonra aynı request token yeni yazma üretmedi. Restore ve revert planları güncel izin, kısıt ve trigger’lardan geçti. PK ve collated unique değeri başka bir stable row ID kullanıyorsa mevcut satır korundu.

`PURGE` okunabilir History sınırını ilerletir. Fiziksel WAL zinciri disk kurtarma için saklanır. Tombstone kimliği geri açan yeni bir WAL işlemi için 0.2.0 veya onu destekleyen sonraki Server sürümü gerekir. Desteklenen geri yükleme türleri ve sınırlar [History belgesinde](../../HISTORY.md).

Sonraki kapsam Etap 21 Decision kural modelidir. [Sıradaki etap](NEXT_TASK.md)
