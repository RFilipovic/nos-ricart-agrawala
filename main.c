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
#include <string.h> 
#include <signal.h> // Za kill()

#include "priority_queue.h"

// --- Definicije ---
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

// --- Globalne varijable (po procesu) ---
pthread_t thread;
PriorityQueue pq;

typedef struct {
    int id;
    int read_fds[NUM_VISITORS];
    int carousel_read_fd;
} ThreadData;

int Ci; // Logički sat
int rcvd_replies = 0;
int taken = 0; // 0 = nije na vrtuljku, 1 = na vrtuljku
int rides = 0;
int deferred_replies[NUM_VISITORS];

// --- Upravljanje cjevovodima ---

void get_pipe_name(char *buffer, int from, int to) {
    snprintf(buffer, 50, "/tmp/pipe_%d_%d", from, to);
}

void create_all_pipes() {
    char pipe_name[50];
    for (int i = 0; i < NUM_VISITORS; i++) {
        for (int j = i + 1; j < NUM_VISITORS; j++) {
            get_pipe_name(pipe_name, i, j);
            if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1 && errno != EEXIST) {
                perror("Greška pri stvaranju cijevi (i->j)"); exit(1);
            }
            get_pipe_name(pipe_name, j, i);
            if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1 && errno != EEXIST) {
                perror("Greška pri stvaranju cijevi (j->i)"); exit(1);
            }
        }
        get_pipe_name(pipe_name, i, CAROUSEL_ID);
        if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1 && errno != EEXIST) {
            perror("Greška pri stvaranju cijevi (i->vrtuljak)"); exit(1);
        }
        get_pipe_name(pipe_name, CAROUSEL_ID, i);
        if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1 && errno != EEXIST) {
            perror("Greška pri stvaranju cijevi (vrtuljak->i)"); exit(1);
        }
    }
}

// --- Komunikacija i ispis ---

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
    // Formatirani ispis svake poslane poruke (zahtjev zadatka)
    if (from == CAROUSEL_ID) {
        printf("Vrtuljak  : Šaljem %-18s -> Posjetitelju %d (Tm=%d)\n", 
               msg_type_to_string(msg->type), to, msg->Tm);
    } else {
        if (to == CAROUSEL_ID) {
            printf("Posjetitelj %-2d: Šaljem %-18s -> Vrtuljku    (Tm=%d)\n", 
                   from, msg_type_to_string(msg->type), msg->Tm);
        } else {
            printf("Posjetitelj %-2d: Šaljem %-18s -> Posjetitelju %d (Tm=%d)\n", 
                   from, msg_type_to_string(msg->type), to, msg->Tm);
        }
    }

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

