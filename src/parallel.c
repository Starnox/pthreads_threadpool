// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <stdatomic.h>

#include "os_graph.h"
#include "os_threadpool.h"
#include "os_list.h"

#define MAX_TASK 1000
#define MAX_THREAD 4

atomic_int sum;
atomic_int nodes_visited;
os_graph_t *graph;

os_threadpool_t *tpool;

pthread_mutex_t rwvisited;

void node_task(void *arg)
{
	if (arg == NULL)
		return;
	os_node_t *node = (os_node_t *)arg;

	pthread_mutex_lock(&rwvisited);
	// If the node has been visited, return
	if (graph->visited[node->nodeID] == 1) {
		pthread_mutex_unlock(&rwvisited);
		return;
	}
	// Otherwise mark the node as visited
	graph->visited[node->nodeID] = 1;
	pthread_mutex_unlock(&rwvisited);

	// Increment the number of nodes visited
	atomic_fetch_add_explicit(&nodes_visited, 1, memory_order_relaxed);

	// Add the nodeInfo to the sum
	atomic_fetch_add_explicit(&sum, node->nodeInfo, memory_order_relaxed);

	// Go through all the neighbours and add them to the threadpool
	for (int i = 0; i < node->cNeighbours; ++i) {
		os_node_t *neighbour = graph->nodes[node->neighbours[i]];
		// If the neighbour has been visited, continue

		pthread_mutex_lock(&rwvisited);
		if (graph->visited[neighbour->nodeID] == 1) {
			pthread_mutex_unlock(&rwvisited);
			continue;
		}
		pthread_mutex_unlock(&rwvisited);

		// Otherwise create a task for the neighbour and add it to the threadpool
		os_task_t *task = task_create(neighbour, node_task);

		add_task_in_queue(tpool, task);
	}
}

int processingIsDone(os_threadpool_t *tp)
{
	return nodes_visited == graph->nCount;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		printf("Usage: ./%s input_file\n", argv[0]);
		exit(1);
	}

	FILE *input_file = fopen(argv[1], "r");

	if (input_file == NULL) {
		printf("[Error] Can't open file\n");
		return -1;
	}

	graph = create_graph_from_file(input_file);
	if (graph == NULL) {
		printf("[Error] Can't read the graph from file\n");
		return -1;
	}

	tpool = threadpool_create(MAX_TASK, MAX_THREAD);
	if (tpool == NULL) {
		printf("[Error] Can't create threadpool\n");
		return -1;
	}

	pthread_mutex_init(&rwvisited, NULL);

	// Start looping through the graph and start adding tasks to the threadpool
	// we need to do this because the graph is not guaranteed to be connected
	for (int i = 0; i < graph->nCount; ++i) {
		os_node_t *node = graph->nodes[i];
		os_task_t *task = task_create(node, node_task);

		add_task_in_queue(tpool, task);
	}
	threadpool_stop(tpool, processingIsDone);

	printf("%d", sum);
	pthread_mutex_destroy(&rwvisited);
	return 0;
}
