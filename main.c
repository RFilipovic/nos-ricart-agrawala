#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <string.h> // Za memset

// *** POPRAVAK: Uključujemo tvoju definiciju reda ***
#include "priority_queue.h"

// --- Definicije ---
// (Definicija Message i PriorityQueue su sada u .h datoteci)

#define NUM_VISITORS 12
#define RIDES_PER_VISITOR 2
#define CAROUSEL_SEATS 4
#define CAROUSEL_ID 12 // ID vrtuljka (mora biti različit od 0-11)

#define MSG_REQUEST 1 // R-A: Tražim K.O.
#define MSG_REPLY 2   // R-A: Odgovor na zahtjev
#define MSG_SJEDNI 3  // Vrtuljak: Možeš sjesti
#define MSG_USTANI 4  // Vrtuljak: Vožnja gotova, ustani
#define MSG_RIDE 5    // Posjetitelj -> Vrtuljak: Želim se voziti
#define MSG_DONE 6    // Posjetitelj -> Vrtuljak: Gotov sam sa svim vožnjama

#define MAX(a, b) ((a) > (b) ? (a) : (b))

// --- Globalne varijable za JEDAN proces posjetitelja ---
pthread_t thread;
PriorityQueue pq;

typedef struct {
    int id;
    int read_fds[NUM_VISITORS];
    int carousel_read_fd;
} ThreadData;

int Ci;
int rcvd_replies = 0;
int taken = 0; // 0 = nije na vrtuljku, 1 = na vrtuljku
int rides = 0;
int deferred_replies[NUM_VISITORS];

// --- Funkcije za cjevovode (nepromijenjene) ---

void get_pipe_name(char *buffer, int from, int to) {
    snprintf(buffer, 50, "/tmp/pipe_%d_%d", from, to);
}

void create_all_pipes() {
    char pipe_name[50];
    
    for (int i = 0; i < NUM_VISITORS; i++) {
        for (int j = i + 1; j < NUM_VISITORS; j++) {
            get_pipe_name(pipe_name, i, j);
            if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
                if (errno != EEXIST) {
                    perror("Greška pri stvaranju cijevi (i->j)");
                    exit(1);
                }
            }
            get_pipe_name(pipe_name, j, i);
            if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
                if (errno != EEXIST) {
                    perror("Greška pri stvaranju cijevi (j->i)");
                    exit(1);
                }
            }
        }
        
        get_pipe_name(pipe_name, i, CAROUSEL_ID);
        if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
            if (errno != EEXIST) {
                perror("Greška pri stvaranju cijevi (i->vrtuljak)");
                exit(1);
            }
        }
        get_pipe_name(pipe_name, CAROUSEL_ID, i);
        if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
            if (errno != EEXIST) {
                perror("Greška pri stvaranju cijevi (vrtuljak->i)");
                exit(1);
            }
        }
    }
}

void send_message(int from, int to, Message *msg) {
    char pipe_name[50];
    get_pipe_name(pipe_name, from, to);
    int fd = open(pipe_name, O_WRONLY); 
    if (fd == -1) {
        perror("send_message: open error");
        return;
    }
    write(fd, msg, sizeof(Message));
    close(fd);
}

