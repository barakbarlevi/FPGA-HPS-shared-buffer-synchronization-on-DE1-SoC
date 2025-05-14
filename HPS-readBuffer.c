#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 
#include <sys/mman.h>
#include <sys/time.h> 
#include <math.h> 
#include <stdint.h>
#include <pthread.h>

#define BUFFER_SIZE 1024 * 1024                 // I replaced 1GB with 1MB for convenience 
#define PAGE_SIZE   4096
#define NUM_PAGES   (BUFFER_SIZE / PAGE_SIZE) 

#define FPGA_WRITER_BASE  0x3F000000  			// Reserved base address for the 1MB buffer
#define FPGA_WRITER_SPAN  0x00100000  			// 1MB
#define FPGA_AXI_BASE  	  0xC0000000    		// Hardware defined, can't be changed, comes from datasheets for the normal AXI bus
#define FPGA_AXI_SPAN     0x00001000    		// Can change depending on # of needed IO ports. Has to be a multiple of 4096 because it has to be in memory pages, which are 4096 bytes on the DE1-SoC
#define FPGA_LW_BASE 	  0xff200000   		 	// Hardware defined, can't be changed, comes from datasheets for the lightweight AXI bus
#define FPGA_LW_SPAN	  0x00001000    		// Same comment as with the normal AXI bus span. 0x1000 is the smallest value you can have in the span

#define FPGA_PIO_READ_OFFSET_0x10	  0x10		// There might be multiple slave for a single master - the normal axi master / lightweight axi master. 
#define FPGA_PIO_READ_OFFSET_0x20	  0x20		// We can't have multiple slaves occupying the same memory. The PIO ports live at a certain offset from 
												// the base memory address for the AXI bus or the lw bus. Those can be controlled in Qsys.


void *h2f_virtual_base;                 		// FPGA_WRITER_BASE is a physical memory address. userspace applications can't directly access physical memory addresses
void *h2f_lw_virtual_base;              		// So these two are pointers to the virtual memory base addresses, used later on with matching offsets
																		
char     *sharedBufferBase_ptr	   			                   = NULL;	// Pointer to the shared buffer base address
volatile uint32_t *normal_axi_pio_output_HPS_READY_ptr 	       = NULL;  // These pointers are the basis for HPS-FPGA synchronization
volatile uint32_t *normal_axi_pio_input_FPGA_WRITE_ADDRESS_ptr = NULL;	// They're pointers to memeory locations where we will read/write on the
volatile uint32_t *normal_axi_pio_input_FPGA_WRAP_COUNT_ptr    = NULL;	// main/lw AXI bus in order to communicate over the PIO ports
volatile uint32_t *lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr  = NULL;
volatile uint32_t *lw_axi_pio_output_HPS_READ_PAGE_INDEX_ptr   = NULL;  

pthread_mutex_t FPGA_resumed_from_halt_mutex = PTHREAD_MUTEX_INITIALIZER; 	// Using a condition variable to signal FPGA returning
pthread_cond_t FPGA_resumed_from_halt_cond = PTHREAD_COND_INITIALIZER;  	// from halt state
int fpga_resumed = 0;														// Prevent a possible hang case where the signal from the halt thread is sent early

int fd;																		// fd for /dev/mem

/**
 * @brief Read 4096 bytes from a specified address in shared buffer
 * @param buf is a buffer to read into
 * @param addr is the address to read from in shared buffer
 */
void readSharedBuffer(char *buf, volatile void *addr) { memcpy(buf, addr, PAGE_SIZE); }


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
            result += value; // Add values at even indices
        } else {
            result -= value; // Subtract values at odd indices
        }
    }
    return result;
}


/**
 * @brief Thread function that uses sleep in order to simulate the FPGA running out of data to write, waiting on other calculations
 *        to finish so it can resume writing. In this case, 10 seconds is enough to have the CPU catch up and  have the read pointer
 * 		  reach a distance of 0 pages from the stalled FPGA write pointer, allowing us to verify proper synchronization implementation
 * 		  in the main program.
 * 		  When done sleeping, this thread will re-activate FPGA writing, and signal the main thread to go on reading and processing
 */
