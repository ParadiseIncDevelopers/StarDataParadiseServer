# Kullanıcının seçtiği portun korunması

22 Kasım 2026. Kullanıcı son kullanılan sunucu portunun bağlantı profiline
ayarlanmasını ve sonraki işlemlerde başlangıçta seçtiği portun korunmasını
istedi. 3197 ve 3334 örnektir; bütün kurulumlar için sabit port seçimi değildir.

Mevcut yerel kurulumda son kabul edilmiş native sunucu portu **41754** idi
([önceki kurulum kaydı](LOCAL_AI_ACTIVATION_2026_11_21.md)). Kalıcı sunucu
argümanları ve worker SQL bağlantısı aynı portu kullanıyordu. Workbench `test`
profilinde `port` girdisi yoktu; `hostIdentifier` içinde `%port%` kalmıştı.
Profili yeniden açmak 3197 sürücü varsayılanını gösteriyordu.

`test` profili uygulamanın kendi profil yöneticisinden `127.0.0.1:41754`
olarak kaydedildi. Kaydedilen XML'de port `int` tipindedir ve `hostIdentifier`
`Mysql@127.0.0.1:41754` olur. Uygulama normal biçimde tamamen kapatılıp güncel
EXE yeniden açıldı; profilde 41754 korundu ve **Bağlantı sınaması başarılı**
sonucu alındı. Sunucu mevcut veri/UUID ve binary hash denetimi yapan yerel
başlatıcıyla başlatıldı; initialize veya kullanıcı SQL verisi değişikliği yapılmadı.

Workbench ve Server `AGENTS.md` dosyalarına kalıcı port tercihini koruma kuralı
eklendi. Kullanıcının seçtiği 3197, 3334 veya başka geçerli port; güncelleme,
yeniden başlatma, worker ayarı ve sonraki EXE teslimlerinde varsayılanla ya da
izole test portuyla değiştirilmez. İlk kurulumda seçim yoksa varsayılan
önerilebilir. Çelişen ayarlarda ilgili gerçek sunucu/profil doğrulanır.

Yerel `out/deployment-20261121/Start-Server.ps1` artık dinleyici denetimi ve
hata mesajındaki portu `final-arguments.json` içindeki `--port=` girdisinden
okur. Tek, 1..65535 aralığında geçerli port zorunludur. `-ValidateOnly`
binary hash, veri kimliği ve portu başlatma yapmadan doğrular. Kurulu portu
veya worker ayarını değiştirmek gerekmedi; ürün binary'si yeniden derlenmedi.

Kanıtlar kardeş Workbench deposunda `out/port-persistence-20261122/` altındadır:

| Sonuç | Kontrol |
| --- | --- |
| PASS | `profile-after-save.json`: profil portu 41754, tip `int`, doğru host identifier. |
| PASS | `ui-verification.json`: tam uygulama yeniden açılışı, portun yeniden yüklenmesi ve gerçek GUI bağlantı sınaması. |
| PASS, exit 0 | `Start-Server.ps1 -ValidateOnly`: binary/veri kimliği doğrulandı; `server-launcher-validation.json`. |
| PASS, exit 0 | PowerShell `Parser.ParseFile`: başlatıcıda sözdizimi hatası 0. |
| PASS | `final-verification.json`: profil, sunucu ve worker portları 41754; Workbench EXE hash'i önceki teslimle aynı. |
| PASS | Workbench ve Server `git diff --check`. Önceden var olan README, ilerleme ve PyPI değişiklikleri korundu. |
| NOT_RUN | Yeni ürün derlemesi, temiz kurulum, gerçek OpenAI/Claude çağrısı veya worker sağlık kabulü bu ayar değişikliğinin testi değildir. |

İlk GUI sınamalarında formdaki değişiklik henüz kalıcı olarak doğrulanmadan
3306 hedefli bağlantı hatası alındı. Bu başarısız sonuçlar kabul sayılmadı;
profil kaydı ve tam yeniden açılıştan sonraki gerçek GUI sınaması geçti.
İlk salt okunur XML sorgusunda eksik port düğümüne erişim hatası oldu; eksik
değeri destekleyen sorguyla düzeltildi. Bazı `rg` yolları/PowerShell bayrakları
ilk tanılamada yanlış verildi ve doğru dosya yollarıyla tekrar okundu.
Sandbox içi loopback yoklamaları timeout verdi; bunlar gerçek sunucunun kapalı
olduğuna kanıt sayılmadı. Nihai GUI bağlantı testi başarılıdır.
