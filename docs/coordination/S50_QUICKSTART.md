# StarDataParadise çevrimdışı hızlı başlangıç

Örnekleri ayrı bir test instance’ında ve `main.public` bağlamında uygula. SQL adımları yeni tablo, yordam ve model oluşturur. Decision ve CPU ML için ilgili Server özelliklerini kurup etkinleştir.

## Kurulum ve TLS bağlantısı

1. Çevrimdışı arşivin SHA-256 değerini teslimin checksum listesiyle karşılaştır. Arşivi `C:\SDP\paket` gibi kısa bir konuma çıkar ve birlikte gelen üç MSI, `components.json` ve `Setup.ps1` dosyalarını aynı dizinde tut.
2. PowerShell 7’de arşiv dizinine geç. Server kurulacaksa yönetici oturumunu kullan. Uzak Server’a bağlanmak için `workbench-only` veya `cli-only` profili standart kullanıcı kapsamında çalışır. Ortak kurulum için şu komutu kullan.

~~~powershell
Get-FileHash -LiteralPath C:\SDP\paket.zip -Algorithm SHA256
pwsh -NoProfile -STA -File .\Setup.ps1 -Profile complete -AllowDevelopment
~~~

3. Sihirbazda benzersiz instance adı, ayrı program/data/log/temp dizinleri, TLS sertifikası, özel anahtar ve başlangıç yöneticisi seç. Yerel kullanımda loopback ve örneğin `7443` portunu kullan. TLS sertifikasının SAN alanı istemcinin doğrulayacağı adla eşleşmeli. Özel anahtarı Server’da tut ve istemciye doğrulama CA’sının PEM dosyasını ver. Yedekleme için ayrı `backup` dizini tanımla.
4. Kurulu Server dizininden oluşturulan `server.json` ile başlat. Hizmet kurulumunda başlatma komutu yönetim kılavuzunda yer alır. Ham MSI ile kurulan Server için yapılandırma ve ilk yönetici adımları yerel çalışma kılavuzundadır.

~~~powershell
.\StarDataParadiseServer.exe --config C:\SDP\lab\server.json
~~~

Server’ın `LISTENING` satırındaki portu doğrula. Ayrı pencerede kurulu CLI dizininden bağlan. CA dosyası, port ve kullanıcı adı kendi instance’ına ait olmalı. Parola konsolda girilir.

~~~powershell
.\StarDataParadiseCLI.exe --host localhost --port 7443 --user local-admin --ca C:\SDP\lab\ca.pem
~~~

Workbench’te **Connections → Manage profiles…** ile host, port, kullanıcı, CA PEM ve gerekiyorsa TLS adını gir. **Test** bağlantıyı doğrular, **Connect** SQL oturumunu açar. **Help → Offline SQL and PL/SQL help…** uygulama içinde çevrimdışı açılır. CLI’da `--help` seçenekleri, oturum içindeki `\status` ve `\capabilities` bağlantı ve sunucu durumunu gösterir. [Kurulum](../../packaging/SETUP.md) · [Yerel çalışma](../../RUN.md) · [Windows hizmeti](../../ADMIN.md)

## SQL ve PL/SQL

CLI oturumunda veya Workbench SQL editöründe ifadeleri sırayla çalıştır. Sorgu, fonksiyon ve prosedür `17` için `58` döndürür.

~~~sql
CREATE TABLE s50_readings (
    id BIGINT PRIMARY KEY,
    reading BIGINT NOT NULL,
    outcome_value BIGINT NOT NULL
);

INSERT INTO s50_readings VALUES
    (1,1,10),(2,2,13),(3,3,16),(4,4,19),(5,5,22),(6,6,25),(7,7,28),(8,8,31),
    (9,9,34),(10,10,37),(11,11,40),(12,12,43),(13,13,46),(14,14,49),(15,15,52),(16,16,55),
    (17,17,58),(18,18,61),(19,19,64),(20,20,67),(21,21,70),(22,22,73),(23,23,76),(24,24,79),
    (25,25,82),(26,26,85),(27,27,88),(28,28,91),(29,29,94),(30,30,97),(31,31,100),(32,32,103);

