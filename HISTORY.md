# History v1

SQL Server, commit edilmiş WAL kayıtlarındaki satır ve katalog imajlarını revision sırasıyla sunar. `revision`, `parent` ve `transaction` artan WAL LSN’leriyle tanımlanır. Başarılı bir SQL veri ya da katalog commit’i, actor ve UTC saniyesini aynı WAL çerçevesine yazar. Eski WAL kayıtları mevcut commit outcome bilgisiyle eşleştirilir. Outcome bilgisi bulunmayan eski storage işlemlerinde actor ve UTC alanları `null` döner.

```sql
HISTORY LOG FOR public.orders FROM REVISION 12 TO REVISION 40 LIMIT 25;
HISTORY LOG FOR public.orders ROW 8589934593 SINCE UTC 1760000000;
HISTORY DIFF public.orders FROM REVISION 12 TO REVISION 40;
HISTORY READ public.orders AS OF REVISION 12;
HISTORY VERIFY;
HISTORY STATUS;
```

`LOG` aynı transaction içindeki değişiklikleri yayın sırasıyla gösterir. `DIFF` uçlardaki net farkı, `READ` ise belirtilen revision’da geçerli satırları ve şemayı verir. PK güncellemesi stable row ID’yi korur. Katalog nesnelerinde stable object ID, nesne türü ve şema sürümü bulunur. İki uçta aynı ad farklı stable nesnelere aitse diff `object_identity_changed` ile durur. `SINCE UTC` ve `UNTIL UTC` kapsayıcı Unix saniyesi filtreleridir. Sıralama revision ile yapılır. BLOB alanı `type`, `size`, `sha256` ve `scope` ile özetlenir. DDL ve PL/SQL kaynak imajları için ek DDL yetkisi gerekir.

Kapsamlı okuma için bugünkü `HISTORY` ile tablo veya view üzerinde `SELECT`, rutinde `EXECUTE`, diğer katalog nesnelerinde `DDL` gerekir. Silinmiş nesnelerin geçmişini yönetici görebilir. Hesap/parola kayıtları, outcome token’ları, policy kayıtları ve fiziksel indeks sayfaları sonuçlara alınmaz. Global tarama ve doğrulama global `HISTORY` gerektirir. History komutu bağımsız bir salt okunur transaction alır. Açık kullanıcı transaction’ı veya sonuç seti varsa komut `INVALID_STATE` döner.

```sql
HISTORY PIN SNAPSHOT 'qa-copy' AT REVISION 12;
HISTORY UNPIN SNAPSHOT 'qa-copy';
HISTORY RETAIN BEFORE REVISION 20 QUOTA 1048576;
HISTORY PURGE BEFORE REVISION 20;
```

Yalnız yönetici retention ve pin durumunu değiştirebilir. `BEFORE REVISION` belirtilen revision’ı ve sonrasını okunabilir bırakır. Geriye açılması istenen bir sınır reddedilir. `snapshot`, `dataset`, `backup` ve `restore` türünde kayıtlı bir pin sınırın ötesine geçilmesini engeller. Pin adı 1–64 ASCII harf, rakam, alt çizgi, nokta veya kısa çizgiden oluşabilir. Aktif sorgular transaction lease’i boyunca aynı snapshot’ı tutar. `HISTORY STATUS` kayıtlı backup ve dataset kopyalarını bildirir. Bu kopyaların kendi yaşam döngüleri vardır.

Sınır dışındaki sorgular `INVALID_STATE` ve `revision_not_recoverable` döner. History kotasına sığmayan commit geri alınır ve veri, journal, outcome birlikte yayınlanır. WAL v1 recovery tam segment zincirini kullandığından `PURGE` okunabilir geçmiş sınırını kalıcı olarak ilerletir. Fiziksel WAL arşivi recovery için saklanır ve kendi kotasına tabidir. Disk arızası kurtarması için ayrı bir backup gerekir.

`HISTORY VERIFY` WAL CRC’lerini ve yeni revision’ların SHA-256 zincirini denetler. İlk seal öncesi revision’lar WAL CRC’leriyle doğrulanır. Bağımsız güven kökü bulunmadığı için zincir bütünlük kontrolü sağlar ve dışarıdan doğrulanmış değiştirilemezlik garantisi vermez.

