# Watchdog — POSIX Process İzleme ve Yönetim Sistemi

Bu proje, Linux üzerinde C ve POSIX mekanizmaları kullanarak en az iki worker child process'i izleyen bir `watchdog` uygulamasıdır. Beklenmedik sonlanmalar sınırlı sayıda yeniden başlatılır; sinyaller ana kontrol döngüsünde işlenir; önemli olaylar zaman damgasıyla `watchdog.log` dosyasına yazılır.

## Gereksinimler

- Linux
- GCC
- GNU Make
- Bash (entegrasyon testleri için)

## Derleme

Komutları depo kök dizininde çalıştırın:

```bash
make clean
make
```

Başarılı derleme sonunda depo kökünde iki executable oluşur:

- `./watchdog` — ana izleme programı
- `./worker` — `fork()` sonrasında `exec()` ile çalıştırılan worker

Uyarıları görmek için varsayılan derleme seçenekleri `-Wall -Wextra -Wpedantic` kullanır.

## Çalıştırma

```bash
./watchdog
```

Program başlangıçta watchdog PID'sini terminale yazdırır. Worker oluşturma, sonlanma, restart, timer ve shutdown olayları `watchdog.log` dosyasına eklenir. Programı terminalden `Ctrl+C` ile veya başka bir terminalden `kill -TERM <WATCHDOG_PID>` komutuyla kontrollü kapatabilirsiniz.

## Desteklenen sinyaller

| Sinyal | Davranış |
|---|---|
| `SIGUSR1` | Worker PID ve durum snapshot'ını log dosyasına yazar. |
| `SIGINT` | Kontrollü shutdown başlatır. |
| `SIGTERM` | Kontrollü shutdown başlatır. |
| `SIGCHLD` | Sonlanan child'ların ana akışta `waitpid(..., WNOHANG)` ile toplanmasını tetikler. |
| `SIGALRM` | Periyodik kontrol bayrağını tetikler. |

Watchdog PID'sine sinyal göndermek için:

```bash
kill -USR1 <WATCHDOG_PID>
kill -TERM <WATCHDOG_PID>
```

Beklenmedik child sonlanmasını test ederken PID'yi önce `watchdog.log` içindeki `process_created` veya `process_restarted` kaydından doğrulayın. Ardından yalnızca o worker PID'sine sinyal gönderin:

```bash
kill -KILL <WORKER_PID>
```

`SIGKILL` ile watchdog PID'sini yanlışlıkla hedeflemeyin. Geniş kapsamlı `pkill` veya `killall` kullanmayın.

## Logger davranışı

Logger API'si `include/logger.h` içinde tanımlıdır:

- `logger_init(path)`: log dosyasını append modunda açar. `FD_CLOEXEC` ayarı, dosya tanımlayıcısının `exec()` edilen worker'a sızmasını önler.
- `logger_event(...)`: timestamp, olay, worker, PID ve ayrıntıları tek satır olarak yazar ve `fflush()` yapar.
- `logger_status(...)`: worker slot'larının PID, durum, restart sayısı ve son çıkış bilgilerini yazar.
- `logger_close()`: log akışını flush edip kapatır.

Logger yalnızca normal program akışından çağrılmalıdır; signal handler içinden çağrılmamalıdır. Zaman damgaları yerel saat diliminde `YYYY-MM-DD HH:MM:SS` biçimindedir.

## Testler

Yedi ana senaryoyu ve ek shutdown timeout senaryosunu çalıştırmak için:

```bash
make test
```

Script yalnızca kendi başlattığı watchdog PID'sini ve o watchdog'un loglarından doğruladığı worker PID'lerini kullanır. Var olan `watchdog.log` dosyasını test öncesi yedekler ve script tamamlandığında geri yükler. Teste ait log kopyaları `/tmp/watchdog-tests.*` altında tutulur; script sonuçta bu dizini yazdırır.

Test senaryoları:

1. Normal çalışma ve iki worker'ın başlatılması.
2. Worker'ların süreli modda normal çıkması ve restart edilmemesi.
3. Beklenmedik `SIGKILL` sonrası worker'ın yeniden başlatılması.
4. Bir worker slot'u için üç restart sınırının aşılmaması.
5. `SIGUSR1` ile durum raporu oluşturulması.
6. `SIGTERM` ve `SIGINT` ile kontrollü shutdown.
7. Log kayıtlarının timestamp biçiminin doğrulanması.
8. Ek kontrol: SIGTERM'i işleyemeyen (SIGSTOP ile durdurulmuş) worker için timeout sonrasında SIGKILL escalation.

Normal sonlanma testi için yalnızca test amaçlı `WATCHDOG_WORKER_DURATION` ortam değişkeni kullanılır. Bu değişken verilmediğinde worker'lar sinyal gelene kadar çalışır.

## Mimari

- `src/main.c`: başlatma, ana döngü ve shutdown koordinasyonu (Student 1).
- `src/signal_manager.c`: `sigaction()` ve signal-safe bayraklar (Student 1).
- `src/process_manager.c`: `fork()`, `exec()`, PID tablosu, `waitpid()`, restart ve shutdown (Student 2).
- `workers/worker.c`: worker yaşam döngüsü (Student 2).
- `src/logger.c`: timestamp'li olaylar ve status raporu (Student 3).
- `tests/test_scenarios.sh`, `Makefile`, bu README: test ve build altyapısı (Student 3).

Her worker slot'u en fazla üç otomatik restart girişimine sahiptir. İlk başlatma restart sayısına dahil değildir. Normal `0` çıkışında restart yapılmaz. Shutdown başladıktan sonra restart yapılmaz. Varsayılan shutdown timeout'u 5 saniyedir.

## Proje standartları

- Worker kaynağı: `workers/worker.c`
- Worker executable: repo kökünde `./worker`
- Watchdog executable: repo kökünde `./watchdog`
- Log dosyası: repo kökünde `watchdog.log`
- Çalıştırma dizini: repo kökü
