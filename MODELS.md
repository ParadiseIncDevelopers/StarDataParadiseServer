# CPU model registry

Etap 25, hazır dataset’in eğitim bölümünden dört algoritmayı ayrı Windows worker’da eğitir. Linear, logistic, CART tree ve random forest kullanılabilir. Dataset preprocessing sürümü ve train/validation/test bölümleri model girdisine bağlıdır. Eğitim, validasyon ve held-out test metrikleri ayrı ölçülür. Worker sonucu kalıcı sürümü yayımlar. Yeni sürüm etkinleştirilince PREDICT bir immutable sürüm ve aynı fitted dönüşümü kullanır.

```sql
CREATE MODEL USING '{"request_token":"<32 hex>","name":"sales","algorithm":"linear"}';
TRAIN MODEL USING '{"request_token":"<32 hex>","name":"sales","dataset_id":"<dataset id>","seed":42}';
EVALUATE MODEL USING '{"name":"sales","version":1}';
ALTER MODEL USING '{"request_token":"<32 hex>","name":"sales","version":1}';
PREDICT MODEL USING '{"name":"sales","features":{"amount":120}}';
```

Aynı gövdeler `model_create`, `model_train`, `model_evaluate`, `model_activate`, `model_predict` üzerinden gönderilebilir. `model_status` aktif sürümü ve immutable sürüm manifestlerini gösterir. Eğitim talebi `job_id` döner. Job sonucunun sürümü ve SHA-256 değeri aynı transaction’da yayımlanır. Aktivasyon öncesi artifact hash’i ve feature genişliği doğrulanır. Predict kullanılan sürümü döndürür.

Regression MAE, MSE ve RMSE üretir. Logistic/tree/forest ikili sınıf olasılığı, 0.5 eşiği ve confusion matrix, accuracy, precision, recall ve F1 üretir. Paydası sıfır metrik `null` ve neden koduyla gösterilir. Doğrudan model inceleme ve tahmin, model sahibi ya da yönetici ve güncel dataset kaynak izinleriyle sınırlıdır.

Eğitim CPU tabanlıdır. Worker kullanılan algoritma, seed, gerçek logical CPU thread sayısı, tip/şekil ve metrikleri kaydeder. Registry snapshot dataset, run kimliği, options ve runtime formatını saklar. Aynı CPU/runtime davranışında sonuçları karşılaştırırken [linear/logistic sözleşmesini](ml/LINEAR_MODELS.md) ve dataset preprocessing kurallarını kullan.

## Denetimsiz modeller

| Algoritma | Eğitim çıktısı | Yeni kayıt | Değerlendirme |
| --- | --- | --- | --- |
| `kmeans` | Seeded k-means++ ile seçilen merkezler, eğitim etiketleri ve inertia | En yakın merkez; eşitlikte düşük etiket | Her bölümde tüm satırların Euclidean silhouette değeri |
| `dbscan` | Yoğunluk tabanlı kümeler ve `-1` gürültü etiketi | Açık unsupported hatası | Eğitimde gürültü hariç silhouette; validation ve test metrikleri unavailable |
| `pca` | Eğitim merkezlemesi, bileşenler ve sample varyansı | Bileşen skorları ve dönüştürülmüş özellik uzayında reconstruction | Her bölümde reconstruction RMSE |

Denetimsiz modellerde dataset hazırlarken hedef sütun belirtme. Eğitim isteğindeki `options` K-Means için `k`, isteğe bağlı `max_iterations` ve `tolerance`; DBSCAN için `eps` ve `min_points`; PCA için `components` alır. K-Means boş kümenin önceki merkezini korur. PCA'nın varyans oranı toplam varyans sıfırsa `null` döner. Silhouette iki veya daha fazla geçerli küme gerektirir; tek örnek içeren kümenin örnek skoru sıfırdır. PCA'nın sayısal sözleşmesi [PCA.md](ml/PCA.md) dosyasındadır.

`model_compare` gövdesi `{"runs":[{"name":"m","version":1},{"name":"m","version":2}],"split":"test"}` biçimindedir. Her çalışma için dataset kimliği, metrik ailesi ve bölüm metrikleri döner. Karşılaştırılabilirlik aynı değişmez dataset, bölüm ve tanımlı metrik gerektirir. K-Means tüm satırları ve DBSCAN yalnız gürültü dışı satırları kapsadığı için ayrı metrik aileleri kullanılır. Sonuçta `different_dataset`, `different_metric` veya `undefined_metric` nedeni bulunabilir. İki DBSCAN deneyi eğitimde farklı satırları gürültü saymışsa `different_sample_scope` döner.

## Derin öğrenme

`mlp` algoritması aynı veri ve model hazırlama akışında ikili sınıflandırma yapar. Eğitim seçenekleri, limitler ve minimal Server derlemesi [MLP sözleşmesinde](ml/DEEP_LEARNING.md).

## Checkpoint ve model yaşam döngüsü

MLP eğitim isteğine `pause_after_epoch` eklenirse worker tamamlanan epoch sınırında eğitim durumunu kaydeder. Job `paused` olur. `model_checkpoint` ile alınan `job_id` ve `checksum`, `model_resume` isteğinde model adı ve yeni `request_token` ile gönderilir. Resume yeni bir job alır ve aynı dataset, bölümler, seed, çalışma zamanı, ağırlıklar, optimizer, RNG ve iterator durumunu kullanır. İzinler, SHA-256 ve dataset bağlamı yeniden doğrulanır. Tamamlanan eğitim, yeni sürüm yayımlar ve mevcut aktif sürümü korur.

`model_activate` doğrulanmış bir sürümü seçer. `model_rollback` etkin sürümden daha eski bir sürüm gerektirir. `model_deactivate` aktif işaretçiyi kaldırır. `model_delete` etkin olmayan ve History pin’i bulunmayan bir sürümün artifaktını kaldırır. Silinen sürüm numarası yeniden kullanılmaz. Yeni aktivasyon mevcut aktif sürümle uyumlu ham özellik adlarını, tiplerini ve sınıf eşlemesini gerektirir.

`model_devices` Windows CUDA sürücüsünü, cihaz seçimini ve toplam aygıt bellek bütçesini sorgular. GPU eğitim backend’i bu paketle gelmez. `device:"gpu"` isteği kod 1004 döndürür. Aynı isteğe `allow_cpu_fallback:true` eklendiğinde artifact `hardware.backend:"cpu"` ve fallback gerekçesini kaydeder. CUDA ön kontrolü NVIDIA Driver API’yi kullanır. [NVIDIA Device Management](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__DEVICE.html)
