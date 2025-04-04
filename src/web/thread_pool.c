#define _GNU_SOURCE  // For pthread_timedjoin_np
#include "web/thread_pool.h"
#include "core/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Initialize a thread pool
 * 
 * @param num_threads Number of worker threads
 * @param queue_size Size of the task queue
 * @return thread_pool_t* Pointer to the thread pool or NULL on error
 */
thread_pool_t *thread_pool_init(int num_threads, int queue_size) {
    if (num_threads <= 0 || queue_size <= 0) {
        log_error("Invalid thread pool parameters: num_threads=%d, queue_size=%d", 
                 num_threads, queue_size);
        return NULL;
    }
    
    // Allocate thread pool structure
    thread_pool_t *pool = calloc(1, sizeof(thread_pool_t));
    if (!pool) {
        log_error("Failed to allocate memory for thread pool");
        return NULL;
    }
    
    // Initialize pool fields
    pool->thread_count = num_threads;
    pool->queue_size = queue_size;
    pool->head = 0;
    pool->tail = 0;
    pool->count = 0;
    pool->shutdown = false;
    
    // Allocate task queue
    pool->queue = calloc(queue_size, sizeof(task_t));
    if (!pool->queue) {
        log_error("Failed to allocate memory for task queue");
        free(pool);
        return NULL;
    }
    
    // Initialize mutex and condition variables
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        log_error("Failed to initialize mutex");
        free(pool->queue);
        free(pool);
        return NULL;
    }
    
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        log_error("Failed to initialize not_empty condition variable");
        pthread_mutex_destroy(&pool->mutex);
        free(pool->queue);
        free(pool);
        return NULL;
    }
    
    if (pthread_cond_init(&pool->not_full, NULL) != 0) {
        log_error("Failed to initialize not_full condition variable");
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->queue);
        free(pool);
        return NULL;
    }
    
    // Allocate thread array
    pool->threads = calloc(num_threads, sizeof(pthread_t));
    if (!pool->threads) {
        log_error("Failed to allocate memory for threads");
        pthread_cond_destroy(&pool->not_full);
        pthread_cond_destroy(&pool->not_empty);
        pthread_mutex_destroy(&pool->mutex);
        free(pool->queue);
        free(pool);
        return NULL;
    }
    
    // Create worker threads
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            log_error("Failed to create worker thread %d", i);
            // Shutdown already created threads
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->not_empty);
            
            // Wait for threads to exit
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            
            // Clean up resources
            pthread_cond_destroy(&pool->not_full);
            pthread_cond_destroy(&pool->not_empty);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->threads);
            free(pool->queue);
            free(pool);
            return NULL;
        }
    }
    
    log_info("Thread pool initialized with %d threads and queue size %d", 
             num_threads, queue_size);
    return pool;
}

/**
 * @brief Add a task to the thread pool
 * 
 * @param pool Thread pool
 * @param function Function to execute
 * @param argument Argument to pass to the function
 * @return true if task was added successfully, false otherwise
 */
bool thread_pool_add_task(thread_pool_t *pool, void (*function)(void *), void *argument) {
    if (!pool || !function) {
        log_error("Invalid parameters for thread_pool_add_task");
        return false;
    }
    
    // Create task
    task_t task;
    task.function = function;
    task.argument = argument;
    
    // Add task to queue
    pthread_mutex_lock(&pool->mutex);
    
    // Wait until queue is not full
    while (pool->count == pool->queue_size && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }
    
    // Check if pool is shutting down
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return false;
    }
    
    // Add task to queue
    bool result = queue_push(pool, task);
    
    // Signal that queue is not empty
    pthread_cond_signal(&pool->not_empty);
    
    pthread_mutex_unlock(&pool->mutex);
    
    return result;
}

/**
 * @brief Shutdown the thread pool
 * 
 * @param pool Thread pool
 */
