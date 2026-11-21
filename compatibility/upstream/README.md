# StarDataParadise Server `v0.5.0`

`server-26.10.0.patch`, MySQL Server tabanına uygulanan birleşik kaynak değişikliklerini ve native `mysqltest` senaryolarını içerir. Kaynak tabanı `mysql/mysql-server` deposunun `mysql-26.10.0` etiketi ve `561753af986c3c006c4b6c7caa8359748fb55b22` commit’idir. Ürün sürümü 0.5.0, kanal stable olur.

```powershell
git apply --check C:/src/StarDataParadiseServer/compatibility/upstream/server-26.10.0.patch
git apply C:/src/StarDataParadiseServer/compatibility/upstream/server-26.10.0.patch
```

Yamayı ayrı bir upstream çalışma ağacına uygulayın ve o ağacın CMake derlemesinde `-DMYSQL_SERVER_SUFFIX=-StarDataParadise` kullanın. Motor ayrı uyumluluk sürümü `26.10.0-StarDataParadise` bildirir. Yalnız bu birleşik yama uygulanır; önceki ara özellik yamaları ek kurulum adımı değildir. Yanındaki manifest, temiz pinned upstream'e uygulanabilirliği ve normalize kaynak byte'larının SHA256 değerlerini kaydeder.

Bu sürüm AI/Knowledge, typed araç/onay/roller ve [kısa SQL/PLSQL](sql-short-native.md) içerir. AI kurulumu ve startup profilleri [AI kılavuzunda](../ai/README.md), gerçek motor/SDK/Workbench kabulü ve dış sağlayıcı sınırları [stable kabul kaydında](../../docs/coordination/STABLE_RELEASE_2026_11_21.md) bulunur. Yeni SQL HIST/Git ve public SQL JOB sistemi ertelendi; native capability yanıtları bunları etkin göstermez.

Depodaki var olan wire v1, legacy History/Decision/PLSQL/ML/compute ve yedekleme uyumluluk katmanları korunur. Bu legacy API'ler yeni SQL HIST/JOB dilinin kabul edildiği anlamına gelmez. [Uyumluluk kılavuzu](../README.md) · [Önceki geçiş kaydı](../../docs/coordination/MYSQL_MIGRATION.md).
