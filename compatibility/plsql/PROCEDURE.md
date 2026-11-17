# Yerel BIGINT PL/SQL yordamı

`scalar_procedure.py`, eski `CREATE PROCEDURE ... AS BEGIN ... END;` biçimini çağıranın yetkileriyle çalışan yerel MySQL yordamına dönüştürür. Oluştururken `CREATE ROUTINE`, çağırırken `EXECUTE` ve yordamın okuduğu tablo için `SELECT` izinleri gerekir. Yordam işlem açıp kapatmaz. InnoDB sorgusu çağıranın işlemindeki değişiklikleri okuyabilir.

```sql
CREATE PROCEDURE subtotal_for(p_id IN BIGINT, subtotal IN OUT BIGINT, quantity OUT BIGINT) AS
BEGIN
    quantity := (SELECT i.quantity FROM items AS i WHERE i.id = :p_id);
    subtotal := subtotal + quantity;
END;
```

Tanımı bir UTF-8 dosyasına kaydedip derlenen Shell ile hedef şemaya bağlı olarak çalıştırın. Bu örnek Server TLS CA’sını ve kullanıcı parolasını Shell’in normal bağlantı seçeneklerinden alır.

```powershell
mysqlsh --mysql --uri 'sdp_app@127.0.0.1:3306/inventory' --ssl-mode=VERIFY_IDENTITY --ssl-ca mysql-ca.pem --py --file compatibility/plsql/scalar_procedure.py subtotal.sql
```

Native `CALL` içindeki `OUT` ve `INOUT` konumlarında MySQL oturum değişkenleri kullanılır.

```sql
SET @subtotal = 10;
CALL inventory.subtotal_for(1, @subtotal, @quantity);
SELECT @subtotal, @quantity;
```

Alt küme `BIGINT IN`, `OUT`, `IN OUT`, `NULL`, işaretli tamsayı sınırları, `+`, `-`, `*`, parantezler ve `OUT` veya `IN OUT` atamalarını kapsar. Örnekteki bağlı `SELECT` biçimi tek bir işaretli `BIGINT` sütunu okur. Eşleşme yoksa `NULL`, birden fazla eşleşme varsa MySQL skaler alt sorgu hatası döner.

`FOR i IN alt..üst LOOP` sınırları bir kez değerlendirir ve iki ucu da kapsar. Ters aralık boş kalır ve `NULL` sınır hata verir. Döngü sayacı atamaya kapalıdır. `CONTINUE WHEN` kalan ifadeleri atlar. `IF ... THEN` ve isteğe bağlı `ELSE` kolu skaler karşılaştırma kullanır. Her çağrının tek bir 100.000 adımlık sınırı vardır. Sayaç, yürütülen ifadeler ve döngü turları için azalır. Sınır aşılırsa çağrı hata verir.

İç içe `FOR`, diğer döngüler, yordam içinden yazma, cursor, `CREATE OR REPLACE` ve eski wire OUT sonuç seti bu aşamada kapsam dışıdır.
