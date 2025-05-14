#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>


#define PAGE_SIZE 4096
#define BUFFER_SIZE 1024 * 1024                             // I replaced 1GB with 1MB for convenience 
#define BYTES_TO_WRITE (1024*1024 + 1)*sizeof(int32_t)
                                                            // (1023)*sizeof(int32_t)             - No calculations should be performed   
                                                            // (1024)*sizeof(int32_t)             - First time we see a Calculation result: -512 
                                                            // (262143)*sizeof(int32_t)           - Biggest integer to not get a wrap-around
                                                            // (262144)*sizeof(int32_t)           - First integer that causes a wrap-around
                                                            // (1024*1024)*sizeof(int32_t)        - Will cause several wrap-arounds

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//     NOTE: Explanations for the thread synchronization in this program are illustrated in the document linked in the readme file       //
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Globals - shared's buffer and its head
char *sharedBuffer = NULL;                         // Could have been implemented as a private member of a C++ class
char *write_head = NULL;                           // Simulates the FPGA's write pointer

// Globals - Thread synchronization. Could be encapulated in a synchronization object
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_condition = PTHREAD_COND_INITIALIZER;
bool bufferReadyForProcessing = 0;                 // Sync flag to indicate if calculation is in progress
size_t bytesWrittenInCurrentBatch = 0;             // Since readSharad() reads a fixed 4096 bytes, this flag will be set to 1 whenever new 4096 bytes were written
bool stop_requested = 0;                           // Sync flag to stop process from hanging


// Functions


/**
 * @brief Read 4096 bytes from a specified address in shared buffer
 * @param buf is a buffer to read into
 * @param addr is the address to read from in the shared buffer
 */
void readSharedBuffer(char *buf, char *addr) { memcpy(buf, (void *)addr, PAGE_SIZE); }


/**
 * @brief was named "handle" in our discussion
 * @param buf a pointer to a buffer,
 * @param len from which doCalc reads this amount of data
 * @return a generic calculation on the data. I implemented a loop that does x-(x+1)+(x+2)-(x+3)+(x+4)-(x+5)+(x+6)...  
 *         len will be PAGE_SIZE representing 1024 integers so it should result in -512
*/
int doCalc(char *buf, uint32_t len)
{
    printf("Entered doCalc()\n");
    int result = 0;
    for (int i = 0; i < len / sizeof(int); ++i) {
        int value = *(int *)(buf + i * sizeof(int));
        if (i % 2 == 0) {
            result += value;
        } else {
            result -= value;
        }
    }
    return result;
}


/**
 * @brief Thread function that calls readSharedBuffer() to store 4096 sharedBuffersBuffer's bytes in a temporary buffer, and call doCalc() to process the retreived data
 */
void *calculate(void *arg) {
    char buffer[PAGE_SIZE];      // Temporary buffer to read the data from sharedBuffer into using readSharedBuffer()
    char *tail = sharedBuffer;   // Read pointer
    int tailWrapArounds = 0;
    
    while (1) {
        pthread_mutex_lock(&buffer_mutex);

        while (!bufferReadyForProcessing && !stop_requested) {
            printf("Calc thread: Waiting on producer to signal\n");
            pthread_cond_wait(&buffer_condition, &buffer_mutex);
        }

        if (stop_requested && !bufferReadyForProcessing) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }

        readSharedBuffer(buffer, tail);                       // We keep reading PAGE_SIZE bytes from the tail into our constantly-overriden buffer
        int result = doCalc(buffer, PAGE_SIZE);         // doCalc() gets the base address of our temp buffer, and reads PAGE_SIZE amount of bytes to process
        printf("Calculation result: %d\n", result);

        tail += PAGE_SIZE;                              // After we read 4096 bytes from tail, we'll promote it to point at the next bunch and wrap arround when necessary
        if (tail >= sharedBuffer + BUFFER_SIZE) {
            tail = sharedBuffer;  
            tailWrapArounds++;
            printf("\nPerformed a tail wrap-around. tailWrapArounds is now: %d\n\n", tailWrapArounds);
        }

        bufferReadyForProcessing = 0;
        bytesWrittenInCurrentBatch = 0;
        printf("calculate() now signaling\n");
        pthread_cond_signal(&buffer_condition);


        pthread_mutex_unlock(&buffer_mutex);
    }

    printf("Exiting calculation thread\n");
    return NULL;
}


/**
 * @brief Function that simulates a single FPGA write to sharedBuffer. We're writing 4-byte integers in ascending order starting from 0.
 */
void simulateSingleFPGAWrite()
{
    static int32_t ascendingInteger = 0; // Declared static because retains its value across calls to the function, so that we keep writing ascending integers in the buffer. TODO: Protection mechanism from overflowing the valid range for an int
    static int headWrapArounds = 0;      // Also retains its value across calls to the function, so that we are shown the accumulated amount of head wrap arounds
    pthread_mutex_lock(&buffer_mutex);

    if (!((write_head + sizeof(int32_t)) <= (sharedBuffer + BUFFER_SIZE))) {
        write_head = sharedBuffer;
        headWrapArounds++;
        printf("\nPerformed a head wrap-around. headWrapArounds is now: %d\n\n", headWrapArounds);
    }

    *(int32_t *)write_head = ascendingInteger;
    write_head += sizeof(int32_t);

    ascendingInteger++;                                   // The next int32_t to write to sharedBuffer will increase by 1
    bytesWrittenInCurrentBatch += sizeof(int32_t);
    
    if (bytesWrittenInCurrentBatch >= PAGE_SIZE && !bufferReadyForProcessing) {
        bufferReadyForProcessing = 1;
        printf("simulateSingleFPGAWrite() now signaling\n");
        pthread_cond_signal(&buffer_condition);
    }

    while (bufferReadyForProcessing) {
        pthread_cond_wait(&buffer_condition, &buffer_mutex);
    }

    
    pthread_mutex_unlock(&buffer_mutex);
}