void thread_pool_shutdown(thread_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    log_info("Starting thread pool shutdown");
    
    // Set shutdown flag
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    
    // Log the number of threads we're waiting for
    log_debug("Waiting for %d worker threads to exit", pool->thread_count);
    
    // Track which threads have been successfully joined
    bool *thread_joined = calloc(pool->thread_count, sizeof(bool));
    if (!thread_joined) {
        log_error("Failed to allocate memory for thread tracking, continuing with shutdown");
    } else {
        // Initialize all to false
        for (int i = 0; i < pool->thread_count; i++) {
            thread_joined[i] = false;
        }
    }
    
    // First attempt: Try to join threads with a timeout
    for (int i = 0; i < pool->thread_count; i++) {
        // Use a timeout to avoid hanging if a thread is stuck
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3; // 3 second timeout (reduced from 5)
        
        log_info("Waiting for thread pool worker %d to exit...", i);
        int ret = pthread_timedjoin_np(pool->threads[i], NULL, &ts);
        
        if (ret == 0) {
            log_info("Thread pool worker %d exited normally", i);
            if (thread_joined) thread_joined[i] = true;
        } else {
            log_warn("Thread pool worker %d did not exit within timeout, will try to cancel", i);
        }
    }
    
    // Second attempt: Cancel any threads that didn't exit normally
    int stuck_threads = 0;
    for (int i = 0; i < pool->thread_count; i++) {
        if (!thread_joined || !thread_joined[i]) {
            log_warn("Cancelling thread pool worker %d", i);
            
            // Try to cancel the thread
            int cancel_ret = pthread_cancel(pool->threads[i]);
            if (cancel_ret != 0) {
                log_error("Failed to cancel thread pool worker %d: %d", i, cancel_ret);
            }
            
            // Try to join with a shorter timeout after cancellation
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1; // 1 second timeout
            
            int join_ret = pthread_timedjoin_np(pool->threads[i], NULL, &ts);
            if (join_ret == 0) {
                log_info("Thread pool worker %d exited after cancellation", i);
                if (thread_joined) thread_joined[i] = true;
            } else {
                log_error("Thread pool worker %d still did not exit after cancellation", i);
                stuck_threads++;
            }
        }
    }
    
    // Log summary of thread status
    if (stuck_threads > 0) {
        log_error("%d thread pool workers could not be terminated properly", stuck_threads);
    } else {
        log_info("All thread pool workers have been terminated");
    }
    
    // Free the tracking array
    if (thread_joined) {
        free(thread_joined);
    }
    
    // Add a small delay to ensure all threads have fully exited
    usleep(250000); // 250ms
    
    // Clean up resources
    thread_pool_destroy(pool);
    
    log_info("Thread pool shutdown complete");
}

/**
 * @brief Destroy the thread pool
 * 
 * @param pool Thread pool
 */
void thread_pool_destroy(thread_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    // Free resources
    if (pool->threads) {
        free(pool->threads);
    }
    
    if (pool->queue) {
        free(pool->queue);
    }
    
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    
    free(pool);
}

/**
 * @brief Worker thread function
 * 
 * @param arg Thread pool
 * @return void* NULL
 */
static void *worker_thread(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    task_t task;
    
    // Set up cancellation state
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    // Get thread ID for logging
    pthread_t tid = pthread_self();
    int thread_idx = -1;
    
    // Find our index in the thread array
    for (int i = 0; i < pool->thread_count; i++) {
        if (pthread_equal(pool->threads[i], tid)) {
            thread_idx = i;
            break;
        }
    }
    
    while (true) {
        // Wait for task
        pthread_mutex_lock(&pool->mutex);
        
        // Wait until queue is not empty or shutdown
        while (pool->count == 0 && !pool->shutdown) {
            // This is a cancellation point
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }
        
        // Check if pool is shutting down
        if (pool->shutdown) {
            if (pool->count == 0) {
                // No more tasks and shutdown requested, exit thread
                pthread_mutex_unlock(&pool->mutex);
                if (thread_idx >= 0) {
                    log_info("Connection worker thread %d exiting due to shutdown", thread_idx);
                } else {
                    log_info("Connection worker thread exiting due to shutdown");
                }
                pthread_exit(NULL);
            } else {
                // There are still tasks in the queue
                if (thread_idx >= 0) {
                    log_info("Connection worker thread %d exiting after wait due to shutdown", thread_idx);
                } else {
                    log_info("Connection worker thread exiting after wait due to shutdown");
                }
                pthread_mutex_unlock(&pool->mutex);
                pthread_exit(NULL);
            }
        }
        
        // Get task from queue
        bool result = queue_pop(pool, &task);
        
        // Signal that queue is not full
        pthread_cond_signal(&pool->not_full);
        
        pthread_mutex_unlock(&pool->mutex);
        
        // Execute task (this is a cancellation point if the task calls a cancellable function)
        if (result) {
            // Check for shutdown again before executing task
            if (pool->shutdown) {
                if (thread_idx >= 0) {
                    log_info("Connection worker thread %d skipping task due to shutdown", thread_idx);
                } else {
                    log_info("Connection worker thread skipping task due to shutdown");
                }
                continue;
            }
            
            // Add a cancellation point before executing the task
            pthread_testcancel();
            
            // Execute the task
            task.function(task.argument);
            
            // Add a cancellation point after executing the task
            pthread_testcancel();
        }
    }
    
    return NULL;
}

/**
 * @brief Push a task to the queue
 * 
 * @param pool Thread pool
 * @param task Task to push
 * @return true if task was pushed successfully, false otherwise
 */
static bool queue_push(thread_pool_t *pool, task_t task) {
    if (pool->count == pool->queue_size) {
        return false;
    }
    
    pool->queue[pool->tail] = task;
    pool->tail = (pool->tail + 1) % pool->queue_size;
    pool->count++;
    
    return true;
}

/**
 * @brief Pop a task from the queue
 * 
 * @param pool Thread pool
 * @param task Task to fill
 * @return true if task was popped successfully, false otherwise
 */
static bool queue_pop(thread_pool_t *pool, task_t *task) {
    if (pool->count == 0) {
        return false;
    }
    
    *task = pool->queue[pool->head];
    pool->head = (pool->head + 1) % pool->queue_size;
    pool->count--;
    
    return true;
}