## Wire ve Native

Kimliği doğrulanmış bağlantıda `history_v1` yeteneğiyle `history_log`, `history_diff`, `history_read`, `history_verify`, `history_status`, `history_retain`, `history_purge`, `history_pin` ve `history_unpin` kullanılabilir. `target` bir SQL nesne adıdır. `revision`, `from`, `to`, `row_id` ve `quota_bytes` pozitif ondalık dizgelerdir. `since_utc` ve `until_utc` işaretli saniye, `offset` ve `limit` tamsayıdır. Sorguların varsayılan sayfa boyutu 32, üst sınırı 256’dır. Kalan sonuçların konumu `next_offset` ile döner. Geçmiş değişirken tutarlı sayfalama için aynı `TO REVISION` veya `AS OF REVISION` sınırı kullanılmalıdır.

Hazırlanmış `HISTORY` SQL komutları `execute.result.history` altında aynı JSON verisini döndürür. Native ABI 1 istemcisi bu veriye `sdp_history_command` ile doğrudan erişir. Örnek satır farkı, alan tiplerini ve binary özetini aynı imajda taşır.

```json
{
  "revision": "40",
  "parent": "39",
  "transaction": "40",
  "kind": "row",
  "object_id": "4294967297",
  "row_id": "8589934593",
  "schema_version": 1,
  "before": {"schema_version": 1, "values": {"id": {"t": "i64", "v": "1"}}},
  "after": {"schema_version": 1, "values": {"id": {"t": "i64", "v": "2"}}}
}
```

## Geri yükleme ve revert

```sql
HISTORY RESTORE public.orders AS OF REVISION 40 ROW 8589934593;
HISTORY REVERT public.orders REVISION 43 ROW 8589934593 FIELD total;
```

Bu komutlar önce planı döndürür. Plan, karşılaştırılan güncel revision’ı, stable ID’leri, seçilen alanı, erişilebilir bağımlılıkları ve satır/katalog etkilerini içerir. Bir plan oluşturmak yazma yapmaz. Uygulamak için aynı bağlantıdan ilgili verb ile `APPLY '<plan_id>' REQUEST '<request_token>'` gönderilir. RPC’de `history_restore` ve `history_revert` kullanılır. Önizleme gövdesi `target`, `revision`, isteğe bağlı `row_id` ve `field` alır. Uygulama gövdesi yalnızca `plan_id` ve `request_token` alır. İkisi 32 küçük harfli onaltılık karakterdir. Her oturum son planını tutar.

Güncel revision değişmişse uygulama `stale_preview` döndürür. Kullanıcı yeni plan alıp etkileri tekrar inceleyebilir. Uygulama güncel yazma, `HISTORY`, okuma ve gerekli DDL yetkisini doğrular. Satırları güncel FK, unique, check ve trigger kontrollerinden geçirir. Aynı PK başka bir stable ID tarafından kullanılıyorsa işlem çakışma olarak durur. Şema veya alan tipi uyuşmazlığı `schema_conflict` döndürür. Alan revert’i commit’in değiştirdiği alanı geri alır ve sonradan düzenlenen bağımsız alanlara dokunmaz.

Tablo seviyesinde RESTORE seçilen revision’ın satır kümesini hedefler. Önizleme bunun için silinecek, değişecek ve eklenecek satırları gösterir. TRUNCATE geri alımı, kapatılmış DROP TABLE nesnesinin geçmişten aynı kimlikle açılması ve routine kaynağının güncel bağımlılıklarla yeniden derlenmesi desteklenir. Her başarılı uygulama yeni bir revision üretir. Tablo ve indeks geri açımında stable nesne kimlikleri korunur. Fiziksel indeks kökleri güncel collation ile yeniden kurulur.

Apply yanıtı kaybolduğunda aynı actor, aynı verb, plan kimliği ve request token ile tekrar gönderir. Kalıcı receipt ve outcome aynı commit içinde yazılır. Yeniden bağlanma veya sunucunun yeniden başlatılmasından sonra 24 saat içinde önceki LSN `replayed: true` ile döner. Başka bir işlemin token’ı `request_conflict` üretir. Uygulanmamış planın oturumu kapandıysa yeni önizleme gerekir. Doğrulaması geçmeyen History üzerinden apply yapılmaz.