SELECT id, outcome_value FROM s50_readings WHERE id = 17;

CREATE FUNCTION s50_adjustment(input_value IN BIGINT) RETURN BIGINT AS
BEGIN
    RETURN input_value * 3 + 7;
END;

CREATE PROCEDURE s50_read(p_key IN BIGINT, p_value OUT BIGINT) AS
BEGIN
    SELECT outcome_value INTO p_value FROM s50_readings WHERE id = :p_key;
END;

SELECT s50_adjustment(17);
CALL s50_read(17, NULL);
~~~

[SQL uyumluluğu](../../SQL_COMPATIBILITY.md) · [PL/SQL](../../PLSQL_LANGUAGE.md)

## Decision ve History

Yönetici oturumunda negatif değeri reddeden kuralı oluştur. INSERT’i ayrı çalıştır. Sunucu `42501` döndürür ve ardından `99` için sonuç boş gelir.

~~~sql
CREATE DECISION RULE s50_nonnegative ON s50_readings FOR INSERT PRIORITY 1
WHEN (NEW.outcome_value < 0) THEN DENY;

INSERT INTO s50_readings VALUES (99,99,-1);
SELECT id FROM s50_readings WHERE id = 99;
HISTORY LOG FOR public.s50_readings LIMIT 25;
~~~

LOG çıktısından ilk INSERT’in commit revizyonunu al. Açık transaction’ı bitirip bir değeri değiştir ve geri yüklemeyi aynı oturumda planla. `<revizyon>` yerine sayı, `<plan_id>` yerine önizleme sonucu ve `<request_token>` yerine 32 küçük harfli onaltılık karakter gir. PowerShell’de `[guid]::NewGuid().ToString('N')` bir token üretir.

~~~sql
UPDATE s50_readings SET outcome_value = 700 WHERE id = 17;
HISTORY RESTORE public.s50_readings AS OF REVISION <revizyon>;
HISTORY RESTORE APPLY '<plan_id>' REQUEST '<request_token>';
SELECT outcome_value FROM s50_readings WHERE id = 17;
HISTORY VERIFY;
~~~

Önizleme veri yazmaz. APPLY yeni bir commit yazar ve sorgu yeniden `58` döndürür. Önizleme ile APPLY arasında bağlantıyı koru. [Decision](../../DECISION.md) · [History ve geri yükleme](../../HISTORY.md)

## CPU model eğitimi

History örneği veriyi geri yükledikten sonra CLI’da aşağıdaki satırları kullan. `<dataset_id>` ve `<job_id>` yerlerine ilgili cevapların kimliklerini gir. Başarılı işin bildirdiği sürümü etkinleştir. Yeni bir modelin ilk başarılı eğitiminde sürüm `1` olur.

~~~text
\rpc dataset create {"sql":"SELECT reading,outcome_value FROM s50_readings ORDER BY id","options":{"features":[{"column":"reading","transform":"numeric","impute":"reject"}],"target":"outcome_value","target_kind":"numeric","split":{"mode":"random","seed":23,"train":60,"validation":20}}}
\rpc model create {"name":"s50_forecast","algorithm":"linear"}
\rpc model train {"name":"s50_forecast","dataset_id":"<dataset_id>","seed":42}
\rpc job watch {"job_id":"<job_id>"}
\rpc model activate {"name":"s50_forecast","version":1}
~~~

~~~sql
EVALUATE MODEL USING '{"name":"s50_forecast","version":1}';
PREDICT MODEL USING '{"name":"s50_forecast","features":{"reading":17}}';
~~~

Test verisindeki ilişki `3 × reading + 7` olduğundan tahmin yaklaşık `58` olur. [Dataset](../../DATASET.md) · [CPU modelleri](../../MODELS.md) · [İş kuyruğu](../../JOBS.md)

