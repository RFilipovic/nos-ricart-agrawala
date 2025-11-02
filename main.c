#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include "priority_queue.h"

#define NUM_VISITORS 12
#define RIDES_PER_VISITOR 2
#define CAROUSEL_SEATS 4
#define CAROUSEL_ID 12

#define MSG_REQUEST 1
#define MSG_REPLY 2
#define MSG_SJEDNI 3
#define MSG_USTANI 4
#define MSG_RIDE 5

pthread_t thread;
PriorityQueue pq;

typedef struct {
    int id;
} ThreadData;

int Ci;
int rcvd_replies = 0;
int taken = 0;
int rides = 0;
int deferred_replies[NUM_VISITORS];

void get_pipe_name(char *buffer, int from, int to) {
    snprintf(buffer, 50, "/tmp/pipe_%d_%d", from, to);
}

void create_all_pipes() {
    char pipe_name[50];
    
    // napravi pipelineove za sve kombinacije
    for (int i = 0; i < NUM_VISITORS; i++) {
        for (int j = i + 1; j < NUM_VISITORS; j++) {
            get_pipe_name(pipe_name, i, j);
            if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
                if (errno != EEXIST) {
                    perror("Greška pri stvaranju cijevi");
                    exit(1);
                }
            }
            // obrnuti smjer za bidirekcionalnu komunikaciju
            get_pipe_name(pipe_name, j, i);
            if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
                if (errno != EEXIST) {
                    perror("Greška pri stvaranju cijevi");
                    exit(1);
                }
            }
        }
        
        // pipeline za komunikaciju procesa posjetitelja i vrtuljka
        get_pipe_name(pipe_name, i, CAROUSEL_ID);
        if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
            if (errno != EEXIST) {
                perror("Greška pri stvaranju cijevi");
                exit(1);
            }
        }
        get_pipe_name(pipe_name, CAROUSEL_ID, i);
        if (mknod(pipe_name, S_IFIFO | 0666, 0) == -1) {
            if (errno != EEXIST) {
                perror("Greška pri stvaranju cijevi");
                exit(1);
            }
        }
    }
}

void send_message(int from, int to, Message *msg) {
    char pipe_name[50];
    get_pipe_name(pipe_name, from, to);
    int fd = open(pipe_name, O_WRONLY);
    write(fd, msg, sizeof(Message));
    close(fd);
}

Message receive_message(int from, int to) {
    char pipe_name[50];
    Message msg;
    get_pipe_name(pipe_name, from, to);
    int fd = open(pipe_name, O_RDONLY | O_NONBLOCK);  // Add non-blocking flag
    
    if (fd == -1) {
        msg.type = -1;  // Indicate no message available
        return msg;
    }
    
    ssize_t bytes_read = read(fd, &msg, sizeof(Message));
    close(fd);
    
    if (bytes_read <= 0) {
        msg.type = -1;  // Indicate no message was read
    }
    
    return msg;
}

