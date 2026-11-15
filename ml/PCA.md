# PCA CPU modeli

PCA en az iki eğitim satırı ve 1–64 özellik alır. En çok 1024 satır kullanılabilir. `components` bir ile özellik sayısı arasındadır. Girdiler sonlu olmalıdır. Model eğitim satırlarının ortalama ve örnek kovaryansını kullanır. Simetrik Jacobi dönüşümleri özvektörleri büyük varyanstan küçüğe sıralar. Eşit durumda algoritmanın sabit sırası korunur.

Artifact, görünen fiziksel ortalama `center`, sayısal merkezleme için `origin` ve `mean_offset`, eksen dizisi `components`, `explained_variance`, `explained_variance_ratio` ve `total_variance` alanlarını içerir. Dönüşüm saklanan eksenleri kullanır. Test veya yeni çıkarım verisi üzerinden ortalama ya da eksen yeniden öğrenilmez. Varyans oranı bütün eğitim eksenlerine göre hesaplanır. Varyans tamamen sıfırsa oranlar `null` ve neden `zero_total_variance` olur.

Geri kurma, skorları saklanan eksenler boyunca toplar ve eğitim merkezini ekler. Tüm eksenlerle geri kurma sayısal tolerans içinde özgün vektörü verir. Daha az eksenle geri kurmada atılan eksenlerdeki bilgi kaybolur.