// --- Dretva posjetitelja (Slušač) ---
// Ova dretva se vrti u pozadini i samo prima poruke
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
                    Ci = MAX(Ci, msg.Tm) + 1; // Pravilo globalnog sata
                    
                    // Ispis svake primljene poruke (zahtjev zadatka)
                    printf("Posjetitelj %-2d: Primio %-18s <- Posjetitelja %d (Tm=%d)\n", 
                           data->id, msg_type_to_string(msg.type), msg.sender_id, msg.Tm);

                    if(msg.type == MSG_REQUEST) {
                        // Pravilo 2. Ricart-Agrawala
                        enqueue(&pq, &msg); 
                        int my_req_index = findByProcessNum(&pq, data->id);

                        if (taken == 1) {
                            // Zauzet sam (na vožnji), odgodi odgovor
                            deferred_replies[msg.sender_id] = 1;
                            printf("Posjetitelj %d: (Stanje: Na vožnji) Odgađam odgovor za %d\n", data->id, msg.sender_id);
                        } else if (my_req_index == -1) {
                            // Ne želim u K.O., odgovori odmah
                            Message reply = {MSG_REPLY, data->id, Ci, 0};
                            send_message(data->id, msg.sender_id, &reply);
                        } else {
                            // I ja želim u K.O., provjeri prioritete
                            Message my_req = pq.messages[my_req_index]; 
                            if (msg.Tm < my_req.Tm || (msg.Tm == my_req.Tm && msg.sender_id < data->id)) {
                                // Njegov zahtjev ima prioritet, odgovori odmah
                                Message reply = {MSG_REPLY, data->id, Ci, 0};
                                send_message(data->id, msg.sender_id, &reply);
                            } else {
                                // Moj zahtjev ima prioritet, odgodi
                                deferred_replies[msg.sender_id] = 1;
                                printf("Posjetitelj %d: (Stanje: Moj Tm niži) Odgađam odgovor za %d\n", data->id, msg.sender_id);
                            }
                        }
                    }
                    else if(msg.type == MSG_REPLY) {
                        // Pravilo 3. Ricart-Agrawala
                        rcvd_replies++;
                        printf("Posjetitelj %-2d: (Brojač odgovora: %d/%d)\n", 
                               data->id, rcvd_replies, NUM_VISITORS - 1);
                    }
                } else if (bytes_read == 0) { 
                    // POPRAVAK: Rješavanje pipe deadlocka
                    // Pisac je zatvorio cijev (nakon send_message). Zatvori i odmah ponovno otvori.
                    close(data->read_fds[i]);
                    get_pipe_name(pipe_name, i, data->id); 
                    data->read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK); 
                    if (data->read_fds[i] == -1) {
                        perror("visitor_thread: re-open pipe failed");
                    }
                } else if (errno != EAGAIN) { 
                    // EAGAIN je normalno (znači "nema podataka"), sve ostalo je greška
                }
            }
        }

        // 2. Provjeri poruke od VRTULJKA
        if (data->carousel_read_fd != -1) {
            ssize_t bytes_read = read(data->carousel_read_fd, &msg, sizeof(Message));
            if (bytes_read > 0) {
                Ci = MAX(Ci, msg.Tm) + 1;
                
                printf("Posjetitelj %-2d: Primio %-18s <- Vrtuljka    (Tm=%d)\n", 
                       data->id, msg_type_to_string(msg.type), msg.Tm);

                if(msg.type == MSG_SJEDNI) { 
                    printf("Posjetitelj %d: Sjeo.\n", data->id); // Pseudokod: "ispiši Sjeo..."
                    taken = 1; // Javi glavnoj petlji
                }
                else if(msg.type == MSG_USTANI) { 
                    printf("Posjetitelj %d: Ustao.\n", data->id); // Pseudokod: "ispiši Sišao..."
                    taken = 0; // Javi glavnoj petlji
                    rides++;   
                    
                    // POPRAVAK: Rješavanje logičkog deadlocka
                    // Pošalji sve odgovore odgođene *dok si bio na vožnji*
                    printf("Posjetitelj %d: Sišao, šaljem odgođene R-A odgovore...\n", data->id);
                    for (i = 0; i < NUM_VISITORS; i++) { 
                        if (deferred_replies[i]) {
                            Ci++; 
                            Message reply = {MSG_REPLY, data->id, Ci, 0};
                            send_message(data->id, i, &reply);
                            deferred_replies[i] = 0;
                        }
                    }
                }
            } else if (bytes_read == 0) {
                // POPRAVAK: Rješavanje pipe deadlocka
                close(data->carousel_read_fd);
                get_pipe_name(pipe_name, CAROUSEL_ID, data->id); 
                data->carousel_read_fd = open(pipe_name, O_RDONLY | O_NONBLOCK);
                if (data->carousel_read_fd == -1) {
                    perror("visitor_thread: re-open carousel pipe failed");
                }
            }
        }
        usleep(10000); // Smanji opterećenje CPU-a
    }
    return NULL;
}

