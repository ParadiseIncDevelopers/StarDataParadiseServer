# MySQL üzerinde History satır günlüğü

`provision.py` yeni bir InnoDB uygulama tablosu ve o tabloya özel bir InnoDB günlük şeması için SQL üretir. Tablo, tek bir imzalı BIGINT birincil anahtar ve BIGINT, metin veya BLOB sütunlarından oluşur. Çıktıyı MySQL istemcisine uygulayın ve uygulama hesaplarına DML izinlerini üç trigger da başarıyla oluştuktan sonra verin. Üretici mevcut şemaları değiştirmez. Yerel `CREATE` adımları ayrı DDL işlemleridir. Kurulum yarıda kalırsa erişim vermeden önce oluşturulan nesneleri inceleyin.

```powershell
python -B compatibility/history/provision.py --source-schema sales --table orders --journal-schema sales_history --key id --column id:bigint --column label:text --column payload:blob
```

## Kapsam ve okuma

Trigger’lar doğrudan `INSERT`, `UPDATE` ve `DELETE` değişikliklerini izler. Satır ve günlük aynı InnoDB transaction’ına yazılır. Rollback veya savepoint geri alımı ikisini birlikte etkiler. Birincil anahtar güncellemesi satır kimliğini korur. Silinip tekrar kullanılan anahtara yeni satır kimliği verilir. Günlük imajları BIGINT ve metin için wire türlerini kullanır. BLOB imajı boyutu ve SHA-256 özetini içerir.

```sql
CALL sales_history.history_log(NULL, NULL, NULL, NULL, NULL, 0, 32);
CALL sales_history.history_read(1, NULL, 0, 32);
CALL sales_history.history_diff(0, 1, NULL, 0, 32);
```

`history_log` sırasıyla başlangıç ve bitiş revision’ı, satır kimliği, iki kapsayıcı Unix saniyesi, offset ve limit alır. `history_read` revision, satır kimliği, offset ve limit alır. `history_diff` başlangıç ve bitiş revision’larındaki net imaj farklarını verir. İsteğe bağlı filtreler `NULL` olabilir. En fazla 256 kayıt alınabilir. Sabit bir bitiş revision’ıyla sayfalayın. `0`, boş tablonun ilk durumunu gösterir.

Okuma yordamları `SQL SECURITY INVOKER` ile çalışır. Yalnız günlüğe erişme yetkisi verilmiş yöneticiler bunları kullanmalıdır. Uygulama yazıcıları kaynak tablo DML yetkileriyle çalışır. Günlüğe doğrudan erişim veya şema yetkisi verilirse bunun kapsamı MySQL izinlerine bağlıdır. Eski nesne bazlı `HISTORY` ve güncel `SELECT` kontrolü bu modülde henüz yok.

## Tek satırı geri yükleme ve alan değişikliğini geri alma

`--restore` ile oluşturulan BIGINT ve metin tablolarında, hâlâ mevcut bir satırı History’deki önceki bir imaja döndürmek için aşağıdaki iki fonksiyonu kullanın. BLOB özetleri herhangi bir değer geri yüklemek için yetersiz olduğundan BLOB sütunlu tablolar bu seçeneği reddeder.

```powershell
python -B compatibility/history/provision.py --source-schema sales --table orders --journal-schema sales_history --key id --column id:bigint --column label:text --restore
```

```sql
SELECT sales_history.history_restore_preview(1, 1);
SELECT sales_history.history_restore_apply('<plan_id>', '<request_token>');
```

İlk fonksiyon hedef revision ve stable satır kimliğini alır. Canlı satırda değişiklik yapmadan önceki ve hedef tipli imajları, günlüğün geçerli revision’ını ve bir plan kimliği döndürür. Apply için aynı MySQL hesabı tarafından sağlanan, 32 küçük harfli onaltılık karakterden oluşan bir request token kullanılır. Tabloya arada yazıldıysa `stale_preview`, hedef anahtar başka satıra aitse `row_identity_conflict` döner. Tablo biçimi değiştiyse veya ilgili History trigger’ları eksikse işlem `schema_conflict` ile durur.

Apply bir native MySQL `UPDATE` çalıştırır. Tablonun kısıtları ve Decision guard’ları bu işlemde geçerlidir. Başka bir tablonun satırlarını otomatik değiştiren yabancı anahtar güncelleme eylemleri kapsam dışıdır. Uygulanan imaj hedefle eşleşmezse satır, günlük ve receipt birlikte geri alınır. Fonksiyon kendi başına transaction başlatmaz veya commit etmez. Çağıranın transaction’ı ve rollback’i geçerlidir. Aynı hesap, plan ve token başarılı çağrının revision’ını `replayed: true` ile tekrar alabilir.