void* threadHaltFPGAWriting(void* arg) {
    printf("threadHaltFPGAWriting: Pausing FPGA writer (setting HPS_READY = 0)\n");
    *normal_axi_pio_output_HPS_READY_ptr = 0;
    usleep(10*1000000);  
    *normal_axi_pio_output_HPS_READY_ptr = 1;
    printf("threadHaltFPGAWriting: Resuming FPGA writer (setting HPS_READY = 1)\n");

    pthread_mutex_lock(&FPGA_resumed_from_halt_mutex);
	fpga_resumed = 1;
    pthread_cond_signal(&FPGA_resumed_from_halt_cond);
	printf("Signal sent from threadHaltFPGAWriting\n");
    pthread_mutex_unlock(&FPGA_resumed_from_halt_mutex);

    return NULL;
}


	
int main(void)
{
	uint32_t pageIterator = 0;					// Will be used to limit the number of pages read, so that our program could end and not loop forever in a while(1)
	
    // Linux treats all IO as file names. /dev/mem provides access to the system's physical memory IO unit. It opens it as if it were a file
	if( ( fd = open( "/dev/mem", ( O_RDWR | O_SYNC ) ) ) == -1 ) 	{
		printf("ERROR: could not open \"/dev/mem\"...\n");
		return 1;
	}
    
	// Using mmap() to maps physical memory into the program’s virtual address space using /dev/mem
	// Once mapped, we get virtual addresses pointing to physical ones we've specified, and can read/write as if it were local memory
	void *fpga_writer_virtual_base = mmap(
		NULL, 									// NULL - we're telling the kernel "put this whereever you want", If addr is not NULL, then the kernel takes it as a hint about where to place the mapping
		FPGA_WRITER_SPAN,					    // FPGA_LW_SPAN = we estimate how much memory we're going to map in units of pages
		(PROT_READ | PROT_WRITE),				// (PROT_READ | PROT_WRITE) = we allow for reading and writing
		MAP_SHARED,								// Our chosen flag argument. See man mmap
		fd,										// /dev/mem provides access to the system's physical memory IO unit. Linuex treats it like a file
		FPGA_WRITER_BASE						// The contents of our file mapping (as opposed to an anonymous mapping) are  initialized using length (= FPGA_WRITER_SPAN) bytes starting at
												// offset (= FPGA_WRITER_BASE) in the file, or other object, (= physical memory) referred to by the file descriptor fd
	);
	if (fpga_writer_virtual_base == MAP_FAILED) {
		printf("ERROR: mmap for fpga_writer failed...\n");
		close(fd);
		return 1;
	}

	h2f_lw_virtual_base = mmap( NULL, FPGA_LW_SPAN, ( PROT_READ | PROT_WRITE ), MAP_SHARED, fd, FPGA_LW_BASE );	
	if( h2f_lw_virtual_base == MAP_FAILED ) {
		printf("ERROR: mmap for h2f_lw_virtual_base failed...\n");
		munmap(fpga_writer_virtual_base, FPGA_WRITER_SPAN);
		close(fd);
		return(1);
	}

	h2f_virtual_base = mmap( NULL, FPGA_AXI_SPAN, ( PROT_READ | PROT_WRITE ), MAP_SHARED, fd, FPGA_AXI_BASE); 	
	if( h2f_virtual_base == MAP_FAILED ) {
		printf("ERROR: mmap for h2f_virtual_base failed...\n");
		munmap(fpga_writer_virtual_base, FPGA_WRITER_SPAN);
		munmap(h2f_lw_virtual_base, FPGA_LW_SPAN);
		close(fd);
		return(1);
	}


	// Set up our pointers to the right memory addresses
	sharedBufferBase_ptr      					= (char *)               (fpga_writer_virtual_base);
	normal_axi_pio_output_HPS_READY_ptr 		= (volatile uint32_t *)  (h2f_virtual_base);
	normal_axi_pio_input_FPGA_WRITE_ADDRESS_ptr = (volatile uint32_t *)  (h2f_virtual_base + FPGA_PIO_READ_OFFSET_0x10);
	normal_axi_pio_input_FPGA_WRAP_COUNT_ptr 	= (volatile uint32_t *)  (h2f_virtual_base + FPGA_PIO_READ_OFFSET_0x20);
	lw_axi_pio_output_HPS_READ_PAGE_INDEX_ptr   = (volatile uint32_t *)  (h2f_lw_virtual_base);
	lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr  = (volatile uint32_t *)  (h2f_lw_virtual_base + FPGA_PIO_READ_OFFSET_0x10);
	

	char temp_buffer[PAGE_SIZE];            // Local temp buffer to read each page into
    uint32_t read_page_index = 0;           // What page we are currently consuming
    int arm_wrap_count = 0;	

	memset((void *)sharedBufferBase_ptr, 0, BUFFER_SIZE);		// Zero out the entire 1MB buffer for initialization
	
	// Our synchronization mechanism here. In the expected case, the FPGA is expected to be up to 1 length of the buffer ahead of the tail pointer
	// The distance variables holds the number of pages between the two pointers
	// Verilog ensures the write pointer doesn't cross the tail pointer, which will result in a distance > NUM_PAGES
	// As long as 0 < distance <= (NUM_PAGES - 1) , we're fine. The FPGA might get into a halt, possibly waiting on some other computation to finish
	// In this case the CPU will start closing on the FPGA, decrementing distance.
	// But the decrementing will only be in steps of 1, because the tail pointer only grows in steps of a single page
	// Therefore in the case where the FPGA stalls for a long time, we expect to reach distance == 0.
	// I didn't account for that in my implementation, because I have the FPGA running solely on writing to the buffer. I didn't implement anything
	// Holding it down. This case should be accounted for in a multi-component system
	// distance <= 0 would mean that the tail pointer processed a page for the time which isn't the first time and should be avoided
	int32_t distance = ((int32_t)(*normal_axi_pio_input_FPGA_WRAP_COUNT_ptr - arm_wrap_count)) * NUM_PAGES
                 + ((int32_t)(*lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr - read_page_index));
	
	volatile char *tail_ptr = sharedBufferBase_ptr + (read_page_index * PAGE_SIZE); // Calculating a new pointer value, which remains remains of type volatile char* . No casting involved in this line

	// Print out some initial state data
	printf("Verifying buffer zero-initialization:\n");
	for (int i = 0; i < 16; ++i) {
		uint32_t val = ((uint32_t *)sharedBufferBase_ptr)[i];  // Casting treats the beginning of the 1MB shared buffer as if it were an array of uint32_t 
		printf("Word %2d: 0x%08X\n", i, val);
	}
	printf("\nBefore setting hps_ready to 1\n");
	printf("***FPGA SIDE****\n");
	printf("FPGA write_page_index = %u\n", *lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr);
	printf("FPGA avm_address      = 0x%08X\n", *normal_axi_pio_input_FPGA_WRITE_ADDRESS_ptr);
	printf("FPGA wrap_count       = 0x%08X\n", *normal_axi_pio_input_FPGA_WRAP_COUNT_ptr);
	printf("***HPS SIDE****\n");
	printf("sharedBufferBase_ptr  = %p\n", (void *)sharedBufferBase_ptr);
	printf("read_page_index       = %u\n", read_page_index);
	printf("tail_ptr              = %p\n", (void *)tail_ptr);
	printf("Distance              = %d pages\n", distance);
	*normal_axi_pio_output_HPS_READY_ptr = 1;
	printf("\nNow hps_ready is set to 1, entering the loop\n");

	//while(1) 
	for (pageIterator = 0; pageIterator < NUM_PAGES * 4 ; pageIterator++)
	{
		if (distance < 0) {
			fprintf(stderr, "Error: Distance < 0, HPS is reading ahead of FPGA writer\n");
			fprintf(stderr, "read_page_index       = %u\n", read_page_index);
			fprintf(stderr, "FPGA write_page_index = %u\n", *lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr);
			fprintf(stderr, "HPS wrap count        = %d\n", arm_wrap_count);
			fprintf(stderr, "FPGA wrap count       = %u\n", *normal_axi_pio_input_FPGA_WRAP_COUNT_ptr);
			fprintf(stderr, "Distance              = %d\n", distance);
			*normal_axi_pio_output_HPS_READY_ptr = 0;

			munmap(fpga_writer_virtual_base, FPGA_WRITER_SPAN);
			munmap(h2f_lw_virtual_base, FPGA_LW_SPAN);
			munmap(h2f_virtual_base, FPGA_AXI_SPAN);
			close(fd);
			return 1;
		}


		// I chose a random point in time to halt the FPGA writings: 1 buffer wrap on the arm side, page index 128 out of 255
		if (arm_wrap_count == 1 && read_page_index == 128) {
			printf("Entered arm_wrap_count == 1 && read_page_index == 128. Creating threadHaltFPGAWriting\n");
			pthread_t thread_id;
			pthread_create(&thread_id, NULL, threadHaltFPGAWriting, NULL);
			pthread_detach(thread_id);  // No need to join
		}


		// If this asserts, the pointers are zero-distanced at some time during their interaction
		if(distance ==0 && arm_wrap_count >0) {
			printf("Main thread: Waiting for resume signal from threadHaltFPGAWriting\n");
			pthread_mutex_lock(&FPGA_resumed_from_halt_mutex);
			while (!fpga_resumed) {
				pthread_cond_wait(&FPGA_resumed_from_halt_cond, &FPGA_resumed_from_halt_mutex);
			}
			fpga_resumed = 0;
			pthread_mutex_unlock(&FPGA_resumed_from_halt_mutex);
			printf("Main thread: Resumed after test thread signal from threadHaltFPGAWriting\n");
		}


		tail_ptr = sharedBufferBase_ptr + (read_page_index * PAGE_SIZE);	// Calculating a new pointer value, which remains remains of type volatile char *
		distance = ((int32_t)(*normal_axi_pio_input_FPGA_WRAP_COUNT_ptr - arm_wrap_count)) * NUM_PAGES
                 + ((int32_t)(*lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr - read_page_index));
		printf("***FPGA SIDE****\n");
		printf("FPGA write_page_index = %u\n", *lw_axi_pio_input_FPGA_WRITE_PAGE_INDEX_ptr);
    	printf("FPGA avm_address      = 0x%08X\n", *normal_axi_pio_input_FPGA_WRITE_ADDRESS_ptr);
		printf("FPGA wrap_count       = 0x%08X\n", *normal_axi_pio_input_FPGA_WRAP_COUNT_ptr);
		printf("***HPS SIDE****\n");
		printf("read_page_index       = %u\n", read_page_index);
		printf("tail_ptr              = %p\n", (void *)tail_ptr);
		printf("Distance              = %d pages\n", distance);
		
		readSharedBuffer(temp_buffer, (const void *)tail_ptr);							// Read data into local buffer
		int result = doCalc(temp_buffer, PAGE_SIZE);

		int first_value = *(int *)temp_buffer;										// First integer in the current x-(x+1)+(x+2)-(x+3)+... sequence
		printf("Page %u result        = %d (first int: %d)\n", read_page_index, result, first_value);
        *lw_axi_pio_output_HPS_READ_PAGE_INDEX_ptr = (uint8_t)read_page_index; 		// Notify FPGA that this page has been consumed
        read_page_index = (read_page_index + 1) % NUM_PAGES;						// Advance to next page

		printf("read_page_index incremented, now equals: %d \n\n", read_page_index);

        if (read_page_index == 0) {
            arm_wrap_count++;
            printf("\nHPS side completed a full circular buffer cycle (%d wraps)\n\n", arm_wrap_count);
        }
	} 

	printf("Exited pageIterator for loop()\n");
    printf("First 32-bit word: 0x%08X\n", *(uint32_t *)(sharedBufferBase_ptr));						// Prints a 32-bit value at the start
    printf("Last 32-bit word:  0x%08X\n", *(uint32_t *)(sharedBufferBase_ptr + BUFFER_SIZE - 4));	// Prints a 32-bit value just before the end of the buffer
    printf("\nFirst 32 bytes (hex):\n");
    for (int i = 0; i < 32; i++) {
        printf("%02X ", (unsigned char)sharedBufferBase_ptr[i]);									// Prints a hexdump of the first 32 bytes
    }
    printf("\n\nLast 32 bytes (hex):\n");
    for (int i = BUFFER_SIZE - 32; i < BUFFER_SIZE; i++) {
        printf("%02X ", (unsigned char)sharedBufferBase_ptr[i]);									// Prints a hexdump of the last 32 bytes
    }
    printf("\n");


	// Cleanup
	*normal_axi_pio_output_HPS_READY_ptr = 0;
	munmap(fpga_writer_virtual_base, FPGA_WRITER_SPAN);
	munmap(h2f_lw_virtual_base, FPGA_LW_SPAN);
	munmap(h2f_virtual_base, FPGA_AXI_SPAN);
    close(fd);
    return 0;
}