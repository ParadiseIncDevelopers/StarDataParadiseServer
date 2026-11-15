# Dataset bölme ve ön işleme

Hazırlayıcı, `columns` ad listesi ve `rows` değer dizileri olan bir kaynak alır. Bir hücre sonlu sayı, metin, boolean veya `null` olabilir. Dataset en az üç ve en çok 1024 satır alır. En çok 128 kaynak sütunu ve 256 çıktı özelliği üretilebilir. Sayısal hesaplar double ile yapılır. Bir değer `10^100` büyüklüğünü geçerse hazırlama reddedilir.

```json
{
  "features": [
    {"column": "amount", "transform": "standard", "impute": "mean"},
    {"column": "city", "transform": "onehot", "impute": "mode"}
  ],
  "target": "paid",
  "target_kind": "categorical",
  "split": {"mode": "stratified", "seed": 41, "group": "customer_id", "train": 60, "validation": 20}
}
```

Kalan oran test kümesine ayrılır. Varsayılan oranlar 60, 20 ve 20’dir. Bölme her kaynak indeksini bir kez kullanır. Grup verilirse grup bütün halinde tek kümeye atanır ve oranlar bölünebilir birim sayısına göre yaklaştırılır. Stratified bölmede her sınıf eğitimde yer alır. Bir grupta farklı hedef sınıfları bulunursa veya üç boş olmayan küme oluşmazsa istek reddedilir. Bu yüzden küçük veri setlerinde istenen oranlar yaklaşıktır.

`time` modu, `time` alanında verilen sütundaki işaretli 64 bit zaman damgalarını sıralar. Zaman damgaları aynı birimde olmalıdır ve karşılaştırmada double’a çevrilmez. Eşit zamanlar ve birbirine değen grup zaman aralıkları aynı bölme biriminde kalır. Eğitimdeki tüm zamanlar validation zamanlarından, validation zamanları da test zamanlarından öncedir.

`numeric` giriş ölçeğini korur. `standard`, eğitim ortalaması ile popülasyon standart sapmasını kullanır. `normalize`, eğitim minimumu ile maksimumu arasındaki aralığı kullanır. Sabit sütunda ölçek birdir. `mean`, `zero`, `mode` ve `reject` eksik değer politikalarıdır. `onehot` yalnız `mode` veya `reject` kabul eder. Doldurma, ölçek ve kategoriler yalnız eğitim indeksleri üzerinden öğrenilir. Eğitimde tüm değeri eksik olan sütun reddedilir. Eşit sıklıkta kategorik mod için JSON skaler anahtarının ilk sırası seçilir. Dönüşüm, eğitimde bulunmayan bir özellik kategorisini sabit bir ayrı sütuna kodlar.

Çıktı `split` indekslerini, değişmez `transforms` tarifini, kaynak sırasındaki `features` matrisini, `target` değerlerini ve varsa eğitimden öğrenilen `target_classes` listesini içerir. Kategorik hedef başka kümelerde eğitimde görülmemiş sınıf içerirse hazırlama reddedilir. Çıkarım, adlandırılmış değerlerden `transform_features` ile aynı sütun sırasını ve dönüşümleri uygular. Aynı satır sırası, seed ve ürün derlemesi tekrar üretim kapsamıdır.
