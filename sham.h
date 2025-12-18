#ifndef SHAM_H
#define SHAM_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>

// Define bool, true, false for POSIX compatibility
#ifndef __cplusplus
typedef enum
{
   false = 0,
   true = 1
} bool;
#endif

// Protocol constants
#define SHAM_SYN 0x1
#define SHAM_ACK 0x2
#define SHAM_FIN 0x4

#define SHAM_MAX_DATA_SIZE 1024
#define SHAM_WINDOW_SIZE 10
#define SHAM_RTO_MS 500
#define SHAM_MAX_RETRIES 5
#define SHAM_HEADER_SIZE sizeof(struct sham_header)
#define SHAM_MAX_PACKET_SIZE (SHAM_HEADER_SIZE + SHAM_MAX_DATA_SIZE)

// Flow control constants
#define SHAM_DEFAULT_RECV_BUFFER_SIZE (32 * 1024)  // 32KB receive buffer 
                                                    
#define SHAM_DEFAULT_ADVERTISED_WINDOW (16 * 1024) // 16KB initial window 
                                                    

// Connection states
typedef enum
{
   SHAM_CLOSED,
   SHAM_LISTEN,
   SHAM_SYN_SENT,
   SHAM_SYN_RECEIVED,
   SHAM_ESTABLISHED,
   SHAM_FIN_WAIT_1,
   SHAM_FIN_WAIT_2,
   SHAM_CLOSE_WAIT,
   SHAM_CLOSING,
   SHAM_LAST_ACK,
   SHAM_TIME_WAIT
} sham_state_t;

// S.H.A.M. Header Structure
struct sham_header
{
   uint32_t seq_num;     // Sequence Number
                          
   uint32_t ack_num;     // Acknowledgment Number
                          
   uint16_t flags;       // Control flags (SYN, ACK, FIN)
                          
   uint16_t window_size; // Flow control window size
                          
};

// S.H.A.M. Packet Structure
struct sham_packet
{
   struct sham_header header;
   uint8_t data[SHAM_MAX_DATA_SIZE];
   size_t data_len; // Actual data length                    
};

// Window entry for sliding window
struct sham_window_entry
{
   struct sham_packet packet;
   struct timeval send_time;
   int retries;
   bool acked;
};

// Out-of-order buffer entry
struct sham_ooo_entry
{
   struct sham_packet packet;
   bool valid;
};

// Connection context
struct sham_connection
{
   int sockfd;
   struct sockaddr_in peer_addr;
   socklen_t peer_len;

   // Sequence number management
   uint32_t send_seq;  // Next sequence number to send
   uint32_t recv_seq;  // Next sequence number expected
   uint32_t send_base; // Base of send window (oldest unacknowledged)

   sham_state_t state;

   // Sliding window for sender
   struct sham_window_entry send_window[SHAM_WINDOW_SIZE];
   int window_start; // Start index of window
   int window_count; // Number of packets in window

   // Flow control variables
   uint32_t last_byte_sent;   // Last byte sent by sender

   uint32_t last_byte_acked;  // Last byte acknowledged by receiver
                               
   uint16_t peer_window_size; // Receiver's advertised window size (in bytes)
                               
   uint16_t recv_buffer_size; // Our receive buffer size
                               
   uint16_t recv_buffer_used; // Bytes currently in receive buffer
                               

   // Out-of-order buffer for receiver
    
   struct sham_ooo_entry ooo_buffer[SHAM_WINDOW_SIZE];

   // Packet loss simulation
    
   float loss_rate; // Probability of dropping incoming packets (0.0-1.0)
                     

   FILE *log_file;
   FILE *verbose_log_file; // Verbose logging for evaluation
                            
};

// Function declarations
struct sham_connection *sham_create_connection(void);
void sham_free_connection(struct sham_connection *conn);

// Socket operations
int sham_socket(void);
int sham_bind(int sockfd, int port);

// Connection management
int sham_connect(struct sham_connection *conn, const char *host, int port);
int sham_listen(struct sham_connection *conn, int port);
struct sham_connection *sham_accept(struct sham_connection *listen_conn);
int sham_close(struct sham_connection *conn);

// Data transfer
int sham_send(struct sham_connection *conn, const void *data, size_t len);
int sham_recv(struct sham_connection *conn, void *buffer, size_t len);
int sham_send_file(struct sham_connection *conn, const char *filename);
int sham_recv_file(struct sham_connection *conn, const char *filename);

// Packet operations
struct sham_packet sham_create_packet(uint32_t seq, uint32_t ack, uint16_t flags,
                                      const void *data, size_t data_len);
struct sham_packet sham_create_packet_with_conn(struct sham_connection *conn, uint32_t seq, uint32_t ack, uint16_t flags,
                                                const void *data, size_t data_len);
int sham_send_packet(struct sham_connection *conn, const struct sham_packet *packet);
int sham_recv_packet(struct sham_connection *conn, struct sham_packet *packet);

// Utility functions
void sham_log(FILE *log_file, const char *format, ...);
void sham_print_packet(const struct sham_packet *packet);
uint32_t sham_generate_isn(void);
long sham_get_time_ms(void);
bool sham_is_timeout(const struct timeval *start, int timeout_ms);

// Internal functions
int sham_process_ack(struct sham_connection *conn, const struct sham_packet *packet);
int sham_handle_timeout(struct sham_connection *conn);
int sham_buffer_ooo_packet(struct sham_connection *conn, const struct sham_packet *packet);
int sham_deliver_ooo_packets(struct sham_connection *conn, uint8_t *buffer, size_t *buffer_pos, size_t buffer_size);

// Flow control functions
uint16_t sham_calculate_advertised_window(struct sham_connection *conn);
int sham_can_send_data(struct sham_connection *conn, size_t data_len);
void sham_update_flow_control(struct sham_connection *conn, size_t bytes_sent);
void sham_update_recv_buffer(struct sham_connection *conn, int bytes_consumed);

// Packet loss simulation
bool sham_should_drop_packet(float loss_rate);

// Verbose logging for evaluation
void sham_verbose_log(struct sham_connection *conn, const char *format, ...);
bool sham_is_verbose_logging_enabled(void);
FILE *sham_open_verbose_log(const char *role);

#endif