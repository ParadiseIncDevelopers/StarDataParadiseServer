# CPU MLP

Model formatı 1, tam bağlantılı bir ikili sınıflandırma ağıdır. Girişte en çok 64 özellik ve 1–3 gizli katman bulunur. Gizli katman boyutu 1–32 arasındadır. En çok 2048 parametre ve toplam 1024 eğitim ile validation satırı kullanılır. Girdi değerleri sonlu olmalıdır. Özellik ve parametre büyüklüğü için sınır `10^6`’dır.

```json
{
  "hidden_layers": [8],
  "activation": "tanh",
  "loss": "bce",
  "optimizer": "adam",
  "learning_rate": 0.01,
  "batch_size": 16,
  "epochs": 120,
  "patience": 20,
  "min_delta": 0.000001
}
```

Gizli katmanda `tanh`, `relu` ve `sigmoid` seçilebilir. Çıkış sigmoid ile olasılık verir. Eğitim, logit üzerinden sayısal olarak kararlı ikili çapraz entropi hesaplar. Performans ölçümünde eşik 0,5’tir. Eğitim bölümü hem 0 hem 1 içerir. Validation bölümü boş olamaz. Test bölümü ağırlık veya erken durdurma seçimine katılmaz.

Başlangıç Glorot uniform dağılımını ve sıfır bias’ı kullanır. Seed, aynı derleme ve veri sırası için mini batch permütasyonlarını tekrar üretir. Batch boyutu en çok 256 ve epoch en çok 1000’dir. CPU çalışma bütçesi 300 milyon modellenen bağlantı işlemiyle sınırlıdır. SGD düz mini batch gradyanı uygular. Adam katsayıları 0,9 ve 0,999, epsilon `10^-8`’dir. Sayısal backend, ürünün sabitlenmiş MSVC v145 C++20 çalışma zamanı ve standart matematik işlevleridir.

Erken durdurma, validation kaybı `min_delta` kadar ilerlemeden `patience` epoch geçtiğinde çalışır. Çıktı gerçekten görülen en düşük validation kaybının ağırlıklarını kullanır ve ilk ile seçilen eğitim/validation kayıplarını gösterir. `best_epoch: 0`, başlangıç ağırlıklarının doğrulanmış en iyi sonuç olduğu anlamına gelir.

Parametreler katman sırasıyla düz dizidir. Her çıkış nöronu giriş ağırlıklarını ve en sonda bias’ını taşır. Checkpoint callback’i tamamlanmış epoch sınırında çağrılır. Durum, güncel ve en iyi ağırlıkları, Adam momentlerini ve optimizer adımını, `mt19937_64` seed ve çekim sayısını, epoch ile sıfır batch konumunu, validation sayaçlarını ve kullanılan iş bütçesini içerir. Dış kayıt, aynı dataset kimliğini ve artifact checksum’ını doğrulamalıdır. Model artifact’ı çıkarım için seçilen ağırlıkları ve ölçümleri saklar.