void *visitor_thread(void *args) {
    ThreadData *data = (ThreadData*)args;
    Message msg;
    
    while(1) {
        for(int i = 0; i < NUM_VISITORS; i++) {
            if(i != data->id) {
                msg = receive_message(i, data->id);
                
                if(msg.type != -1) {  

                    Ci++;
                    if(msg.type == MSG_REQUEST) {

                        enqueue(&pq, &msg);

                        if (taken == 1) {
                            deferred_replies[msg.sender_id] = 1;
                            printf("Posjetitelj %d je u kritičnoj sekciji — odgađa odgovor za posjetitelja %d\n",
                                data->id, msg.sender_id);
                        } else {
                            if(findByProcessNum(&pq, data->id) == -1 || 
                            msg.Tm < Ci) {
                                Message reply;
                                reply.type = MSG_REPLY;
                                reply.sender_id = data->id;
                                reply.Tm = Ci;
                                reply.wants_ride = 0;
                                
                                send_message(data->id, msg.sender_id, &reply);
                                printf("Posjetitelj %d šalje odgovor posjetitelju %d\n", 
                                    data->id, msg.sender_id);
                            }
                        }
                    }
                    else if(msg.type == MSG_REPLY) {
                        rcvd_replies++;
                        printf("Posjetitelj %d primio odgovor od posjetitelja %d (%d/11)\n", 
                               data->id, msg.sender_id, rcvd_replies);
                    }
                }
            }
        }

        msg = receive_message(CAROUSEL_ID, data->id);
        if(msg.type != -1) {
            Ci++;
            if(msg.type == MSG_SJEDNI) { 
                rcvd_replies = 0;  
                printf("Posjetitelj %d primio poruku da sjedne\n", data->id);
                Message result;
                dequeue(&pq, &result);

                Message confirm;
                confirm.type = MSG_REPLY;
                confirm.sender_id = data->id;
                confirm.Tm = msg.Tm;
                confirm.wants_ride = 1;
                send_message(data->id, CAROUSEL_ID, &confirm);
                printf("Posjetitelj %d potvrdio sjedanje\n", data->id);
                taken = 1;
                sleep(3);
            }
            else if(msg.type == MSG_USTANI) { 
                printf("Posjetitelj %d primio poruku da ustane\n", data->id);
                Message confirm;
                confirm.type = MSG_REPLY;
                confirm.sender_id = data->id;
                confirm.Tm = msg.Tm;
                confirm.wants_ride = 0;
                send_message(data->id, CAROUSEL_ID, &confirm);
                taken = 0;
                rides++;

                for (int i = 0; i < NUM_VISITORS; i++) {
                    if (deferred_replies[i]) {
                        Message reply;
                        reply.type = MSG_REPLY;
                        reply.sender_id = data->id;
                        reply.Tm = Ci;
                        reply.wants_ride = 0;
                        send_message(data->id, i, &reply);
                        printf("Posjetitelj %d sada šalje odgođeni odgovor posjetitelju %d\n",
                            data->id, i);
                        deferred_replies[i] = 0;
                    }
                }
            }
        }

        usleep(100000); 
    }
    return NULL;
}

void visitor_process(int id) { 
    Ci = rand() %10;
    ThreadData targs;
    targs.id = id;

    if (pthread_create(&thread, NULL, visitor_thread, &targs) != 0) {
        perror("Error creating thread");
        exit(1);
    }
    
    while (rides < RIDES_PER_VISITOR) {
        usleep((rand() % 1900 + 100) * 1000);

        if(findByProcessNum(&pq, id) == -1 && taken == 0) {
            Message msg;
            msg.Tm = Ci;
            msg.type = 1;
            msg.sender_id = id;
            msg.wants_ride = 1;

            enqueue(&pq, &msg);

            for(int i = 0; i < NUM_VISITORS; i++) {
                if(i != id) {
                    send_message(id, i, &msg);
                    printf("Posjetitelj %d šalje zahtjev posjetitelju %d\n", id, i);
                }
            }
        }
        else if(rcvd_replies == NUM_VISITORS - 1){

            Message carousel_msg;
            carousel_msg.type = MSG_RIDE;
            carousel_msg.sender_id = id;
            carousel_msg.Tm = Ci;
            carousel_msg.wants_ride = 1;
            
            send_message(id, CAROUSEL_ID, &carousel_msg);
            printf("Posjetitelj %d šalje zahtjev vrtuljku\n", id);
            
            rcvd_replies = 0;
        }

    }
    
    printf("Posjetitelj %d završio.\n", id);
    exit(0);
}

void *carousel_thread(void *args){

    return NULL;
}

//typedef struct {
//    int type;           
//    int sender_id;      
//    int Tm;      
//    int wants_ride;     
//} Message;

void carousel_process() {
    int active_visitors = NUM_VISITORS;
    int Ci = rand() % 10;
    Message msg;
    
    while (active_visitors > 0) {
        
    }
    
}

void signal_handler(int sig) {
    printf("Primio signal signal %d, pocisti...\n", sig);
    system("rm -f /tmp/pipe_*");
    exit(1);
}

int main() {

    pid_t pid;
    initQueue(&pq);

    srand(time(NULL));
    signal(SIGINT, signal_handler);
    
    create_all_pipes();

    for (int i = 0; i < NUM_VISITORS; i++) {
        pid = fork();
        if (pid == 0)
            visitor_process(i);
    }

    carousel_process();
    
    return 0;
}