// --- Dretva posjetitelja (Listener) ---
void *visitor_thread(void *args) {
    ThreadData *data = (ThreadData*)args;
    Message msg;
    char pipe_name[50];
    int i; 
    
    while(1) {
        // 1. Provjeri poruke od DRUGIH POSJETITELJA (R-A)
        for(i = 0; i < NUM_VISITORS; i++) {
            if(i != data->id && data->read_fds[i] != -1) {
                
                ssize_t bytes_read = read(data->read_fds[i], &msg, sizeof(Message));
                
                if (bytes_read > 0) { // Imamo poruku!
                    Ci = MAX(Ci, msg.Tm) + 1;
                    
                    if(msg.type == MSG_REQUEST) {
                        printf("Posjetitelj %d: Primio R-A ZAHTJEV od %d (Tm=%d)\n", 
                               data->id, msg.sender_id, msg.Tm);
                        
                        enqueue(&pq, &msg); 
                        int my_req_index = findByProcessNum(&pq, data->id);

                        // *** ISPRAVNA LOGIKA ODBIJANJA ***
                        // Odbij ako:
                        // 1. Si TRENUTNO na vožnji (taken == 1)
                        // 2. Ili ako si U REDU za K.O. i imaš prednost (my_req_index != -1, i tvoj Tm je manji)
                        if (taken == 1) {
                            deferred_replies[msg.sender_id] = 1;
                            printf("Posjetitelj %d: (Na vožnji) Odgađam odgovor za %d\n", data->id, msg.sender_id);
                        } else if (my_req_index == -1) {
                            // Ne tražim K.O. i nisam na vožnji, odgovori odmah
                            Message reply = {MSG_REPLY, data->id, Ci, 0};
                            send_message(data->id, msg.sender_id, &reply);
                        } else {
                            // Nisam na vožnji, ali tražim K.O., provjeri R-A
                            Message my_req = pq.messages[my_req_index]; 
                            if (msg.Tm < my_req.Tm || (msg.Tm == my_req.Tm && msg.sender_id < data->id)) {
                                Message reply = {MSG_REPLY, data->id, Ci, 0};
                                send_message(data->id, msg.sender_id, &reply);
                            } else {
                                deferred_replies[msg.sender_id] = 1;
                                printf("Posjetitelj %d: (Moj Tm niži) Odgađam odgovor za %d\n", data->id, msg.sender_id);
                            }
                        }
                    }
                    else if(msg.type == MSG_REPLY) {
                        rcvd_replies++;
                        printf("Posjetitelj %d: Primio R-A ODGOVOR od %d (%d/%d)\n", 
                               data->id, msg.sender_id, rcvd_replies, NUM_VISITORS - 1);
                    }
                } else if (bytes_read == 0) { 
                    close(data->read_fds[i]);
                    get_pipe_name(pipe_name, i, data->id); 
                    data->read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK); 
                    if (data->read_fds[i] == -1) {
                        perror("visitor_thread: re-open pipe failed");
                    }
                } else if (errno != EAGAIN) { 
                    // Nema podataka, ne radi ništa
                }
            }
        }

        // 2. Provjeri poruke od VRTULJKA
        if (data->carousel_read_fd != -1) {
            ssize_t bytes_read = read(data->carousel_read_fd, &msg, sizeof(Message));
            if (bytes_read > 0) {
                Ci = MAX(Ci, msg.Tm) + 1;
                
                if(msg.type == MSG_SJEDNI) { 
                    printf("Posjetitelj %d: Sjeo na vrtuljak\n", data->id);
                    taken = 1; // Javi glavnoj petlji da može nastaviti
                }
                else if(msg.type == MSG_USTANI) { 
                    printf("Posjetitelj %d: Ustao s vrtuljka\n", data->id);
                    taken = 0; // Javi glavnoj petlji da je vožnja gotova
                    rides++;   // Povećaj brojač vožnji
                    
                    // *** POPRAVAK: Šalji odgođene odgovore NAKON vožnje ***
                    // (Logika koja je nedostajala i uzrokovala zadnji deadlock)
                    printf("Posjetitelj %d: Sišao, šaljem odgođene R-A odgovore...\n", data->id);
                    for (i = 0; i < NUM_VISITORS; i++) { // Ponovno koristi 'i'
                        if (deferred_replies[i]) {
                            Ci++; // Uskladimo sat
                            Message reply = {MSG_REPLY, data->id, Ci, 0};
                            send_message(data->id, i, &reply);
                            printf("Posjetitelj %d: Šaljem odgođeni R-A odgovor za %d\n", data->id, i);
                            deferred_replies[i] = 0;
                        }
                    }
                }
            } else if (bytes_read == 0) {
                close(data->carousel_read_fd);
                get_pipe_name(pipe_name, CAROUSEL_ID, data->id); 
                data->carousel_read_fd = open(pipe_name, O_RDONLY | O_NONBLOCK);
                if (data->carousel_read_fd == -1) {
                    perror("visitor_thread: re-open carousel pipe failed");
                }
            }
        }
        usleep(10000); 
    }
    return NULL;
}

