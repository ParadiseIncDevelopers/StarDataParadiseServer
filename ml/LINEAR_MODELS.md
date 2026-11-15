# Lineer ve lojistik CPU modelleri

İki algoritma en çok 1024 satır ve 64 özellik kullanır. Girdi değerleri sonlu olmalı ve büyüklüğü `10^100` sınırını aşmamalıdır. Eğitim özelliklerini merkeze alır ve ölçekler. Artifact hem asıl birimlerdeki katsayıları hem de sayısal açıdan güvenli çıkarım için ölçekli katsayıları saklar. Çıkarım eğitim dönüşümünü tekrar kullanır.

**Lineer regresyon.** `ridge` varsayılan olarak sıfırdır. Çözüm, sütun pivotlu Householder QR ile kare hata toplamını düşürür. Pozitif `ridge`, standartlaştırılmış eğimlere ceza uygular. Sabit terim cezaya girmez. Rank eksikliğinde çözüm serbest yönlerin katsayılarını sıfıra sabitler.

**İkili lojistik sınıflandırma.** Eğitimde hem sıfır hem bir sınıfı bulunmalıdır. Optimizasyon kararlı log kaybı ve azalan adım denetimi kullanır. Varsayılan `iterations` 512, `learning_rate` 1 ve `l2` 0,0001’dir. İterasyon üst sınırı 2000’dir. Kaynak hesabı eğitim ve adım değerlendirmelerini birlikte sınırlar. Tahmin bir olasılık döndürür. Sınıf eşiği 0,5’tir.

Katsayı uygunluğu ve test metrikleri kullanılan dataset ve ayrımla birlikte değerlendirilir. Aynı derleme ve aynı sıradaki veri üzerinde eğitim deterministiktir.
