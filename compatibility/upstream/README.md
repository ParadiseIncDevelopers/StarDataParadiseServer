# StarDataParadise Server 0.5.0 önizlemesi

`server-26.10.0.patch`, MySQL Server tabanına uygulanan kaynak değişikliklerini ve iki `mysqltest` senaryosunu içerir. Kaynak tabanı `mysql/mysql-server` deposunun `mysql-26.10.0` etiketi ve `561753af986c3c006c4b6c7caa8359748fb55b22` commit’idir.

```powershell
git apply --check C:/src/StarDataParadiseServer/compatibility/upstream/server-26.10.0.patch
git apply C:/src/StarDataParadiseServer/compatibility/upstream/server-26.10.0.patch
```

Yamayı ayrı bir upstream çalışma ağacına uygulayın ve o ağacın CMake derlemesinde `-DMYSQL_SERVER_SUFFIX=-StarDataParadise` kullanın. Motor `26.10.0-StarDataParadise` sürümünü bildirir. Bu depo wire v1, History, Decision, PL/SQL, ML, compute ve yedekleme uyumluluk katmanlarını da içerir. Kurulum ve canlı kabul komutları [uyumluluk kılavuzunda](../README.md), sınanan işlemler ve açık kalanlar [geçiş kaydında](../../docs/coordination/MYSQL_MIGRATION.md) bulunur.
