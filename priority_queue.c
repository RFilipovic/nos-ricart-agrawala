#include "priority_queue.h"

void initQueue(PriorityQueue *pq) {
    pq->size = 0;
    pq->capacity = 10;
    pq->messages = (Message*)malloc(pq->capacity * sizeof(Message));
    if (!pq->messages) {
        printf("Greška pri alokaciji memorije!\n");
        exit(1);
    }
}

void freeQueue(PriorityQueue *pq) {
    free(pq->messages);
    pq->messages = NULL;
    pq->size = 0;
    pq->capacity = 0;
}

int isEmpty(PriorityQueue *pq) {
    return pq->size == 0;
}

void resizeQueue(PriorityQueue *pq) {
    int new_capacity = pq->capacity * 2;
    Message *new_messages = (Message*)realloc(pq->messages, new_capacity * sizeof(Message));
    if (!new_messages) {
        printf("Greška pri realokaciji memorije!\n");
        freeQueue(pq);
        exit(1);
    }
    pq->messages = new_messages;
    pq->capacity = new_capacity;
}

int comparePriority(Message *m1, Message *m2) {
    if (m1->Tm < m2->Tm) return -1;
    if (m1->Tm > m2->Tm) return 1;
    return (m1->sender_id < m2->sender_id) ? -1 : 
           (m1->sender_id > m2->sender_id) ? 1 : 0;
}

int findInsertPosition(PriorityQueue *pq, Message *new_msg) {
    int left = 0;
    int right = pq->size - 1;
    int pos = pq->size;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        int cmp = comparePriority(new_msg, &pq->messages[mid]);
        
        if (cmp < 0) {
            pos = mid;
            right = mid - 1;
        } else if (cmp > 0) {
            left = mid + 1;
        } else {
            pos = mid;
            break;
        }
    }
    
    return pos;
}

int enqueue(PriorityQueue *pq, Message *new_msg) {
    if (pq->size >= pq->capacity) {
        resizeQueue(pq);
    }
    
    int insert_pos = findInsertPosition(pq, new_msg);
    
    for (int i = pq->size; i > insert_pos; i--) {
        pq->messages[i] = pq->messages[i - 1];
    }
    
    pq->messages[insert_pos] = *new_msg;
    pq->size++;
    
    return 1;
}

int dequeue(PriorityQueue *pq, Message *result) {
    if (isEmpty(pq)) {
        printf("Red je prazan!\n");
        return 0;
    }
    
    *result = pq->messages[0];
    
    for (int i = 0; i < pq->size - 1; i++) {
        pq->messages[i] = pq->messages[i + 1];
    }
    
    pq->size--;
    return 1;
}

int peek(PriorityQueue *pq, Message *result) {
    if (isEmpty(pq)) {
        printf("Red je prazan!\n");
        return 0;
    }
    
    *result = pq->messages[0];
    return 1;
}

int removeAt(PriorityQueue *pq, int index, Message *result) {
    if (index < 0 || index >= pq->size) {
        printf("Nevažeći index!\n");
        return 0;
    }
    
    *result = pq->messages[index];
    
    for (int i = index; i < pq->size - 1; i++) {
        pq->messages[i] = pq->messages[i + 1];
    }
    
    pq->size--;
    return 1;
}

int findByProcessNum(PriorityQueue *pq, int sender_id) {
    for (int i = 0; i < pq->size; i++) {
        if (pq->messages[i].sender_id == sender_id) {
            return i;
        }
    }
    return -1;
}

void printQueue(PriorityQueue *pq) {
    printf("Sadržaj reda (%d/%d poruka):\n", pq->size, pq->capacity);
    for (int i = 0; i < pq->size; i++) {
        printf("[%d] Tm: %d, Sender: %d, Type: %d, Wants ride: %d\n", 
               i, pq->messages[i].Tm, pq->messages[i].sender_id, 
               pq->messages[i].type, pq->messages[i].wants_ride);
    }
    printf("\n");
}