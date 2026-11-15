# Uyumluluk ve yetenekler

| Bileşen | Doğrulanan durum |
| --- | --- |
| Windows x64 | Visual Studio 2026, v145, Windows SDK 10.0.26100.0, CMake 4.2.2 |
| Workbench | Qt Base 6.11.3. Bağlantısız başlangıç, iki bağımsız native TLS oturumu, Windows Credential Manager ve sır içermeyen profil dışa aktarımı doğrulandı. |
| CLI | C++20. Native ABI 1 ile TLS, DNS, profiller, terminal ve JSON durum çıktısı doğrulandı. |
| Server | TLS 1.2+, bootstrap, auth, hello, health, capabilities, süreç ve dizin kilidi. Adres varsayılanı loopback. |
| Native DLL | Ürün 0.1.0, C ABI 1, wire v1. Windows x64 DLL ve paket tüketicisi gerçek Server’a bağlandı. |
| .NET, Java, Python SDK | Yerel paket 0.1.0 ve paket smoke testleri. Ağ istemcileri sonraki SDK etaplarında. |
| SQL, metadata, transaction, jobs, yönetim RPC, History, Decision ve ML | Unavailable. |
| Storage | Disk formatı v1. Heap, BLOB, satır kodlama ve salt okunur integrity taraması doğrulandı. |
| WAL / değişiklik journal | WAL v1 ve iç transaction API, seri lease, savepoint, atomik satır/katalog değişiklik kaydı. |
| Recovery ve checkpoint | Checkpoint formatı v1. Windows süreç kesintisi, torn-page onarımı, idempotent replay, WAL rotasyonu ve Etap 08/09 uyumluluğu doğrulandı. M2 tam geçmiş için segmentleri saklar. |
| PL/SQL IR ve model formatı | Sürüm atanmasını bekliyor. |
| Linux, macOS ve diğer Windows mimarileri | NOT_RUN. |

Wire v1 kaynakları [contracts/v1](../../contracts/v1/PROTOCOL.md) altında. Etap kanıtları [M1](M1_REPORT.md) ve [M2](M2_REPORT.md) raporlarında.
