/*
 * main.c - Student 1
 *
 * watchdog ana programı: başlatma sırası, ana kontrol döngüsü ve graceful
 * shutdown koordinasyonu.
 *
 * Bu dosya diğer modüllerin iç işleyişine girmez; yalnızca ortak API'lerini
 * çağırır:
 *   - signal_manager.h  (Student 1): sinyal bayrakları
 *   - process_manager.h (Student 2): worker başlatma, reaping, restart, timer
 *   - logger.h          (Student 3): zaman damgalı loglar ve durum raporu
 */

#include "signal_manager.h"
#include "process_manager.h"
#include "logger.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Ekip standardı (rehber 4.5) */
#define WATCHDOG_LOG_PATH "watchdog.log"

/* Periyodik kontrol aralığı (rehber 5.4: önerilen 1 saniye) */
#define PERIODIC_CHECK_INTERVAL_SECONDS 1u

/* Ana döngü her turda en fazla bu kadar uyur. Sinyal gelirse nanosleep()
 * EINTR ile hemen döner; yani bu süre sadece "boşta bekleme" süresidir. */
#define MAIN_LOOP_SLEEP_NS (100L * 1000L * 1000L) /* 100 ms */

static const char *shutdown_signal_name(int signo)
{
    switch (signo) {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
    default:
        return "unknown";
    }
}

/*
 * SIGUSR1 isteği: process manager'dan durum tablosunun bir kopyasını
 * (snapshot) al ve logger'a yazdır. Bu iş handler'da değil, burada (normal
 * program akışında) yapılır.
 */
static void handle_status_request(void)
{
    process_info_t items[PROCESS_MANAGER_MAX_CHILDREN];
    size_t count;

    logger_event("STATUS_REQUEST", "watchdog", getpid(), "SIGUSR1 received");

    count = process_manager_get_status(items, PROCESS_MANAGER_MAX_CHILDREN);
    if (logger_status(items, count) != 0)
        fprintf(stderr, "watchdog: failed to write status report\n");
}

/*
 * Ana kontrol döngüsü. Shutdown isteği gelene kadar döner ve gelen sinyalin
 * numarasını (SIGINT/SIGTERM) döndürür.
 *
 * Neden pause() değil de nanosleep()? Bayrakları kontrol ettikten sonra
 * pause() çağırırsak, sinyal tam kontrol ile pause() arasında gelirse
 * program bir sonraki sinyale kadar uyuyakalır (lost wake-up). Kısa
 * nanosleep() ile en kötü durumda istek 100 ms gecikir, asla kaybolmaz.
 */
static int run_main_loop(void)
{
    const struct timespec nap = { 0, MAIN_LOOP_SLEEP_NS };
    int shutdown_signal;

    for (;;) {
        /* 1) Shutdown en öncelikli: geldiyse başka hiçbir iş yapmadan çık. */
        shutdown_signal = signal_manager_take_shutdown_request();
        if (shutdown_signal != 0)
            return shutdown_signal;

        /* 2) SIGCHLD: sonlanan child'ları topla (restart kararı Student 2'de). */
        if (signal_manager_take_child_event())
            process_manager_reap_terminated();

        /* 3) SIGALRM: periyodik bakım/kontrol. */
        if (signal_manager_take_timer_tick())
            process_manager_periodic_check();

        /* 4) SIGUSR1: durum raporu. */
        if (signal_manager_take_status_request())
            handle_status_request();

        /* 5) Bir sonraki sinyale veya 100 ms dolana kadar uyu.
         *    EINTR hata değil: sinyal geldi demektir, döngü başa döner. */
        if (nanosleep(&nap, NULL) == -1 && errno != EINTR) {
            perror("watchdog: nanosleep");
            /* Uyuyamıyorsak bile döngü çalışmaya devam eder. */
        }
    }
}

/*
 * Graceful shutdown (rehber 5.3):
 *   1. Timer'ı durdur (artık periyodik kontrol / restart yok).
 *   2. Process manager'ın sınırlı kapatmasını çağır: çalışan child'lara
 *      SIGTERM, timeout boyunca bekle, kalanlara SIGKILL, hepsini waitpid().
 *   3. Kapanışı logla ve log dosyasını kapat.
 */
static void graceful_shutdown(const char *reason)
{
    logger_event("SHUTDOWN_START", "watchdog", getpid(), reason);

    process_manager_stop_periodic_timer();
    process_manager_shutdown(PROCESS_MANAGER_DEFAULT_SHUTDOWN_TIMEOUT);

    logger_event("SHUTDOWN_COMPLETE", "watchdog", getpid(),
                 "all workers stopped and reaped");
    logger_close();
}

int main(void)
{
    char details[64];
    int shutdown_signal;

    /*
     * 1) Handler'ları en başta kur. Böylece başlatma sırasında gelen bir
     *    Ctrl+C programı kontrolsüzce öldürmez; bayrak olarak bekler.
     */
    if (signal_manager_init() != 0) {
        perror("watchdog: signal_manager_init");
        return EXIT_FAILURE;
    }

    /* 2) Log dosyasını aç. */
    if (logger_init(WATCHDOG_LOG_PATH) != 0) {
        fprintf(stderr, "watchdog: cannot open log file '%s'\n",
                WATCHDOG_LOG_PATH);
        return EXIT_FAILURE;
    }

    /* Testlerde kill komutu için gerekli PID'i ekrana da yaz. fflush:
     * fork() öncesi stdio tamponu boşalmalı, yoksa child'lar aynı çıktıyı
     * tekrar basabilir. */
    printf("watchdog started, PID: %ld (log: %s)\n", (long)getpid(),
           WATCHDOG_LOG_PATH);
    fflush(stdout);

    logger_event("WATCHDOG_START", "watchdog", getpid(), "watchdog started");

    /* 3) Process tablosunu hazırla. */
    if (process_manager_init() != 0) {
        logger_event("ERROR", "watchdog", getpid(),
                     "process_manager_init failed");
        logger_close();
        return EXIT_FAILURE;
    }

    /* 4) Worker'ları başlat. Başarısız olursa yine de kontrollü kapan:
     *    belki bir worker başlamıştır ve toplanması gerekir. */
    if (process_manager_start_all() != 0) {
        logger_event("ERROR", "watchdog", getpid(),
                     "process_manager_start_all failed");
        graceful_shutdown("startup failure");
        return EXIT_FAILURE;
    }

    /* 5) Periyodik timer'ı başlat (SIGALRM). */
    if (process_manager_start_periodic_timer(PERIODIC_CHECK_INTERVAL_SECONDS)
        != 0) {
        logger_event("ERROR", "watchdog", getpid(),
                     "periodic timer could not be started");
        graceful_shutdown("startup failure");
        return EXIT_FAILURE;
    }

    /* 6) Ana döngü: shutdown isteği gelene kadar çalışır. */
    shutdown_signal = run_main_loop();

    /* 7) Kontrollü kapanma. */
    snprintf(details, sizeof details, "%s received",
             shutdown_signal_name(shutdown_signal));
    printf("watchdog: %s, shutting down...\n", details);
    fflush(stdout);

    graceful_shutdown(details);
    return EXIT_SUCCESS;
}
