# Decision kuralları

Decision kuralları bir tabloya ve yazma olayına bağlıdır. Kuralı yalnız oturum yöneticisi oluşturur ve etkinleştirir. İlk sürüm yazıldığı transaction içinde etkin olur. Aynı adla yeniden CREATE yeni, değişmez bir sürüm ekler. Etkin işaretçi ALTER ile ileri veya geri alınabilir. Katalog imajları aynı commit’in History kaydına dahil edilir.

```sql
CREATE DECISION RULE paid_order ON orders FOR UPDATE PRIORITY 100
WHEN (NEW.phase <> 'approved'
      OR (OLD.phase = 'draft'
          AND EXISTS (SELECT id FROM payments WHERE id = :NEW.id)))
THEN ALLOW;

ALTER DECISION RULE paid_order ON orders ACTIVATE VERSION 2;

CREATE DECISION RULE calculate_total ON orders FOR INSERT PRIORITY 10
WHEN (NEW.quantity > 0)
THEN SET total = NEW.quantity * NEW.unit_price;
```

Olaylar INSERT, UPDATE, DELETE ve WRITE. Öncelik büyükten küçüğe gider. Eşitlikte katalog kural adının byte sırası kullanılır. Aynı kolona yazan hesaplamalardan ilk TRUE eşleşmesi kullanılır. Hesaplamalar aynı önerilen satırı okur, ardından koruma kuralları hesaplanan satır üzerinde değerlendirilir. Uygulanabilir bir koruma kuralı varsa ilk TRUE eşleşme sonucu belirler. Daha yüksek öncelikteki UNKNOWN veya hiçbir korumanın eşleşmemesi yazmayı reddeder. Koruma dışındaki hesaplama kuralı TRUE koşulunda çalışır.

OLD ve NEW yazma olayının önceki ve önerilen satırını temsil eder. Var olmayan satır imajı NULL değerlerinden oluşur. ACTOR, kimliği doğrulanmış oturum adıdır. EXISTS alt sorgusunda `:OLD.kolon` ve `:NEW.kolon` kullanılır. Koşullar ve hesaplamalar tipli PL/SQL IR üzerinden değerlendirilir. Yalın, yan etkisiz skaler PL/SQL fonksiyonları ve snapshot içindeki salt okunur EXISTS desteklenir. Kullanıcı bağlama parametreleri, sequence ilerletme ve yazma yeteneği olan fonksiyonlar kural içinden reddedilir.

Decision, BEFORE trigger’lardan sonra ve CHECK/FK/unique kontrolleri ile heap/index yazmasından önce çalışır. AFTER trigger hesaplanmış imajı alır. SQL, PL/SQL, trigger kaynaklı yazmalar ve History restore/revert aynı yolu kullanır. DDL ve DML mevcut transaction lease’i içinde sıraya girer. Kural şema ve rutin bağımlılığının stable kimliği ile sürümünü kaydeder. Değişen bağımlılık mevcut kuralın yeniden değerlendirilmesini gerektirir ve yazma güvenli biçimde durur. Bağımlı bir nesneyi silme girişimi reddedilir.

`decision_enabled: false` yapılandırması Decision özelliğini kapatır. Mevcut kuralları taşıyan tabloya yazma `module_unavailable` döner. Normal yazma hataları değerleri, özel alanları ve alt sorguyu açıklamayan sabit `decision_reason` kodları taşır. Kuralın geçmiş imajı güncel History ve DDL izinleriyle okunur.

## Açıklama ve simülasyon

```sql
DECISION WHYNOT USING '{"target":"orders","event":"update","row_id":"8589934593","proposed":{"phase":{"t":"text","v":"approved"}}}';
DECISION WHATIF USING '{"target":"orders","event":"insert","proposed":{"quantity":{"t":"i64","v":"3"}}}';
```

`EXPLAIN`, `WHYNOT`, `WHATIF` ve `RECHECK` bir yazma önerisini aynı Decision değerlendiricisiyle salt okunur biçimde denetler. Uygulama güncel satır, yetki ve kuralı kendi transaction’ında tekrar kontrol eder. Önizleme veri, History, trigger veya job etkisi üretmez. Sonuç snapshot revision’ını, uygunluğu, güvenli neden kodunu, kural/sürüm kimliğini, koşul düğümlerini ve ölçülen süreyi döndürür. Gerçek kısa devre sonucu atlanan düğümlerin değeri ve çalışma süresi bulunmaz. Kaynak SQL ve operand değerleri çıktıya alınmaz.

Oturumun tabloyu ve kullanılan veri bağımlılıklarını okuma yetkisi olmalıdır. Kullanılan PL/SQL fonksiyonları EXECUTE ister. Tarihsel inceleme ek HISTORY yetkisi ister ve o revision’daki etkin kuralı kullanır. Harici sorgu girdileri için tarihsel ve canlı veri imajlarının eşitliği doğrulanır. Gerekli imaj, retention veya uyumlu bağımlılık yoksa `dependency_snapshot_unavailable` döner.

`TEST`, aynı snapshot üzerinde iki değişmez kural sürümü ve en çok 16 senaryo alır. Sonuç, her senaryo için önceki ve aday davranışını ve değişen sonuç sayısını verir. `VALIDATE`, kural SQL’ini katalog ve bağımlılıklarıyla doğrular. Native SDK’da create, validate, activate ve inceleme işlemleri `sdp_decision_command` ve `decision_explain_v1` ile kullanılabilir.