// --- Proces posjetitelja (Glavna logika) ---
// Ova funkcija implementira pseudokod "Dretva posjetitelj(K)"
void visitor_process(int id) { 
    srand(time(NULL) ^ getpid()); 
    Ci = 0;
    initQueue(&pq);
    memset(deferred_replies, 0, sizeof(deferred_replies));

    ThreadData targs;
    targs.id = id;
    
    char pipe_name[50];

    // Otvori sve svoje dolazne cijevi JEDNOM
    for (int i = 0; i < NUM_VISITORS; i++) {
        if (i == id) {
            targs.read_fds[i] = -1;
            continue;
        }
        get_pipe_name(pipe_name, i, id);
        targs.read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK);
        if (targs.read_fds[i] == -1) {
            perror("visitor_process: open read pipe"); exit(1);
        }
    }
    get_pipe_name(pipe_name, CAROUSEL_ID, id);
    targs.carousel_read_fd = open(pipe_name, O_RDONLY | O_NONBLOCK);
    if (targs.carousel_read_fd == -1) {
        perror("visitor_process: open carousel read pipe"); exit(1);
    }
    
    // Pokreni dretvu (slušatelja)
    if (pthread_create(&thread, NULL, visitor_thread, &targs) != 0) {
        perror("Error creating thread"); exit(1);
    }
    
    // --- Glavna petlja (ponavljaj 2 puta) ---
    while (rides < RIDES_PER_VISITOR) {
        // Pseudokod: spavaj X milisekundi
        usleep((rand() % 1900 + 100) * 1000);

        // --- ULAZAK U KRITIČNI ODSJEČAK (Ricart-Agrawala) ---
        // (K.O. je samo slanje poruke vrtuljku)
        
        printf("Posjetitelj %d: Želi na vožnju (vožnja %d/%d)\n", id, rides + 1, RIDES_PER_VISITOR);
        Ci++;
        Message my_req = {MSG_REQUEST, id, Ci, 1};
        enqueue(&pq, &my_req);
        
        rcvd_replies = 0;
        
        // Pravilo 1. Ricart-Agrawala
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (i != id) {
                send_message(id, i, &my_req);
            }
        }
        
        // Pravilo 3. Ricart-Agrawala
        while (rcvd_replies < (NUM_VISITORS - 1)) {
            usleep(10000); // Čekaj odgovore
        }
        
        // --- U KRITIČNOM ODSJEČKU ---
        printf("Posjetitelj %d: Ušao u K.O.\n", id);
        
        // Pseudokod: pošalji vrtuljku poruku "Želim se voziti"
        Message ride_req = {MSG_RIDE, id, Ci, 1};
        send_message(id, CAROUSEL_ID, &ride_req); 
        
        // --- ODMAH IZLAZIMO IZ K.O. ---
        // (POPRAVAK: Rješavanje logičkog deadlocka)
        
        printf("Posjetitelj %d: Izlazim iz K.O. (poslao zahtjev)\n", id);
        
        // Pravilo 4. Ricart-Agrawala
        int my_req_index = findByProcessNum(&pq, id);
        if (my_req_index != -1) {
            Message temp_msg;
            removeAt(&pq, my_req_index, &temp_msg);
        }
        // Pošalji sve odgovore odgođene zbog R-A prioriteta
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (deferred_replies[i]) {
                Ci++; 
                Message reply = {MSG_REPLY, id, Ci, 0};
                send_message(id, i, &reply); 
                deferred_replies[i] = 0;
            }
        }
        
        // --- KRAJ KRITIČNOG ODSJEČKA ---
        
        // Pseudokod: po primitku poruke "Sjedi" sjedni... i čekaj
        printf("Posjetitelj %d: Čekam da sjednem...\n", id);
        while (taken == 0) usleep(10000);
        
        // Pseudokod: po primitku poruke "Ustani" ustani...
        printf("Posjetitelj %d: Na vožnji sam, čekam da ustanem...\n", id);
        while (taken == 1) usleep(10000);
        
        printf("Posjetitelj %d: Vožnja %d gotova.\n", id, rides);
    }
    
    // Pseudokod: pošalji poruku vrtuljku "Posjetitelj K završio."
    printf("Posjetitelj %d: Završio sve vožnje.\n", id);
    Message done_msg = {MSG_DONE, id, Ci, 0};
    send_message(id, CAROUSEL_ID, &done_msg); 

    // POPRAVAK: Rješavanje shutdown deadlocka
    // Ostani živ da tvoja dretva može odgovarati na R-A zahtjeve
    while(1) {
        pause(); // Čekaj signal (ubit će te 'main' proces)
    }
    
    // Ovaj kod se tehnički nikad ne izvrši, ali je dobra praksa
    exit(0); 
}

