# AI yetkileri ve onaylı işlemler

Bu sözleşme Server 0.5.0 native onay ve rol API'sini tanımlar. Gerçek sunucudaki
tam Action/rol kabulü native16 ile geçti (`out/ai-native-check/build16-actions.log`,
exit0). `ai_action_v1` ve `ai_role_acl_v1` kapıları release kabulünü ve seçili
başlangıç profilini birlikte bildirir; yalnız özelliğin kaynakta bulunması bu
kapıları açmaz.

## Kurulum profili

İlk kurulum ve yükseltme bakım başlangıcında yapılır:

```text
--sdp-ai-sql=ON --sdp-ai-actions=OFF
```

Yönetici sırasıyla `compatibility/ai/catalog.sql`, `tool_catalog.sql` ve
`action_catalog.sql` dosyalarını kurar. Temiz yeniden başlatmadan sonra normal
profil kullanılır:

```text
--sdp-ai-sql=ON --sdp-ai-actions=ON
```

İki seçenek de salt okunur başlangıç seçeneğidir. Public ACTION ve AI rol
komutları iki seçeneği de ister. `actions=ON` ile normal `CALL` onay denetimi,
`ai-sql=OFF` başlangıcında da çalışır. Eksik veya değişmiş yönetilen politika
kataloğu hata üretir. Kurulu action modülündeki DROP/RENAME temizliği bakım
`actions=OFF` profilinde de native hesap işlemiyle aynı transaction'ı kullanır.

## Aktif roller ve izinler

AI nesnesinin sahibi aşağıdaki nesne izinlerini verebilir. Yordamın APPROVE
izni için güncel native ALTER ROUTINE yetkisi gerekir.

| Hedef | İzinler |
| --- | --- |
| MODEL | USE |
| BEHAVIOR | USE |
| RESPONSE SCHEMA | USE |
| KNOWLEDGE | EXECUTE |
| CONVERSATION | SELECT, APPEND |
| PROCEDURE | APPROVE |

```sql
GRANT USE ON MODEL satis_modeli TO ROLE satis_ai;
GRANT SELECT, APPEND ON CONVERSATION satis_gorusmesi TO ROLE satis_ai;
GRANT EXECUTE ON KNOWLEDGE musteri_ozeti TO ROLE satis_ai;
GRANT APPROVE ON PROCEDURE siparis_iptal TO ROLE satis_onay;
REVOKE APPROVE ON PROCEDURE siparis_iptal FROM ROLE satis_onay;
```

FUNCTION ve PROCEDURE EXECUTE izni normal native MySQL yetki yolunu kullanır.
Custom AI nesnelerinde CREATE/DESCRIBE/GRANT katalogda kayıtlı schema ve nesne
adının tam yazımını kullanır; bu adlar binary collation ile karşılaştırılır.
Native yordam hedefi ise MySQL DD kimliğine ve onun ad karşılaştırma kuralına
bağlanır. Arayüz hedef adını şema gezginindeki katalog kaydından almalıdır.
AI nesnesine rol izni verilmesi hesabın doğrudan izni haline gelmez. Yalnız
oturumun aktif rol kökleri ve bunların güncel iç içe üyelikleri geçerlidir.
`SET ROLE NONE` bu rol kaynaklı izinleri kaldırır.

```sql
SELECT STARDATAPARADISE_PRINCIPALS();
```

Bu sonuç authenticated/logical sahibi ve güncel aktif rol kanıtını gösterir.
Katalog yordamının DEFINER hesabını veya fiziksel worker hesabını sahibi
yerine kullanmaz. Hesap/rol DROP işlemi eski izinleri temizler; aynı authID'nin
yeniden yaratılması eski izinleri veya bekleyen insan onayını canlandırmaz.
Geçerli native USER RENAME izinleri authID ile taşır ve ilgili owner/approver
hesabına bağlı bekleyen onayları stale yapar. MySQL'in rol kimliğini yeniden
adlandırmayı reddeden 3532 kuralı korunur; başarısız rol rename işlemi mevcut
hesabı, izinleri ve bekleyen onay kayıtlarını değiştirmez.
Tamamlanmış receipt/audit kayıtları korunur.

## Hazırla, incele, onayla, çalıştır

```sql
REQUIRE APPROVAL ON PROCEDURE siparis_iptal;

PREPARE ACTION CALL siparis_iptal(9001)
RETURNING ID INTO @islem_id;
DESCRIBE ACTION @islem_id;
```

Hazırlama bir JSON belge ve kalıcı `action_id` döndürür. Varsayılan dönüş
değişkeni `@sdp_action_id` olur. Yetkili onaycı gözlenen sabit kimlik üzerinden
karar verir; işlem sahibi aynı kimlikle çalıştırır:

```sql
APPROVE ACTION '0123456789abcdef0123456789abcdef';
-- Alternatif karar:
DENY ACTION '0123456789abcdef0123456789abcdef';

EXECUTE ACTION @islem_id;
```

PREPARE, APPROVE, DENY, EXECUTE ve DESCRIBE bir sonuç belgesi üretir. DESCRIBE
belgesinde `action_id`, `state`, `target.schema_name`, `target.name`,
`arguments`, `definition_hash`, `resource_scope`, `resource_count`, tarihler,
onaycı, bağlı tool kimliği ve varsa sonuç bulunur. Ham kaynak satırları,
protected rol kanıtı ve native binding kanıtı bu belgeye konmaz.

