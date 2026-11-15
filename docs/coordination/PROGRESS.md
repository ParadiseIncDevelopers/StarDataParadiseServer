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
| 11 Katalog, SQL parser ve tip denetimi | PASSED | [Kalıcı DDL, typed plan, NULL/decimal ve SQL denetleyici](STAGES/11.json) |
| 12 Gerçek CRUD, parametre bağlama ve sonuç akışı | PASSED | [Native ABI 1 üzerinden tipli CRUD, WAL ledger ve bounded fetch](STAGES/12.json) |
| 13 İndeksler, ilişkisel bütünlük ve güvenli DDL | PASSED | [Kalıcı B+Tree, constraint yarışları ve DDL History](STAGES/13.json) |
| 14 İleri sorgular, optimizer, sıralama ve bakım | PASSED | [JOIN/aggregate, Türkçe collation, EXPLAIN ve kota](STAGES/14.json) |
| 15 Kullanıcı, rol, oturum ve standart SQL yetkileri | PASSED | [Canlı GRANT/REVOKE, pool reset, iptal ve M3 regresyonu](STAGES/15.json) |
| 16 PL/SQL blokları, tipli IR ve fonksiyonlar | PASSED | [Tipli rutin/OUT, SQL lexical bağlama, sınırlar ve kalıcı IR](STAGES/16.json) |
| 17 Cursor, trigger ve çoklu sonuç semantiği | PASSED | [Adlandırılmış native sonuçlar, compound değerler, trigger rollback](STAGES/17.json) |
| 18 PL/SQL debugger, test ve profil | PASSED | [Satır adımı, watch, izole test, örneklem ve TLS kapanışı](STAGES/18.json) |
| 19 History log, diff, saklama ve bütünlük | PASSED | [Stable ID, WAL atomikliği, UTC, pin, kota ve tamper testi](STAGES/19.json) |
| 20 History restore ve revert | PASSED | [Stable restore, çakışma, crash ve yedi proje regression](STAGES/20.json) |

[M1 temel altyapı raporu](M1_REPORT.md) · [M2 istemci ve depolama raporu](M2_REPORT.md) · [M3 SQL ve güvenlik raporu](M3_REPORT.md) · [Sıradaki etap](NEXT_TASK.md)