// --- Proces Vrtuljak ---
// Ova funkcija implementira pseudokod "Dretva vrtuljak()"
void carousel_process() {
    printf("Vrtuljak: Pokrećem se s %d sjedala.\n", CAROUSEL_SEATS);
    srand(time(NULL) ^ getpid());

    int read_fds[NUM_VISITORS];
    char pipe_name[50];
    
    for (int i = 0; i < NUM_VISITORS; i++) {
        get_pipe_name(pipe_name, i, CAROUSEL_ID);
        read_fds[i] = open(pipe_name, O_RDONLY | O_NONBLOCK);
        if (read_fds[i] == -1) {
            perror("carousel_process: open read pipe"); exit(1);
        }
    }
    
    // POPRAVAK: Ispravan red za rješavanje "pohlepnog" buga
    int waiting_visitors_queue[NUM_VISITORS]; 
    int queue_head = 0; // Indeks prvog koji čeka
    int queue_tail = 0; // Indeks gdje dolazi idući
    int waiting_count = 0; // Ukupan broj u redu
    
    int finished_visitors = 0;
    
    // Pseudokod: dok ima posjetitelja
    while(finished_visitors < NUM_VISITORS) {
        
        // --- FAZA 1: Skupljanje poruka (dok vrtuljak stoji) ---
        Message msg;
        for (int i = 0; i < NUM_VISITORS; i++) {
            if (read_fds[i] != -1) {
                ssize_t bytes_read = read(read_fds[i], &msg, sizeof(Message));
                if (bytes_read > 0) {
                    printf("Vrtuljak  : Primio %-18s <- Posjetitelja %d (Tm=%d)\n", 
                           msg_type_to_string(msg.type), msg.sender_id, msg.Tm);

                    if (msg.type == MSG_RIDE) {
                        // Pseudokod: čekaj poruke "Želim se voziti"
                        waiting_visitors_queue[queue_tail] = msg.sender_id;
                        queue_tail = (queue_tail + 1) % NUM_VISITORS; 
                        waiting_count++;
                    } else if (msg.type == MSG_DONE) {
                        finished_visitors++;
                        close(read_fds[i]); 
                        read_fds[i] = -1;
                    }
                }
            }
        }
        
        // --- FAZA 2: Odluka o pokretanju vožnje ---
        int active_visitors_left = NUM_VISITORS - finished_visitors;
        int num_to_board = 0;

        if (waiting_count >= CAROUSEL_SEATS) {
            // Slučaj 1: Imamo punu turu (>= 4)
            num_to_board = CAROUSEL_SEATS;
            printf("Vrtuljak: Imam %d u redu (puno), ukrcavam prvih %d.\n", waiting_count, num_to_board);
        
        } else if (waiting_count > 0 && (waiting_count + finished_visitors) == NUM_VISITORS) {
            // Slučaj 2: Zadnja tura (manje od 4)
            num_to_board = waiting_count;
            printf("Vrtuljak: Imam zadnjih %d putnika, pokrećem ukrcaj.\n", num_to_board);
        }

        // Ako imamo koga ukrcati...
        if (num_to_board > 0) {
            
            int seated_visitors[CAROUSEL_SEATS];
            
            // Pseudokod: odgovori na svaku poruku porukom "Sjedi"
            for (int i = 0; i < num_to_board; i++) {
                // Uzmi prvog iz reda
                int visitor_id = waiting_visitors_queue[queue_head];
                queue_head = (queue_head + 1) % NUM_VISITORS; 
                waiting_count--; 
                
                seated_visitors[i] = visitor_id;
                Message seat_msg = {MSG_SJEDNI, CAROUSEL_ID, 0, 0}; 
                send_message(CAROUSEL_ID, visitor_id, &seat_msg);
            }
            
            // --- FAZA 3: Vožnja (BLOKIRAJUĆA) ---
            // Pseudokod: pokreni vrtuljak... spavaj X... zaustavi vrtuljak
            printf("Vrtuljak: Pokrenuo vrtuljak.\n");
            usleep((rand() % 2000 + 1000) * 1000); // 1-3 sekunde
            printf("Vrtuljak: Zaustavljen.\n");
            
            // Pseudokod: pošalji poruku "Ustani"
            for (int i = 0; i < num_to_board; i++) {
                Message arise_msg = {MSG_USTANI, CAROUSEL_ID, 0, 0};
                send_message(CAROUSEL_ID, seated_visitors[i], &arise_msg);
            }
            printf("Vrtuljak: Poslao poruke za ustajanje. Skupljam nove posjetitelje.\n");
        
        } else {
            // Nema dovoljno ljudi, čekaj još...
            usleep(100000); // 0.1 sekunda
        }
    }
    
    printf("Vrtuljak: Svi posjetitelji su završili. Zatvaram.\n");
    for(int i=0; i<NUM_VISITORS; i++) if(read_fds[i] != -1) close(read_fds[i]);
    exit(0);
}


// --- Main funkcija (Stvaranje procesa) ---
int main() {
    printf("Pokrećem simulaciju vrtuljka...\n");
    create_all_pipes();

    pid_t pids[NUM_VISITORS + 1]; // +1 za vrtuljak

    // 1. Stvori proces Vrtuljak
    pids[0] = fork();
    if (pids[0] == 0) {
        carousel_process();
    }

    // 2. Stvori 12 procesa Posjetitelja
    if (pids[0] > 0) { // Samo roditelj stvara
        for (int i = 0; i < NUM_VISITORS; i++) {
            pids[i+1] = fork();
            if (pids[i+1] == 0) {
                visitor_process(i);
            }
        }
    }

    // 3. Roditelj (main) radi kontrolirano gašenje
    if (pids[0] > 0) {
        
        // POPRAVAK: Rješavanje shutdown deadlocka
        // Čekaj SAMO da vrtuljak (pids[0]) završi
        printf("Main: Čekam da Vrtuljak završi...\n");
        waitpid(pids[0], NULL, 0); 
        
        printf("Main: Vrtuljak je završio. Prisilno gasim sve posjetitelje...\n");
        
        // Pošalji SIGKILL signal svoj ostaloj djeci
        for (int i = 0; i < NUM_VISITORS; i++) {
            kill(pids[i+1], SIGKILL);
        }
        
        // Pokupi "mrtve" procese
        for (int i = 0; i < NUM_VISITORS; i++) {
            wait(NULL);
        }
        
        printf("Simulacija završena.\n");
    }

    return 0;
}