// --- Proces posjetitelja (Requester) ---
void visitor_process(int id) { 
    srand(time(NULL) ^ getpid()); 
    Ci = 0;
    initQueue(&pq);
    memset(deferred_replies, 0, sizeof(deferred_replies));

    ThreadData targs;
    targs.id = id;
    
    char pipe_name[50];

    // Otvori SVE cijevi za ČITANJE JEDNOM (neblokirajuće)
    for (int i = 0; i < NUM_VISITORS; i++) {
        if (i == id) {
            targs.read_fds[i] = -1;
            continue;
        }
        get_pipe_name(pipe_name, i, id);
        targs.read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK);
        if (targs.read_fds[i] == -1) {
            perror("visitor_process: open read pipe");
            exit(1);
        }
    }
    get_pipe_name(pipe_name, CAROUSEL_ID, id);
    targs.carousel_read_fd = open(pipe_name, O_RDONLY | O_NONBLOCK);
    if (targs.carousel_read_fd == -1) {
        perror("visitor_process: open carousel read pipe");
        exit(1);
    }
    
    // Pokreni dretvu (listener)
    if (pthread_create(&thread, NULL, visitor_thread, &targs) != 0) {
        perror("Error creating thread");
        exit(1);
    }
    
    // Glavna petlja procesa (requester)
    while (rides < RIDES_PER_VISITOR) {
        // Spavaj X milisekundi
        usleep((rand() % 1900 + 100) * 1000);

        // --- ULAZAK U KRITIČNI ODSJEČAK (Ricart-Agrawala) ---
        
        printf("Posjetitelj %d: Želi na vožnju (vožnja %d/%d)\n", id, rides + 1, RIDES_PER_VISITOR);
        Ci++;
        Message my_req = {MSG_REQUEST, id, Ci, 1};
        enqueue(&pq, &my_req);
        
        rcvd_replies = 0;
        
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (i != id) {
                send_message(id, i, &my_req);
            }
        }
        
        printf("Posjetitelj %d: Poslao R-A zahtjeve (Tm=%d), čekam odgovore...\n", id, Ci);
        while (rcvd_replies < (NUM_VISITORS - 1)) {
            usleep(10000);
        }
        
        // --- U KRITIČNOM ODSJEČKU ---
        printf("Posjetitelj %d: Ušao u K.O. Šaljem zahtjev vrtuljku.\n", id);
        
        Message ride_req = {MSG_RIDE, id, Ci, 1};
        send_message(id, CAROUSEL_ID, &ride_req);
        
        // *** POPRAVAK: ODMAH IZLAZIMO IZ K.O. ***
        // (Ovo je Pravilo 4 iz tvog pseudokoda)
        
        printf("Posjetitelj %d: Izlazim iz K.O. (poslao zahtjev)\n", id);

        // 1. Makni moj zahtjev iz reda
        int my_req_index = findByProcessNum(&pq, id);
        if (my_req_index != -1) {
            Message temp_msg;
            removeAt(&pq, my_req_index, &temp_msg);
        }

        // 2. Pošalji sve odgođene odgovore
        // (Ovu logiku premještamo ovdje iz dretve)
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (deferred_replies[i]) {
                Ci++; // Uskladimo sat prije slanja
                Message reply = {MSG_REPLY, id, Ci, 0};
                send_message(id, i, &reply);
                printf("Posjetitelj %d: Šaljem odgođeni R-A odgovor za %d\n", id, i);
                deferred_replies[i] = 0;
            }
        }
        
        // --- KRAJ KRITIČNOG ODSJEČKA ---
        
        // *** POPRAVAK: Sada čekamo vožnju, ALI IZVAN K.O. ***
        
        printf("Posjetitelj %d: Čekam da sjednem...\n", id);
        while (taken == 0) usleep(10000);
        
        printf("Posjetitelj %d: Na vožnji sam, čekam da ustanem...\n", id);
        while (taken == 1) usleep(10000);
        
        printf("Posjetitelj %d: Vožnja %d gotova.\n", id, rides);
    }
    
    printf("Posjetitelj %d: Završio sve vožnje. Šaljem poruku vrtuljku.\n", id);
    Message done_msg = {MSG_DONE, id, Ci, 0};
    send_message(id, CAROUSEL_ID, &done_msg);

    pthread_cancel(thread);
    for(int i=0; i<NUM_VISITORS; i++) if(targs.read_fds[i] != -1) close(targs.read_fds[i]);
    if(targs.carousel_read_fd != -1) close(targs.carousel_read_fd);
    
    freeQueue(&pq);
    exit(0);
}

