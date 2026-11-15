# Kalıcı iş takvimi

Aralık kuralı `{"every_seconds":30,"misfire":"skip"}` biçimindedir. Aralık 1 ile 31.536.000 saniye arasındadır ve son planlanan UTC zamanına eklenir. Bu zaman, scheduler’ın kalıcı `next_due` kaydıdır.

Günlük kural `{"timezone":"Turkey Standard Time","hour":9,"minute":30,"misfire":"run_once"}` biçimindedir. `UTC` ve Windows zaman dilimi anahtarları desteklenir. Windows takvimi yıl bazında çeviri kurallarını uygular. Saat ileri alındığında seçilen yerel saat yoksa o gün geçilir. Saat geri alındığında aynı yerel saatin ilk UTC karşılığı kullanılır. İlk karşılık geçmişse iş aynı takvim gününde ikinci kez başlatılmaz. Günlük kural 1970–2100 UTC aralığını kullanır ve en çok sekiz yerel gün tarar.

`misfire` alanı isteğe bağlıdır. Varsayılan `skip` kaçırılan çalışmaları atlar. `run_once` birikmiş çalışmaları tek işe indirger. Scheduler kaçırılan çalışmaları kalıcı UTC takvim işareti üzerinden ele alır.

[Windows yıllık zaman dilimi kuralları](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-gettimezoneinformationforyear) · [Dinamik UTC–yerel zaman çevirisi](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-systemtimetotzspecificlocaltimeex)
