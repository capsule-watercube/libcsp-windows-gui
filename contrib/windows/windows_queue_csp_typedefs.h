#pragma once 

#include <windows.h>
struct windows_queue_s {
	void * buffer;
	int size;
	int item_size;
	int items;
	int head_idx;
	CRITICAL_SECTION mutex;
	CONDITION_VARIABLE cond_full;
	CONDITION_VARIABLE cond_empty;
};

typedef struct windows_queue_s windows_queue_t;