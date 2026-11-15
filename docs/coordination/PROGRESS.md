# İlerleme

| Etap | Durum | Kanıt |
| --- | --- | --- |
| 01 Mevcut projeler | PASSED | [Başlangıç raporu](WORKSPACE_BASELINE.md), [etap kaydı](STAGES/01.json) |
| 02 Bağımsız derleme | PASSED | [Yedi bağımsız derleme ve paket testleri](STAGES/02.json) |
| 03 Protokol | PASSED | [Wire v1, fixture ve SHA-256 pini](STAGES/03.json) |
| 04 Server yaşam döngüsü | PASSED | [Windows loopback TLS ve süreç testleri](STAGES/04.json) |
| 05 Native bağlantı ve ABI | PASSED | [Native ZIP, gerçek TLS ve ASan](STAGES/05.json) |
| 06 CLI bağlantısı | PASSED | [Gerçek TLS, JSON durum çıktısı ve terminal testleri](STAGES/06.json) |
| 07 Workbench bağlantısı | PASSED | [İki TLS oturumu, Credential Manager ve profil dışa aktarma](STAGES/07.json) |
| 08 Disk ve satır depolama | PASSED | [Storage v1, heap/BLOB ve I/O hata testleri](STAGES/08.json) |
| 09 WAL ve transaction | PASSED | [Commit, savepoint, fault enjeksiyonu ve 32 eşzamanlı transfer](STAGES/09.json) |
| 10 Recovery, checkpoint ve M2 | PASSED | [Çökme matrisi, ASan ve yedi depo regresyonu](STAGES/10.json) |

[M1 temel altyapı raporu](M1_REPORT.md) · [M2 istemci ve depolama raporu](M2_REPORT.md) · [Sıradaki etap](NEXT_TASK.md)
