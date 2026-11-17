# Fiziksel InnoDB yedekleme

Windows’ta yerel StarDataParadise Server, çalışan InnoDB örneğinin fiziksel kopyasını `CLONE LOCAL DATA DIRECTORY` ile alır. Kaynak Server bu işlem için kapatılmaz. `create` kopyayı hazırladıktan sonra SHA-256 manifestini çıkarır. Aynı fork sürümüyle ayrı bir loopback Server açar. Bağımsız veri dizinini, UUID’yi ve CA doğrulamalı TLS’i denetler. Uygulama tabloları `CHECK TABLE QUICK` kontrolünden ve test Server’ı temiz kapanıştan geçince arşivi yayımlar. Bir sonraki `create` çağrısı aynı arşiv kimliğini, kaynak UUID’sini ve Server ayarlarını doğrular. [MySQL yerel Clone](https://dev.mysql.com/doc/refman/26.10/en/clone-plugin-local.html)

`verify` arşivdeki dosyaların tam listesini, boyutlarını ve SHA-256 değerlerini manifestle karşılaştırır. `restore` aynı kontrolü yapar, geçici kopyasını yeniden başlatır ve doğrulanan veri dizinini daha önce var olmayan bir hedefe taşır. Kaynakla farklı UUID ve TLS dosyaları oluşturulur. Kaynağın veri dizini ve arşiv ayrı depolama konumlarında olmalıdır. Hazır arşivi ayrı diske kopyalamak disk kaybına karşı koruma sağlar. Manifest bozulmayı saptamak içindir. Arşiv ve manifest, yazma izni sınırlı yerel dizinlerde tutulmalıdır.

## Kurulum ve çalıştırma

Aynı Server kaynak ağacında `clone` CMake hedefini derle. Kullanılan Windows x64 `RelWithDebInfo` derlemesinde eklenti `out/mysql-migration/bfast/plugin_output_directory/RelWithDebInfo/mysql_clone.dll` altında üretilir. Kaynak Server'ın plugin dizinine bu DLL’i yerleştir ve Server’ı `--plugin-load-add=mysql_clone.dll` ile başlat ya da yönetici olarak aşağıdaki tek seferlik SQL’i çalıştır. [MySQL Clone kurulumu](https://dev.mysql.com/doc/refman/26.10/en/clone-plugin-installation.html)

```sql
INSTALL PLUGIN clone SONAME 'mysql_clone.dll';
```

Araç Python 3.12+, aynı sürümün `mysqld.exe`, `mysql.exe` ve runtime DLL’lerini kullanır. Arşiv dizinini önceden oluştur ve yerel Server servis hesabı ile işleten hesabın bu dizine erişimini sağla. `create` arşivin doğrulanması için ikinci bir geçici fiziksel kopya oluşturur. Kaynak bağlantısı yalnızca `127.0.0.1` ve CA onaylı TLS üzerinden yapılır. Kullanıcıya `BACKUP_ADMIN`, InnoDB katalog incelemesi için `PROCESS`, tüm uygulama tablolarını görüp denetlemek için `SELECT` ve geri yüklenen test örneğini temiz kapatmak için `SHUTDOWN` gerekir. Parolalı hesap için yalnız ilgili işletim hesabının okuyabildiği, `[client]` bölümünde `password` satırı olan bir MySQL option dosyasını `--client-options` ile ver. Aşağıdaki yolları kendi kurulumuna göre belirle.

```powershell
$id = [guid]::NewGuid().ToString('N')
$runtime = @('--mysqld', $mysqld, '--mysql', $mysql, '--basedir', $basedir,
             '--plugin-dir', $pluginDir, '--user', $user, '--client-options', $optionFile)
python -B -m compatibility.backup.physical_backup --archive-root $archives create $id @runtime --source-port 3306 --source-ca $sourceCa
python -B -m compatibility.backup.physical_backup --archive-root $archives verify $id
python -B -m compatibility.backup.physical_backup --archive-root $archives restore $id @runtime --target $newDataDir
```

Bağımsız sunucuyu ayağa kaldırırken geri yüklenen veri dizinini ve manifestteki kaynak InnoDB sayfa boyutu ve tablo adı ayarlarını kullan. Port, yapılandırma ve bağımsız üretim TLS sertifikası yeni kurulumda belirlenir. Arşiv ve hedef dizinleri kullanıcı verileri ile SQL hesabı kayıtlarını içerir.

Bu biçim uygulama InnoDB tabloları, varsayılan sistem tablespace yerleşimi ve sunucunun veri dizinindeki undo ile doublewrite dosyaları içindir. Uygulama şemalarında MyISAM gibi başka motorlar veya keyring gerektiren şifreli veri bulunduğunda araç işlemi reddeder. Harici InnoDB tablespace’ler yerel Clone tarafından reddedilir. Binlog zinciri, PITR, `store.sdp`/WAL formatı, eski yedek işi RPC’leri ve yedek zamanlaması bu biçimin kapsamı dışındadır. [MySQL Clone sınırları](https://dev.mysql.com/doc/refman/26.10/en/clone-plugin-limitations.html)

## Wire v1 yedek komutları

Wire servisini `--legacy-backup-config` ile başlatmak `backup_catalog` ve `backup_verify` komutlarına yapılandırılan fiziksel arşivlere erişim verir. `backup_verify` arşivi yeniden açıp ayrı Server’da tablo kontrolü yapar. `allow_restore` etkinse `backup_restore` doğrulanan yeni veri dizinini `work_root/target_name` altında yayımlar. Bir arşivin kaynak UUID’si wire arka ucunun Server UUID’siyle eşleşmelidir. Eski yanıtın `lsn` alanı bu fiziksel biçimde `null` döner. `format` alanı arşiv biçimini belirtir. `pitr` ve tam `backup_v1` yeteneği kapalıdır. Eski `backup_submit`, `backup_delete`, yedek işi durum ve iptal RPC’leri ve zamanlama bekliyor.

Yalnızca `administrators` listesinde bulunan tam MySQL `CURRENT_USER()` kimlikleri erişebilir. `runtime` hesabı geçici geri yüklenen kopyada bağlanabilmeli, uygulama tablolarını denetleyebilmeli ve Server’ı temiz kapatabilmelidir. `archive_root` ve `work_root` birbirinden ve Server veri dizinlerinden ayrı, önceden oluşturulmuş yerel dizinlerdir. Eski katalog sözleşmesi en fazla 16 arşiv kimliği listeler. Yapılandırma ve kimlik bilgisi dosyalarına özgü erişim izinlerini wire hizmet hesabına verin.

```json
{
  "archive_root": "D:/sdp/archives",
  "work_root": "C:/sdp/backup-work",
  "administrators": ["backup_admin@localhost"],
  "allow_restore": true,
  "runtime": {
    "mysqld": "C:/sdp/server/bin/mysqld.exe",
    "mysql": "C:/sdp/server/bin/mysql.exe",
    "basedir": "C:/sdp/server",
    "plugin_dir": "C:/sdp/server/lib/plugin",
    "user": "backup_probe",
    "client_options": "C:/sdp/private/probe.cnf"
  }
}
```

## Kabul testi

```powershell
python -B -m compatibility.tests.physical_backup_acceptance
```

Test kaynak ve geri yüklenen Server’ı `out/compatibility-tests` altında oluşturur. Aynı fork yürütülebilir dosyaları ile açık bir yazma transaction’ı sırasında fiziksel snapshot, istek tekrarı, desteklenmeyen motor, bozuk SHA-256, var olan hedefin korunması ve bağımsız hedefin tekrar açılmasını dener. Kaynak ve hedef arasında tipli SQL satırları, katalog kolonları, saklı yordam ve commit/rollback ile hazırlanan kayıtlar karşılaştırılır. `--mysqld`, `--mysql`, `--basedir` ve `--plugin` ile farklı bir derlemeyi sınayabilirsin.

Wire bağımlılıklarının kurulu olduğu Python ile aşağıdaki kabul testi de yönetici yetkisini, arşiv doğrulamasını ve yayınlanan yeni veritabanındaki tipli satırları denetler.

```powershell
python -B compatibility/wire_v1/backup_rpc_acceptance.py
```
