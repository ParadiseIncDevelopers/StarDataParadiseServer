# SQL dataset ve ön işleme

`dataset_create` kimliği doğrulanmış oturumun parametreli SELECT sorgusunu tutarlı transaction snapshot’ında yürütür. Sonuç kolonları, tipleri, kaynak revision’ı ve immutable satırlar saklanır. İstek token’ı, aynı gövdeyle tekrarlandığında var olan dataset kimliğini döndürür. Kimliği öğrenmek başka oturumun verisine erişim vermez.

```json
{
  "request_token": "<32 lowercase hex>",
  "sql": "SELECT label, amount, group_id, event_time FROM events WHERE id >= :minimum",
  "parameter_types": {"minimum":"BIGINT"},
  "parameters": {"minimum":{"t":"i64","v":"1"}},
  "options": {
    "features": [{"column":"amount","transform":"standard","impute":"mean"}],
    "target": "label",
    "target_kind": "categorical",
    "split": {"mode":"time","time":"event_time","group":"group_id","train":60,"validation":20}
  }
}
```

`dataset_get` indeksleri sıfırdan başlayan train/validation/test bölümlerini, immutable kaynak örneğini ve fitted dönüşümü döndürür. Random seed aynı build ve kaynak sıralamasına bağlıdır. Stratified bölümleme sınıf kapsamını korur. Gruplar birlikte taşınır. Zaman sıralamasında aynı veya örtüşen zaman aralıkları ayrılmaz. Zaman sütunu tutarlı birimle UTC tamsayısı içerir.

Doldurma, ölçek, min/max ve kategori sözlüğü sadece train bölümünden hesaplanır. One-hot modelinde yeni değerler ayrılmış bilinmeyen kategoriye gider. Validation, test ve sonraki çıkarım aynı dönüşüm formatını kullanır. Özgün tipli satırlar PL/SQL çağrıları için saklanır. Sayısal öğrenme dönüşümü double kullanır. BLOB ve compound kolonlar bu dataset kaynağına alınmaz.

`dataset_plsql_submit` dataset, yetkili saf routine, parametre-sütun eşlemesi ve 1–64 satır indeksi alır. Server immutable satırlardan tipli çağrıları oluşturur. Ayrı worker saklanan routine kaynağını doğrulayıp derler ve gerçek PL/SQL çalıştırır. Tek çıktı scalar, tek bileşik ya da çok OUT alanı record, çok satırlı çağrı result_set döndürür. Worker’ın SQL ve dosya host erişimi kapalıdır. Eğitim ve hesaplama için kaynak okuma transaction’ı worker boyunca tutulmaz.

Güncel SELECT ve EXECUTE yetkileri istek sırasında denetlenir. Dataset örneği veya ilgili job sonucu istendiğinde kaynak SELECT yetkisi yeniden uygulanır. Kaynak silindiyse örnek erişimi durur. Job lineage referansı varken dataset silinemez. Materialized satırlar kaynak revizyonu retention sınırından geçse bile kullanıcıya ait dataset nesnesinde saklanır.

Bir dataset en çok 1024 kaynak satır ve disk record bütçesine sığan materyal içerir. Worker ayrı giriş, çıktı, zaman ve bellek bütçeleri uygular. `dataset_v1` Server wire yeteneğini bildirir.