Belirli bir satır olayındaki tek bir alan değişikliğini geri almak için olay revision’ını, stable satır kimliğini ve anahtar dışındaki alan adını verin.

```sql
SELECT sales_history.history_revert_preview(2, 1, 'label');
SELECT sales_history.history_revert_apply('<plan_id>', '<request_token>');
```

Seçilen olay alanı değiştirmediyse `revert_field_unchanged` döner. Daha sonraki bir yazma aynı alanı başka bir değere taşımışsa `revert_field_conflict` döner. Alana eski değer zaten yazılmışsa `restore_no_change` döner. Apply yalnız seçilen alanı native MySQL `UPDATE` ile değiştirir ve diğer alanların imajını denetler. RESTORE ve REVERT planları kendi apply fonksiyonlarıyla eşleşir. İkisi de aynı stale revision, hesap, token, izin ve transaction denetimlerini kullanır.

Bu fonksiyonlar `SQL SECURITY INVOKER` ile çalışır. Yordam ve kaynak tablo izinlerini ve plan tablosu erişimini güvenilir History yöneticilerine verin. Uygulama hesaplarına günlük ve plan erişimi vermeyin. İkili günlük kullanılan sunucuda veri değiştiren stored function’lar MySQL’in kurulum yetkilerine ve binary logging politikasına tabidir. Yerel kabul ikili günlük kapalı geçici örnekte yapıldı. [MySQL stored function ve binary logging](https://dev.mysql.com/doc/refman/8.4/en/stored-programs-logging.html)

## Revizyon ve kapsam sınırları

Transaction’lar ilgili günlük sayacı üzerindeki InnoDB kilidiyle sıralanır. Bir transaction’ın tüm satır imajları commit veya rollback ile birlikte yayımlanır. `revision` tabloya ait olay sırasıdır. Bir transaction birden çok olay oluşturabilir. Eski WAL LSN’siyle eşleşme yoktur. `observed_utc` trigger’ın gördüğü SQL zamanıdır. Kalıcı yayım zamanını göstermez. Başka oturumların commit’lerini okumak için bağımsız bir okuma oturumu kullanın. Ortak sayacı paylaşan yazılar bekleyebilir. MySQL deadlock yanıtında uygulama transaction’ı yeniden deneyebilir.

`TRUNCATE`, DDL, tetikleyici yönetimi ve yabancı anahtar cascade işlemleri bu izleme yolunun dışındadır. Uygulama yazıcılarına DDL izni vermeyin. İzlenen tablonun DDL veya cascade ile değiştirilmesi, yeni bir geçmiş başlangıcı oluşturmayı gerektirir. İlk kapsam yeni oluşturulan tek tabloyla sınırlıdır. Tüm tabloyu veya silinen satırı geri yükleme, birden çok alanı veya satırı geri alma, katalog geçmişi, retention, pin, doğrulama, eski `HISTORY …` dilbilgisi, RPC ve `history_v1` yeteneği bekliyor. [MySQL trigger davranışı](https://dev.mysql.com/doc/refman/8.4/en/create-trigger.html)

## Kabul

CA doğrulamalı geçici Server 26.10.0 üzerinde `verify.py` imaj, satır kimliği, BLOB özeti, yetki reddi, savepoint, rollback ve eşzamanlı yazıları denetledi. Aynı geçici veri diziniyle yeniden başlatma sonrasında veri ve günlük de okundu. Yerel kanıt `out/mysql-migration/fork-history-acceptance.log` ve `fork-history-acceptance.exit` dosyalarındadır.

Mevcut Workbench editörü ayrı geçici örnekte geçmiş satır imajlarını ve net farkı `CALL history_read`, `CALL history_diff` ve `CALL history_log` ile sonuç tablosunda gösterdi. Yerel kayıt `out/mysql-migration/fork-history-decision-workbench.log` dosyasındadır.

`verify_restore.py` geçici Server’da tipli imaj, stale revision, çakışan anahtar, hesap ve token, native Decision ile CHECK reddi ve rollback’i denetledi. Aynı örnek yeniden başlatıldığında receipt başka revision üretmeden döndü. Yerel kayıt `out/mysql-migration/fork-history-restore-acceptance.log` ve `fork-history-restore-acceptance.exit` dosyalarındadır.

Field REVERT de eski ve yeni imajları, sonraki bağımsız alan değişikliğinin korunmasını, alan bazlı UPDATE iznini, stale planı, Decision reddini, rollback ve yeniden başlatmadan sonra receipt tekrarını geçti. RESTORE ve REVERT birlikte `out/mysql-migration/fork-history-revert-acceptance.log` ile `fork-history-revert-acceptance.exit` dosyalarında yer alır.
