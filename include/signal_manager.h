#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

/*
 * signal_manager - Student 1
 *
 * SIGINT, SIGTERM, SIGUSR1, SIGCHLD ve SIGALRM için sigaction() ile
 * handler kurar. Handler'lar yalnızca volatile sig_atomic_t bayrakları
 * ayarlar; asıl işi ana döngü, aşağıdaki take_* fonksiyonlarıyla bayrakları
 * kontrol ederek yapar.
 *
 * Her take_* fonksiyonu istek bekliyor mu onu döndürür ve bayrağı temizler.
 * Okuma ve temizleme sırasında ilgili sinyal bloklanır; böylece arada gelen
 * bir sinyal kaybolmaz.
 */

/* Bütün handler'ları kurar. Başarıda 0, hatada -1 döner (errno ayarlanır). */
int signal_manager_init(void);

/* SIGINT veya SIGTERM geldiyse sıfırdan farklı döner. Dönen değer gelen
 * sinyalin numarasıdır (SIGINT ya da SIGTERM); böylece hangisinin geldiği
 * loglanabilir. */
int signal_manager_take_shutdown_request(void);

/* SIGUSR1 (durum raporu isteği) geldiyse sıfırdan farklı döner. */
int signal_manager_take_status_request(void);

/* SIGCHLD geldiyse sıfırdan farklı döner. Birden fazla child çıkışı tek bir
 * sinyalde birleşebilir; bu yüzden çağıran taraf waitpid(..., WNOHANG) ile
 * döngü hâlinde toplama yapmalıdır. */
int signal_manager_take_child_event(void);

/* SIGALRM (periyodik timer tick'i) geldiyse sıfırdan farklı döner. */
int signal_manager_take_timer_tick(void);

#endif /* SIGNAL_MANAGER_H */
