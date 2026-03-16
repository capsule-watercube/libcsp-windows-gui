#pragma once
#include <csp/arch/csp_queue.h>
#include <windows_queue_csp_typedefs.h>

#define WINDOWS_QUEUE_ERROR CSP_QUEUE_ERROR
#define WINDOWS_QUEUE_EMPTY CSP_QUEUE_ERROR
#define WINDOWS_QUEUE_FULL  CSP_QUEUE_ERROR
#define WINDOWS_QUEUE_OK    CSP_QUEUE_OK

windows_queue_t * windows_queue_create(int length, size_t item_size);
void windows_queue_delete(windows_queue_t * q);
int windows_queue_enqueue(windows_queue_t * queue, const void * value, int timeout);
int windows_queue_dequeue(windows_queue_t * queue, void * buf, int timeout);
int windows_queue_items(windows_queue_t * queue);
void windows_queue_empty(windows_queue_t * queue);