EXECUTE kimliği string literal, `@değişken`, `:local`, prepared `?` veya
parantezli saf ifade olarak alır. DESCRIBE string literal, `@değişken` veya
prepared `?` kabul eder. Kimlik 32 küçük onaltılık karakterdir. SQL üreten
arayüzler sunucudan gözlenen kimliği sabitler; değişen hedef/parametre önceki
önizlemeyi geçersiz kılar.

Normal `CALL siparis_iptal(...)` onayı atlayamaz. Denetim argümanları ve yordam
gövdesini çalıştırmadan önce ve prepared execute sırasında yeniden yapılır.
Başarılı EXECUTE tekrarında saklanan sonuç okunur; uygulama yazısı yinelenmez.

## Araç çağrısına bağlı hazırlama

```sql
PREPARE ACTION FOR TOOL CALL '0123456789abcdef0123456789abcdef'
RETURNING ID INTO @islem_id;
```

Hedef, parametreler, yordam tanımı, roller ve başlangıç zaman dilimi korumalı
tool/run kaydından gelir. Provider'ın metni yürütülebilir SQL haline getirilmez.
APPROVE/DENY, bağlı bekleyen tool çağrısını aynı transaction içinde çözer.
Worker güncel owner yetkisiyle onaylanan aynı hedefi ve argümanları yeniden
doğrular; çağrı/sonuç kimlikleri kalıcı receipt ile bağlanır.

## Kaynak ve transaction sınırı

Onay beş dakika geçerlidir. Hazırlanan yordamın native tanımı, trigger ve tablo
bağımlılıkları, parametreleri, politika sürümü ve kaynak durumu onaya bağlanır.
İlk güvenli kaynak kapsamı `all_dependency_tables` olur: ilgili yordamın tüm
bağımlı tabloları karşılaştırılır. WHERE dışında kalan ilgili tablo satırının
değişmesi de yeni hazırlama gerektirebilir.

EXECUTE temiz top-level bağlamda kendi kısa REPEATABLE READ transaction'ını
açar. Tam primary-key taraması ve supremum aralığına kadar next-key kilitleri,
doğrulama ile uygulama yazısı arasındaki INSERT/UPDATE yarışını kapatır.
Receipt ve uygulama yazıları aynı commit/rollback sınırındadır. Oturumun kalıcı
isolation, timezone veya rol seçimi bu işlem için değiştirilmez.

Kaynaklar InnoDB ve primary key gerektirir; en fazla 32 tablo, tablo başına
10.000 satır ve toplam 4 MiB ham kaynak değeri desteklenir. Desteklenmeyen tür,
FK/cascade, bütçe aşımı, key bulunmaması veya güvenle doğrulanamayan yordam
uygulama yazısından önce açık hata üretir. Kaynak hashing'inde FLOAT/DOUBLE,
TIMESTAMP, ENUM/SET, GEOMETRY ve VECTOR desteklenmez. Native validator ayrıca
transaction kontrolü, DDL, keyfi nested CALL/UDF ve dış etki üreten yordamları
bu yürütme yoluna almaz. Mutating yordam SQL SECURITY INVOKER olmalıdır.

Owner tüm kaynak sütunlarını SELECT edebilmeli ve yordamın gerçek DML
yetkilerine sahip olmalıdır. Kilitli doğrulamadaki native `FOR UPDATE` ayrıca
ilgili tabloda UPDATE, DELETE veya LOCK TABLES yetkilerinden birini gerektirir.
Yalnız INSERT ve SELECT izni bu özel kilitli onay yürütmesi için yeterli
değildir. Eksik kilit yetkisi native 1142 ile uygulama yazısından önce
reddedilir; üretim kullanıcılarına otomatik izin verilmez.

Manuel parametre bağlaması JSON null ile SQL NULL'u, binary bytes/boş BLOB'u,
Decimal ve unsigned BIGINT hassasiyetini korur. Native parametre alanına
kayıplı dönüşüm hazırlama sırasında reddedilir. Manuel BLOB parametre desteği
provider araç şemasına BLOB desteği eklendiği anlamına gelmez.

PREPARE, bekleyen business write bulunmayan temiz SP bağlamında `:local`
girdisini ve en az 32 karakterlik atanabilir string `RETURNING ID INTO :local`
hedefini destekler. Atanabilirlik/kapasite kalıcı action kaydından önce
doğrulanır. Pending write ile PREPARE açık hata verir; caller transaction'ı
örtük commit edilmez. EXECUTE SP içinden desteklenmez. DEFINER yordamın
yetkisi hazırlanacak işlemin sahibi yerine ödünç alınmaz.

## Hata ve durum okuması

APPROVE yetkisinin, owner EXECUTE/DML izninin, etkin rol üyeliğinin, hedef
tanımının veya kaynak durumunun değişmesi yürütmeyi durdurur. DESCRIBE
`prepared`, `approved`, `denied`, `succeeded`, `stale` veya süre dolduğunda
`expired` durumunu gösterir.

Native hata 61083/public 510003 açık onay gerektirir; 61084/public 510004 eski
onay kanıtını bildirir. Tool yürütücüsünün kesin rollback hatası
61085/public 510005, ilk AI turun `commit_state` bilgisinden ayrı
`action_commit_state`, `call_id` ve asıl hata alanlarını taşır. Bağlantı/COMMIT
şüphesi kesin rollback diye sunulmaz. SDK sözleşmesi
[native-sql-v1.md](native-sql-v1.md) altında tanımlıdır.