## Java dışa aktarma

Workbench kurulumunda **PL/SQL to Java export tools** özelliğini seç. Python 3.12 ve JDK 17 kullan. Kurulu Workbench’in `tools/routine_export` dizinindeki araçlarla saf `s50_adjustment` fonksiyonunun snapshot’ını al. Komutları yazılabilir bir çalışma dizininde çalıştır ve `$exportTools` değişkenini kendi kurulum yoluna göre değiştir. Snapshot parola istemini kullanır. `s50-java` yeni bir dizin olmalı.

~~~powershell
$exportTools = 'C:\SDP\Workbench\tools\routine_export'
python (Join-Path $exportTools 'snapshot.py') --host localhost --port 7443 --ca C:\SDP\lab\ca.pem --user local-admin --routine s50_adjustment --mode standalone --output s50-routine.json
python (Join-Path $exportTools 'emit_java.py') --snapshot s50-routine.json --output s50-java
$sources = @(Get-ChildItem .\s50-java\src\main\java -Recurse -Filter *.java -File | Select-Object -ExpandProperty FullName)
New-Item -ItemType Directory -Path .\s50-java\classes | Out-Null
@'
public final class VerifyS50 {
    public static void main(String[] args) {
        Object result = stardataparadise.exported.Routines.invoke("main.public.s50_adjustment", 17L).returned();
        if (!Long.valueOf(58L).equals(result)) throw new AssertionError(result);
        System.out.println(result);
    }
}
'@ | Set-Content -LiteralPath .\s50-java\VerifyS50.java -Encoding utf8
javac --release 17 -encoding UTF-8 -d .\s50-java\classes @sources .\s50-java\VerifyS50.java
java -cp .\s50-java\classes VerifyS50
~~~

Son komut `58` yazdırır. Snapshot alındıktan sonra derleme ve standalone çağrı ağ bağlantısı olmadan yürür. `manifest.json` yordam revizyonunu ve çıktı SHA-256 değerlerini kaydeder. [Java export ve JDBC bağlı çağrı](../../JAVA_EXPORT.md)

## Bakım ve geri dönüş

| İşlem | Uygulama |
| --- | --- |
| Yedek ve bağımsız geri yükleme | Backup işi tamamlandığında arşivi doğrula. Boş bir hedefe restore yapıp satır, yordam ve model sonuçlarını hedef bağlantısında karşılaştır. Yeni TLS ve yönetici kimliğini ayrı hazırla. [Yedekleme](../../BACKUP.md) |
| Modify, Repair ve Reconfigure | Ortak Setup’ta ilgili işlemi seç. Reconfigure öncesinde veriyi kullanan instance’ı durdur. [Setup yaşam döngüsü](../../packaging/SETUP.md) |
| Uninstall ve yarım kurulumdan kurtarma | Setup’ta kaldırılacak ürünü seç. Server verisi ve yedekler ayrı konumlarında kalır. Yarım kurulumda mevcut Setup durum diziniyle `Recover` çalıştır. [Setup yaşam döngüsü](../../packaging/SETUP.md) |
| Güncelleme ve rollback | Kaynak instance’ı durdur, işleri boşalt, ayrı hedef ve günlük seç. Kesintide aynı günlükle devam et. Hedefe yeni yazma yapılmışsa günlüğü ve iki veri kopyasını incele. [Güncelleme](../../packaging/UPDATE.md) |
| Çevrimdışı storage inceleme | Instance durduğunda `--check-storage` ile bütünlüğü incele. Replay ve checkpoint için bakım adımlarını uygula. [Kurtarma](../../RECOVERY.md) |

[Kapsam ve bilinen sınırlamalar](KNOWN_LIMITATIONS.md) · [Bileşenler ve lisanslar](../../packaging/THIRD-PARTY.md)
