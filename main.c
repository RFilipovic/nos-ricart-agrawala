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

// --- Pomoćna funkcija za ispis tipa poruke ---
const char* msg_type_to_string(int type) {
    switch(type) {
        case MSG_REQUEST: return "zahtjev (R-A)";
        case MSG_REPLY:   return "odgovor (R-A)";
        case MSG_SJEDNI:  return "sjedni";
        case MSG_USTANI:  return "ustani";
        case MSG_RIDE:    return "zahtjev (Vrtuljak)";
        case MSG_DONE:    return "gotov";
        default:          return "NEPOZNAT";
    }
}

void send_message(int from, int to, Message *msg) {
    // *** NOVI ISPIS ZA SLANJE ***
    // Formatiranje: %-2d (za ID) i %-18s (za tip poruke) osigurava lijepo poravnanje
    if (from == CAROUSEL_ID) {
        // Vrtuljak šalje
        printf("Vrtuljak  : Šaljem %-18s -> Posjetitelju %d (Tm=%d)\n", 
               msg_type_to_string(msg->type), to, msg->Tm);
    } else {
        // Posjetitelj šalje
        if (to == CAROUSEL_ID) {
            printf("Posjetitelj %-2d: Šaljem %-18s -> Vrtuljku    (Tm=%d)\n", 
                   from, msg_type_to_string(msg->type), msg->Tm);
        } else {
            printf("Posjetitelj %-2d: Šaljem %-18s -> Posjetitelju %d (Tm=%d)\n", 
                   from, msg_type_to_string(msg->type), to, msg->Tm);
        }
    }
    // *** KRAJ NOVOG ISPISA ***

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
                    
                    // *** NOVI ISPIS ZA PRIMANJE ***
                    printf("Posjetitelj %-2d: Primio %-18s <- Posjetitelja %d (Tm=%d)\n", 
                           data->id, msg_type_to_string(msg.type), msg.sender_id, msg.Tm);
                    // *** KRAJ NOVOG ISPISA ***

                    if(msg.type == MSG_REQUEST) {
                        // OBRISAN dupli printf
                        enqueue(&pq, &msg); 
                        int my_req_index = findByProcessNum(&pq, data->id);

                        if (taken == 1) {
                            deferred_replies[msg.sender_id] = 1;
                            printf("Posjetitelj %d: (Stanje: Na vožnji) Odgađam odgovor za %d\n", data->id, msg.sender_id);
                        } else if (my_req_index == -1) {
                            Message reply = {MSG_REPLY, data->id, Ci, 0};
                            send_message(data->id, msg.sender_id, &reply);
                        } else {
                            Message my_req = pq.messages[my_req_index]; 
                            if (msg.Tm < my_req.Tm || (msg.Tm == my_req.Tm && msg.sender_id < data->id)) {
                                Message reply = {MSG_REPLY, data->id, Ci, 0};
                                send_message(data->id, msg.sender_id, &reply);
                            } else {
                                deferred_replies[msg.sender_id] = 1;
                                printf("Posjetitelj %d: (Stanje: Moj Tm niži) Odgađam odgovor za %d\n", data->id, msg.sender_id);
                            }
                        }
                    }
                    else if(msg.type == MSG_REPLY) {
                        // OBRISAN dupli printf
                        rcvd_replies++;
                        // Ovaj ispis je koristan da vidimo kad je proces spreman ući u K.O.
                        printf("Posjetitelj %-2d: (Brojač odgovora: %d/%d)\n", 
                               data->id, rcvd_replies, NUM_VISITORS - 1);
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
                
                // *** NOVI ISPIS ZA PRIMANJE ***
                printf("Posjetitelj %-2d: Primio %-18s <- Vrtuljka    (Tm=%d)\n", 
                       data->id, msg_type_to_string(msg.type), msg.Tm);
                // *** KRAJ NOVOG ISPISA ***

                if(msg.type == MSG_SJEDNI) { 
                    printf("Posjetitelj %d: Sjeo.\n", data->id); // Logiramo akciju
                    taken = 1; 
                }
                else if(msg.type == MSG_USTANI) { 
                    printf("Posjetitelj %d: Ustao.\n", data->id); // Logiramo akciju
                    taken = 0; 
                    rides++;   
                    
                    printf("Posjetitelj %d: Sišao, šaljem odgođene R-A odgovore...\n", data->id);
                    for (i = 0; i < NUM_VISITORS; i++) { 
                        if (deferred_replies[i]) {
                            Ci++; 
                            Message reply = {MSG_REPLY, data->id, Ci, 0};
                            send_message(data->id, i, &reply);
                            // Ovaj ispis brišemo, jer ga sad radi send_message
                            // printf("Posjetitelj %d: Šaljem odgođeni R-A odgovor za %d\n", data->id, i);
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
        
        // *** OBRISANO ***: printf("... Poslao R-A zahtjeve ...")
        // (send_message sada ispisuje 11 pojedinačnih poruka)
        
        // Čekaj dok ne dobiješ sve odgovore
        while (rcvd_replies < (NUM_VISITORS - 1)) {
            usleep(10000);
        }
        
        // --- U KRITIČNOM ODSJEČKU ---
        printf("Posjetitelj %d: Ušao u K.O.\n", id);
        
        Message ride_req = {MSG_RIDE, id, Ci, 1};
        send_message(id, CAROUSEL_ID, &ride_req); // Ovo će ispisati "Šaljem ZAHTJEV (Vrtuljak)"
        
        // --- ODMAH IZLAZIMO IZ K.O. ---
        
        printf("Posjetitelj %d: Izlazim iz K.O. (poslao zahtjev)\n", id);

        // 1. Makni moj zahtjev iz reda
        int my_req_index = findByProcessNum(&pq, id);
        if (my_req_index != -1) {
            Message temp_msg;
            removeAt(&pq, my_req_index, &temp_msg);
        }

        // 2. Pošalji sve odgođene odgovore
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (deferred_replies[i]) {
                Ci++; 
                Message reply = {MSG_REPLY, id, Ci, 0};
                send_message(id, i, &reply); // Ovo će ispisati "Šaljem ODGOVOR (R-A)"
                
                // *** OBRISANO ***: printf("... Šaljem odgođeni R-A odgovor ...")
                
                deferred_replies[i] = 0;
            }
        }
        
        // --- KRAJ KRITIČNOG ODSJEČKA ---
        
        printf("Posjetitelj %d: Čekam da sjednem...\n", id);
        while (taken == 0) usleep(10000);
        
        printf("Posjetitelj %d: Na vožnji sam, čekam da ustanem...\n", id);
        while (taken == 1) usleep(10000);
        
        printf("Posjetitelj %d: Vožnja %d gotova.\n", id, rides);
    }
    
    printf("Posjetitelj %d: Završio sve vožnje.\n", id);
    Message done_msg = {MSG_DONE, id, Ci, 0};
    send_message(id, CAROUSEL_ID, &done_msg); // Ovo će ispisati "Šaljem GOTOV"

    pthread_cancel(thread);
    for(int i=0; i<NUM_VISITORS; i++) if(targs.read_fds[i] != -1) close(targs.read_fds[i]);
    if(targs.carousel_read_fd != -1) close(targs.carousel_read_fd);
    
    freeQueue(&pq);
    exit(0);
}

// --- Proces Vrtuljak ---
void carousel_process() {
    printf("Vrtuljak: Pokrećem se s %d sjedala.\n", CAROUSEL_SEATS);
    srand(time(NULL) ^ getpid());

    int read_fds[NUM_VISITORS];
    char pipe_name[50];
    
    for (int i = 0; i < NUM_VISITORS; i++) {
        get_pipe_name(pipe_name, i, CAROUSEL_ID);
        read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK);
        if (read_fds[i] == -1) {
            perror("carousel_process: open read pipe");
            exit(1);
        }
    }
    
    int waiting_visitors[NUM_VISITORS]; 
    int waiting_count = 0;
    int seated_visitors[CAROUSEL_SEATS]; 
    int finished_visitors = 0;
    
    while(finished_visitors < NUM_VISITORS) {
        
        Message msg;
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (read_fds[i] != -1) {
                ssize_t bytes_read = read(read_fds[i], &msg, sizeof(Message));
                if (bytes_read > 0) {
                    
                    // *** NOVI ISPIS ZA PRIMANJE ***
                    printf("Vrtuljak  : Primio %-18s <- Posjetitelja %d (Tm=%d)\n", 
                           msg_type_to_string(msg.type), msg.sender_id, msg.Tm);
                    // *** KRAJ NOVOG ISPISA ***

                    if (msg.type == MSG_RIDE) {
                        // OBRISAN dupli printf
                        waiting_visitors[waiting_count++] = msg.sender_id;
                    } else if (msg.type == MSG_DONE) {
                        // OBRISAN dupli printf
                        finished_visitors++;
                        close(read_fds[i]); 
                        read_fds[i] = -1;
                    }
                }
            }
        }
        
        if (waiting_count >= CAROUSEL_SEATS) {
            printf("Vrtuljak: Imam %d putnika, pokrećem ukrcaj.\n", waiting_count);
            
            // Pošalji poruke "Sjedi"
            for (int i = 0; i < CAROUSEL_SEATS; i++) {
                int visitor_id = waiting_visitors[i];
                seated_visitors[i] = visitor_id;
                // Vrijeme nije bitno za ovu poruku, ali Ci vrtuljka bi se mogao pratiti
                Message seat_msg = {MSG_SJEDNI, CAROUSEL_ID, 0, 0}; 
                send_message(CAROUSEL_ID, visitor_id, &seat_msg);
            }
            waiting_count -= CAROUSEL_SEATS;
            for(int i=0; i < waiting_count; i++) {
                waiting_visitors[i] = waiting_visitors[i + CAROUSEL_SEATS];
            }
            
            printf("Vrtuljak: Pokrenuo vrtuljak.\n");
            usleep((rand() % 2000 + 1000) * 1000); 
            printf("Vrtuljak: Zaustavljen.\n");
            
            // Pošalji poruke "Ustani"
            for (int i = 0; i < CAROUSEL_SEATS; i++) {
                Message arise_msg = {MSG_USTANI, CAROUSEL_ID, 0, 0};
                send_message(CAROUSEL_ID, seated_visitors[i], &arise_msg);
            }
            printf("Vrtuljak: Poslao poruke za ustajanje. Čekam nove posjetitelje.\n");
        }
        
        usleep(100000); 
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