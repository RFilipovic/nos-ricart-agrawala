#ifndef PRIORITY_QUEUE_H
#define PRIORITY_QUEUE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int type;
    int sender_id;
    int Tm;
    int wants_ride; // Iako se ne koristi u R-A, ostavio sam ga radi kompatibilnosti
} Message;

typedef struct {
    Message *messages;
    int size;
    int capacity;
} PriorityQueue;

// Funkcije za rad s redom
void initQueue(PriorityQueue *pq);
void freeQueue(PriorityQueue *pq);
int isEmpty(PriorityQueue *pq);
int enqueue(PriorityQueue *pq, Message *new_msg);
int dequeue(PriorityQueue *pq, Message *result);
int peek(PriorityQueue *pq, Message *result);
int removeAt(PriorityQueue *pq, int index, Message *result);
int findByProcessNum(PriorityQueue *pq, int process_num);
void printQueue(PriorityQueue *pq);

#endif