/**
 * @brief Thread function that calls simulateSingleFPGAWrite() BYTES_TO_WRITE/sizeof(int32_t) times
 */
void *simulateFPGABufferWrites(void *arg)
{
    int32_t read_val = 0;
    printf("Starting to fill buffer\n");
    
    for (int i = 0; i < BYTES_TO_WRITE/sizeof(int32_t); ++i)
    {
        simulateSingleFPGAWrite();
    }

    printf("Exited BYTES_TO_WRITE/4 for loop\n");

    pthread_mutex_lock(&buffer_mutex);
    stop_requested = 1;
    printf("simulateFPGABufferWrites() now signaling\n");
    pthread_cond_signal(&buffer_condition);
    pthread_mutex_unlock(&buffer_mutex);

}


/**
 * @brief This is a help function for sanity checks. It has nothing to do with the actual solution.
 *        Much like in the FPGA writes simulation process above, here we iterate over a range on integers. This function:
 *        1. Decomposes the integers and populates referenceBuffer byte by byte in little endian representation
 *        2. Prints out the last user-defined amount of chars in the populated refereceBuffer
 *        3. Prints out the first and last user-defined amount of chars in a specified page (multiplication of 4096 bytes)
 */
void createReferenceBuffer() {
    char referenceBuffer[BUFFER_SIZE]; 
    size_t num_integers = BUFFER_SIZE / sizeof(int32_t);
    int page_num = 255;
    size_t offset = page_num * PAGE_SIZE;
    size_t num_slots = BUFFER_SIZE / sizeof(int32_t);
    int bytesToPrint = 12;

    for (size_t i = 0; i < num_integers * 4; i++)
    {
        int num = i;
        size_t wrapped_index = i % num_slots;
        size_t buf_index = wrapped_index * 4;

        referenceBuffer[buf_index] = (num & 0xFF);           
        referenceBuffer[buf_index + 1] = (num >> 8) & 0xFF;
        referenceBuffer[buf_index + 2] = (num >> 16) & 0xFF;
        referenceBuffer[buf_index + 3] = (num >> 24) & 0xFF; 
    }

    printf("Last %d characters in the buffer:\n", bytesToPrint);
    for (size_t i = BUFFER_SIZE - 12; i < BUFFER_SIZE; i++)
    {
        printf("Byte %zu: %hhd (0x%02X)\n", i, referenceBuffer[i], (unsigned char)referenceBuffer[i]);
    }

    printf("First %d bytes of page #%zu:\n", bytesToPrint, page_num);
    for (int i = 0; i < 12; i++)
    {
        printf("Byte %zu: %hhd (0x%02X)\n", offset + i, referenceBuffer[offset + i], (unsigned char)referenceBuffer[offset + i]);
    }
    printf("\n");

    printf("Last %d bytes of page #%zu:\n", bytesToPrint, page_num);
    for (int i = PAGE_SIZE - 12; i < PAGE_SIZE; i++)
    {
        printf("Byte %zu: %hhd (0x%02X)\n", offset + i, referenceBuffer[offset + i], (unsigned char)referenceBuffer[offset + i]);
    }
    printf("\n");
}



int main()
{
    createReferenceBuffer();


    if (posix_memalign((void**)&sharedBuffer, PAGE_SIZE, BUFFER_SIZE) != 0) {
        perror("posix_memalign failed");
        return 1;
    }

    memset(sharedBuffer, 0, BUFFER_SIZE);
    write_head = sharedBuffer;
    
    size_t offset = (size_t)(write_head - sharedBuffer);                                // Ranges between 0-BYTES_TO_WRITE
    int32_t read_val = 0;                                                               // Check that sharedBuffer is initialized correctly
    printf("Initial state\n");                                                          // Several initial validations
    printf("sharedBuffer address: %p\n", (void *)sharedBuffer);
    printf("Head is at: %p\n", write_head);
    printf("Offset between SharedBuffer and write_head: %zu\n", offset);
    memcpy(&read_val, (const void *)(write_head - sizeof(int32_t)), sizeof(int32_t));
    printf("Value at buffer's beginning is: %d\n", read_val);

    pthread_t thread_simulateFPGABufferWrites;                                      // In this preliminary simulation, we'll have our writes and reads fully synchronized using pthreads
    pthread_t thread_calculate;                                                     // In this thread readSharedBuffer() and doCalc() are called, and it can be view as the major portion of the
                                                                                    // code that should have been drawn on the board and discussed


    if (pthread_create(&thread_simulateFPGABufferWrites,                            // 
                       NULL,                                                        // Default attributes will suffice
                       simulateFPGABufferWrites,                                    // 
                       NULL))                                                       // No arguments passed to the function
    {
        fprintf(stderr, "Error creating thread: thread_simulateFPGABufferWrites\n");
        return 1;
    }
    
    if (pthread_create(&thread_calculate, NULL, calculate, NULL)) {                 // Same as above
        fprintf(stderr, "Error creating calculate thread\n");
        return 1;
    }   

    pthread_join(thread_simulateFPGABufferWrites, NULL);
    pthread_join(thread_calculate, NULL);
    free(sharedBuffer);

    printf("main() exists\n");
    return 0;
}