// --- Proces Vrtuljak (nepromijenjen) ---
void carousel_process() {
    printf("Vrtuljak: Pokrećem se s %d sjedala.\n", CAROUSEL_SEATS);
    srand(time(NULL) ^ getpid());

    int read_fds[NUM_VISITORS];
    char pipe_name[50];
    
    // Otvori sve cijevi za ČITANJE od posjetitelja
    for (int i = 0; i < NUM_VISITORS; i++) {
        get_pipe_name(pipe_name, i, CAROUSEL_ID);
        read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK);
        if (read_fds[i] == -1) {
            perror("carousel_process: open read pipe");
            exit(1);
        }
    }
    
    int waiting_visitors[NUM_VISITORS]; // Red posjetitelja koji čekaju
    int waiting_count = 0;
    int seated_visitors[CAROUSEL_SEATS]; // Posjetitelji na vožnji
    int finished_visitors = 0;
    
    while(finished_visitors < NUM_VISITORS) {
        
        // 1. Provjeri ima li novih zahtjeva ili poruka o završetku
        Message msg;
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (read_fds[i] != -1) {
                ssize_t bytes_read = read(read_fds[i], &msg, sizeof(Message));
                if (bytes_read > 0) {
                    if (msg.type == MSG_RIDE) {
                        printf("Vrtuljak: Primio zahtjev za vožnju od %d\n", msg.sender_id);
                        waiting_visitors[waiting_count++] = msg.sender_id;
                    } else if (msg.type == MSG_DONE) {
                        printf("Vrtuljak: Posjetitelj %d je gotov sa svim vožnjama.\n", msg.sender_id);
                        finished_visitors++;
                        close(read_fds[i]); // Zatvori cijev, više nam ne treba
                        read_fds[i] = -1;
                    }
                }
            }
        }
        
        // 2. Ako je vrtuljak pun, pokreni vožnju
        if (waiting_count >= CAROUSEL_SEATS) {
            printf("Vrtuljak: Imam 4 posjetitelja, pokrećem ukrcaj.\n");
            
            // Pošalji poruke "Sjedi" za prva 4
            for (int i = 0; i < CAROUSEL_SEATS; i++) {
                int visitor_id = waiting_visitors[i];
                seated_visitors[i] = visitor_id;
                Message seat_msg = {MSG_SJEDNI, CAROUSEL_ID, 0, 0};
                send_message(CAROUSEL_ID, visitor_id, &seat_msg);
            }
            // Pomakni ostatak u redu čekanja
            waiting_count -= CAROUSEL_SEATS;
            for(int i=0; i < waiting_count; i++) {
                waiting_visitors[i] = waiting_visitors[i + CAROUSEL_SEATS];
            }
            
            // Pokreni vrtuljak
            printf("Vrtuljak: Pokrenuo vrtuljak.\n");
            usleep((rand() % 2000 + 1000) * 1000); // Spavaj 1-3 sekunde
            printf("Vrtuljak: Zaustavljen.\n");
            
            // Pošalji poruke "Ustani"
            for (int i = 0; i < CAROUSEL_SEATS; i++) {
                Message arise_msg = {MSG_USTANI, CAROUSEL_ID, 0, 0};
                send_message(CAROUSEL_ID, seated_visitors[i], &arise_msg);
            }
            printf("Vrtuljak: Poslao poruke za ustajanje. Čekam nove posjetitelje.\n");
        }
        
        usleep(100000); // Pauza da ne vrtimo 100% CPU
    }
    
    printf("Vrtuljak: Svi posjetitelji su završili. Zatvaram.\n");
    for(int i=0; i<NUM_VISITORS; i++) if(read_fds[i] != -1) close(read_fds[i]);
    exit(0);
}


// --- Main funkcija (nepromijenjena) ---
int main() {
    printf("Pokrećem simulaciju vrtuljka...\n");
    create_all_pipes();

    pid_t pids[NUM_VISITORS + 1]; // +1 za vrtuljak

    // 1. Stvori proces Vrtuljak
    pids[0] = fork();
    if (pids[0] == 0) {
        // Dijete - Vrtuljak
        carousel_process();
    }

    // 2. Stvori 12 procesa Posjetitelja
    if (pids[0] > 0) { // Samo roditelj stvara posjetitelje
        for (int i = 0; i < NUM_VISITORS; i++) {
            pids[i+1] = fork();
            if (pids[i+1] == 0) {
                // Dijete - Posjetitelj
                visitor_process(i);
            }
        }
    }

    // 3. Roditelj čeka da svi završe
    if (pids[0] > 0) {
        for (int i = 0; i < NUM_VISITORS + 1; i++) {
            wait(NULL);
        }
        printf("Simulacija završena.\n");
    }

    return 0;
}

// *** POPRAVAK: Implementacija reda je sada u tvojoj priority_queue.c datoteci ***