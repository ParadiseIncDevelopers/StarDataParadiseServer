# MySQL üzerinde Decision yazma koruması

`provision.py`, tek tablo ve olaya ait eski `CREATE DECISION RULE` ifadelerinin aşağıdaki bölümünü bir MySQL `BEFORE` trigger’ına çevirir. Girdi, ifadeleri noktalı virgülle ayrılmış UTF-8 dosyasıdır. Kaynak MySQL tablosu InnoDB kullanmalı ve kural sütunları imzalı BIGINT olmalıdır. Çıktıyı trigger oluşturma yetkisi olan hesapla uygulayın.

```sql
CREATE DECISION RULE deny_blocked ON sales.orders FOR INSERT PRIORITY 100
WHEN (NEW.blocked = 1) THEN DENY;
CREATE DECISION RULE minimum_score ON sales.orders FOR INSERT PRIORITY 10
WHEN (NEW.score >= 10) THEN ALLOW;
```

```powershell
python -B compatibility/decision/provision.py order_guards.sql
```

`FOR INSERT` ve `FOR UPDATE` ayrı ayrı desteklenir. `NEW.sütun` değeri imzalı BIGINT sabitiyle `=`, `<>`, `!=`, `<`, `<=`, `>` veya `>=` işleci üzerinden karşılaştırılır. Şema belirtilmemişse `--schema` ile geçerli şemayı verin. `PRIORITY` değeri 0–4294967295 aralığındadır ve yazılmadığında 0 kullanılır. Kurallar büyükten küçük önceliğe ve eşitlikte küçük harfe dönüştürülmüş kural adının artan ASCII bayt sırasına göre değerlendirilir.

Koruma kurallarında ilk TRUE koşulun ALLOW veya DENY sonucu uygulanır. FALSE koşul akışı sonraki kurala bırakır. Daha önce TRUE görülmeden NULL değerle karşılaşılırsa `decision_unknown` döner. TRUE DENY veya tüm koruma koşullarının FALSE olması `decision_denied` verir. Bir kuralın sütun tipi veya tablo motoru değiştiyse `decision_dependency_unavailable` döner. MySQL ALLOW sonrasında normal kısıtlarını uygular. Reddedilen çok satırlı ifade, aynı transaction’daki History olaylarıyla birlikte InnoDB tarafından geri alınır.

## Hesaplanan imaj

```sql
CREATE DECISION RULE calculate_total ON sales.orders FOR INSERT PRIORITY 20
WHEN (NEW.quantity > 0) THEN SET total = NEW.quantity * NEW.unit_price;
```

`SET` imzalı BIGINT bir alanı, `NEW` sütunundan ve ikinci bir `NEW` sütunu veya imzalı BIGINT sabitinden hesaplar. Desteklenen işleçler `+`, `-` ve `*` biçimleridir. Hesaplamalar aynı önerilen satırı okur. Her hedef alan için öncelik sırasındaki ilk TRUE eşleşmesi seçilir. NULL koşullar atlanır. Sonuçlar birlikte `NEW` imajına uygulanır, ardından koruma kuralları hesaplanan imajı okur. NULL sonuç ve taşma MySQL’in normal sütun ve aritmetik davranışına tabidir. Koruma kuralı bulunmayan hesaplama seti, eşleşmeyen satırı geçirir.

## Sınırlar

Kural mevcut satırlara geriye dönük uygulanmaz. Normal `INSERT` ve `UPDATE` yazılarında MySQL trigger yolu kullanılır. Sonradan eklenen ve guard’dan sonra `NEW` imajını değiştiren BEFORE trigger’lar, FK cascade, DDL ve MySQL’in birleşik upsert olay sırası eski Decision sözleşmesi kapsamına alınmadı. Yazma hesabı DML ile sınırlandırılabilir. Trigger yönetimi şema yöneticisinde kalır. [MySQL trigger davranışı](https://dev.mysql.com/doc/refman/8.4/en/create-trigger.html)

Kural adları set içinde benzersiz olmalıdır. İkinci kurulum mevcut trigger’ın üzerine yazmayı denediğinde MySQL adı çakışması verir. Sürüm ve etkinleştirme, `OLD` geçişleri, daha kapsamlı ifadeler, `ACTOR`, alt sorgu, rutin bağımlılıkları, `WHYNOT` ve diğer inceleme komutları bekliyor. Bu çevirici SQL ayrıştırıcısına veya eski Decision RPC’ye eklenmedi. `decision_v1` açılmadı.

## Kabul

Yeni Server’da CA doğrulamalı TLS üzerinden `verify.py` pozitif yazmayı, FALSE ve UNKNOWN reddini, çok satırlı ifadeyi, `INSERT IGNORE`, `INSERT … SELECT`, hazırlanmış ifadeyi, saklı yordamı, şema değişikliğini ve yeniden başlatma sonrasını denetler. Aynı fixture History head’inin reddedilen yazılarda ilerlemediğini doğrular. Yerel kanıt `out/mysql-migration/fork-decision-acceptance.log` ve `fork-decision-acceptance.exit` dosyalarındadır.

Öncelikli set kabulü ALLOW/DENY sırasını, eşit öncelikte kural adını, UNKNOWN durumunu, olay başına tek trigger’ı ve ALLOW sonrası MySQL CHECK kısıtını doğruladı. Yerel kayıt `out/mysql-migration/fork-prioritized-decision-acceptance.log` ve `fork-prioritized-decision-acceptance.exit` dosyalarındadır.

Hesaplama kabulü aynı önerilen imajı, hedef başına ilk TRUE eşleşmesini, NULL ve taşmayı, sonra çalışan koruma kurallarını ve History imajını doğruladı. Yerel kayıt `out/mysql-migration/fork-computed-decision-acceptance.log` dosyasındadır.

Mevcut Workbench editöründeki kabul, geçerli bir `INSERT` gönderdi. Teste özel `SQL SECURITY INVOKER` yordamı ikinci yazmanın gerçek trigger hatasını yakalayıp `decision_unknown` ve değişmeyen History sayacını sonuç tablosunda gösterdi. Yerel kayıt `out/mysql-migration/fork-history-decision-workbench.log` dosyasındadır.
