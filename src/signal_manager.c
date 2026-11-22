/*
 * signal_manager.c - Student 1
 *
 * Sinyal handler'larının kurulumu ve sinyal güvenli (async-signal-safe)
 * bayrak yönetimi.
 *
 * Temel kural: handler içinde printf, malloc, fopen, fork, waitpid, logger
 * gibi güvenli olmayan hiçbir şey çağrılmaz. Handler sadece bir bayrağa
 * değer yazar; asıl iş main.c'deki ana döngüde yapılır.
 */

#include "signal_manager.h"

#include <signal.h>
#include <stddef.h>

/*
 * Bayraklar.
 * - volatile: derleyici değeri bir register'da tutup "değişmedi" sanmasın;
 *   her okumada bellekten okusun (handler değeri ana akışın dışında değiştirir).
 * - sig_atomic_t: tek adımda okunup yazılabilen tamsayı tipi; handler yazarken
 *   ana akış yarım bir değer göremez.
 * - static: sadece bu dosyadan erişilebilir; diğer modüller take_* kullanır.
 */
static volatile sig_atomic_t shutdown_request = 0; /* 0 veya SIGINT/SIGTERM */
static volatile sig_atomic_t status_request   = 0;
static volatile sig_atomic_t child_event      = 0;
static volatile sig_atomic_t timer_tick       = 0;

/*
 * Tek bir handler bütün sinyaller için kullanılır; hangi sinyalin geldiğini
 * signo parametresinden anlar. Sadece bayrak ayarladığı için async-signal-safe.
 */
static void handle_signal(int signo)
{
    switch (signo) {
    case SIGINT:
    case SIGTERM:
        shutdown_request = signo;
        break;
    case SIGUSR1:
        status_request = 1;
        break;
    case SIGCHLD:
        child_event = 1;
        break;
    case SIGALRM:
        timer_tick = 1;
        break;
    default:
        break;
    }
}

/* Bir sinyal için sigaction() ile handler kurar. */
static int install_handler(int signo, int flags)
{
    struct sigaction sa;

    sa.sa_handler = handle_signal;
    sa.sa_flags = flags;

    /*
     * sa_mask: handler çalışırken bloklanacak sinyaller. Bizim beş sinyalimizi
     * de blokluyoruz; böylece bir handler çalışırken diğeri araya girmez.
     */
    if (sigemptyset(&sa.sa_mask) == -1 ||
        sigaddset(&sa.sa_mask, SIGINT) == -1 ||
        sigaddset(&sa.sa_mask, SIGTERM) == -1 ||
        sigaddset(&sa.sa_mask, SIGUSR1) == -1 ||
        sigaddset(&sa.sa_mask, SIGCHLD) == -1 ||
        sigaddset(&sa.sa_mask, SIGALRM) == -1) {
        return -1;
    }

    return sigaction(signo, &sa, NULL);
}

int signal_manager_init(void)
{
    /*
     * SA_RESTART: read/write gibi sistem çağrıları sinyal yüzünden kesilirse
     * çekirdek onları otomatik yeniden başlatır (EINTR hatası azalır).
     * Not: nanosleep() SA_RESTART olsa bile EINTR ile döner; ana döngü
     * bunu bilerek kullanır, sinyal gelince hemen uyanır.
     *
     * SA_NOCLDSTOP: SIGCHLD sadece child sonlandığında gelsin; child
     * durdurulduğunda/devam ettirildiğinde (SIGSTOP/SIGCONT) gelmesin.
     */
    if (install_handler(SIGINT, SA_RESTART) == -1)
        return -1;
    if (install_handler(SIGTERM, SA_RESTART) == -1)
        return -1;
    if (install_handler(SIGUSR1, SA_RESTART) == -1)
        return -1;
    if (install_handler(SIGCHLD, SA_RESTART | SA_NOCLDSTOP) == -1)
        return -1;
    if (install_handler(SIGALRM, SA_RESTART) == -1)
        return -1;

    return 0;
}

/*
 * Bayrağı oku ve sıfırla. Okuma ile sıfırlama arasında aynı sinyal gelirse
 * yeni istek silinip kaybolabilir. Bunu önlemek için o sırada ilgili
 * sinyal(ler)i sigprocmask() ile blokluyoruz. Bloklanan sinyal kaybolmaz,
 * "bekleyen" (pending) olarak kalır ve maske eski hâline dönünce teslim edilir.
 *
 * signo2: bayrağı yazan ikinci bir sinyal varsa onun numarası, yoksa 0.
 */
static int take_flag(volatile sig_atomic_t *flag, int signo1, int signo2)
{
    sigset_t block;
    sigset_t old;
    int blocked;
    int value;

    blocked = sigemptyset(&block) == 0 &&
              sigaddset(&block, signo1) == 0 &&
              (signo2 == 0 || sigaddset(&block, signo2) == 0) &&
              sigprocmask(SIG_BLOCK, &block, &old) == 0;

    /*
     * Bloklama başarısız olsa bile (pratikte olmaz) devam ediyoruz; en kötü
     * durumda tek bir istek bir tur gecikir.
     */
    value = *flag;
    *flag = 0;

    if (blocked)
        (void)sigprocmask(SIG_SETMASK, &old, NULL);

    return value;
}

int signal_manager_take_shutdown_request(void)
{
    /* SIGINT ve SIGTERM aynı bayrağı yazar; ikisini birden blokla. */
    return take_flag(&shutdown_request, SIGINT, SIGTERM);
}

int signal_manager_take_status_request(void)
{
    return take_flag(&status_request, SIGUSR1, 0);
}

int signal_manager_take_child_event(void)
{
    return take_flag(&child_event, SIGCHLD, 0);
}

int signal_manager_take_timer_tick(void)
{
    return take_flag(&timer_tick, SIGALRM, 0);
}
