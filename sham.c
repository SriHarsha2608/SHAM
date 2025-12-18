#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include "sham.h"
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

// Create a new S.H.A.M. connection
struct sham_connection *sham_create_connection(void)
{
    struct sham_connection *conn = malloc(sizeof(struct sham_connection));
    if (!conn)
    {
        return NULL;
    }

    memset(conn, 0, sizeof(struct sham_connection));
    conn->sockfd = -1;
    conn->state = SHAM_CLOSED;
    conn->send_seq = sham_generate_isn();
    conn->send_base = conn->send_seq;
    conn->recv_seq = 0;

    // Initialize flow control variables
    conn->last_byte_sent = conn->send_seq;
    conn->last_byte_acked = conn->send_seq;
    conn->peer_window_size = SHAM_DEFAULT_ADVERTISED_WINDOW;
    conn->recv_buffer_size = SHAM_DEFAULT_RECV_BUFFER_SIZE;
    conn->recv_buffer_used = 0;

    // Initialize packet loss simulation
    conn->loss_rate = 0.0f;

    // Initialize verbose logging
    conn->verbose_log_file = NULL;

    return conn;
}

// Free S.H.A.M. connection
void sham_free_connection(struct sham_connection *conn)
{
    if (conn)
    {
        if (conn->sockfd >= 0)
        {
            close(conn->sockfd);
        }
        if (conn->verbose_log_file)
        {
            fclose(conn->verbose_log_file);
        }
        free(conn);
    }
}

// Create UDP socket
int sham_socket(void)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("socket creation failed");
        return -1;
    }
    return sockfd;
}

// Bind socket to port
int sham_bind(int sockfd, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind failed");
        return -1;
    }
    return 0;
}

// Create a packet
struct sham_packet sham_create_packet(uint32_t seq, uint32_t ack, uint16_t flags,
                                      const void *data, size_t data_len)
{
    struct sham_packet packet;
    memset(&packet, 0, sizeof(packet));

    packet.header.seq_num = htonl(seq);
    packet.header.ack_num = htonl(ack);
    packet.header.flags = htons(flags);
    packet.header.window_size = htons(SHAM_DEFAULT_ADVERTISED_WINDOW); // Will be overridden by connection-specific value

    if (data && data_len > 0 && data_len <= SHAM_MAX_DATA_SIZE)
    {
        memcpy(packet.data, data, data_len);
        packet.data_len = data_len;
    }
    else
    {
        packet.data_len = 0;
    }

    return packet;
}

// Create a packet with connection-specific window size
struct sham_packet sham_create_packet_with_conn(struct sham_connection *conn, uint32_t seq, uint32_t ack, uint16_t flags,
                                                const void *data, size_t data_len)
{
    struct sham_packet packet = sham_create_packet(seq, ack, flags, data, data_len);

    // Override with connection-specific advertised window
    uint16_t advertised_window = sham_calculate_advertised_window(conn);
    packet.header.window_size = htons(advertised_window);

    return packet;
}

// Send a packet
int sham_send_packet(struct sham_connection *conn, const struct sham_packet *packet)
{
    uint8_t buffer[SHAM_MAX_PACKET_SIZE];
    size_t packet_size = SHAM_HEADER_SIZE + packet->data_len;

    // Copy header and data to buffer
    memcpy(buffer, &packet->header, SHAM_HEADER_SIZE);
    if (packet->data_len > 0)
    {
        memcpy(buffer + SHAM_HEADER_SIZE, packet->data, packet->data_len);
    }

    ssize_t sent = sendto(conn->sockfd, (const char *)buffer, packet_size, 0,
                          (struct sockaddr *)&conn->peer_addr, conn->peer_len);

    if (sent < 0)
    {
        perror("sendto failed");
        return -1;
    }

    return sent;
}

// Receive a packet
int sham_recv_packet(struct sham_connection *conn, struct sham_packet *packet)
{
    uint8_t buffer[SHAM_MAX_PACKET_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t received = recvfrom(conn->sockfd, (char *)buffer, sizeof(buffer), 0,
                                (struct sockaddr *)&from_addr, &from_len);
    if (received < 0)
    {
        // For fatal socket errors (like EBADF), mark socket as invalid
        if (errno == EBADF || errno == ENOTSOCK)
        {
            conn->sockfd = -1;
        }
        return -1;
    }

    if (received < (ssize_t)SHAM_HEADER_SIZE)
    {
        return -1; // Packet too small
    }

    // Simulate packet loss by randomly dropping incoming packets
    if (sham_should_drop_packet(conn->loss_rate))
    {
        struct sham_header temp_header;
        memcpy(&temp_header, buffer, SHAM_HEADER_SIZE);
        temp_header.seq_num = ntohl(temp_header.seq_num);

        sham_verbose_log(conn, "DROP DATA SEQ=%u\n", temp_header.seq_num);
        return -1; // Pretend packet was never received
    }

    // Update peer address if not set
    if (conn->peer_len == 0)
    {
        conn->peer_addr = from_addr;
        conn->peer_len = from_len;
    }

    // Copy header
    memcpy(&packet->header, buffer, SHAM_HEADER_SIZE);

    // Convert from network byte order
    packet->header.seq_num = ntohl(packet->header.seq_num);
    packet->header.ack_num = ntohl(packet->header.ack_num);
    packet->header.flags = ntohs(packet->header.flags);
    packet->header.window_size = ntohs(packet->header.window_size);

    // Copy data
    packet->data_len = received - SHAM_HEADER_SIZE;
    if (packet->data_len > 0)
    {
        memcpy(packet->data, buffer + SHAM_HEADER_SIZE, packet->data_len);
    }

    return received;
}

// Wait for a packet with timeout
static int sham_recv_packet_timeout(struct sham_connection *conn,
                                    struct sham_packet *packet, int timeout_ms)
{
    fd_set read_fds;
    struct timeval timeout;

    FD_ZERO(&read_fds);
    FD_SET(conn->sockfd, &read_fds);

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(conn->sockfd + 1, &read_fds, NULL, NULL, &timeout);
    if (result <= 0)
    {
        return result; // Timeout or error
    }

    return sham_recv_packet(conn, packet);
}
// ############## LLM Generated Code Begins ##############
// Three-way handshake - client side
int sham_connect(struct sham_connection *conn, const char *host, int port)
{
    if (conn->state != SHAM_CLOSED)
    {
        fprintf(stderr, "Connection not in CLOSED state\n");
        return -1;
    }

    // Create socket if not already created
    if (conn->sockfd < 0)
    {
        conn->sockfd = sham_socket();
        if (conn->sockfd < 0)
        {
            return -1;
        }
    }

    // Resolve hostname
    struct hostent *he = gethostbyname(host);
    if (!he)
    {
        return -1;
    }

    // Set up peer address
    memset(&conn->peer_addr, 0, sizeof(conn->peer_addr));
    conn->peer_addr.sin_family = AF_INET;
    conn->peer_addr.sin_port = htons(port);
    memcpy(&conn->peer_addr.sin_addr, he->h_addr_list[0], he->h_length);
    conn->peer_len = sizeof(conn->peer_addr);

    // Step 1: Send SYN
    struct sham_packet syn = sham_create_packet_with_conn(conn, conn->send_seq, 0, SHAM_SYN, NULL, 0);
    if (sham_send_packet(conn, &syn) < 0)
    {
        return -1;
    }
    sham_verbose_log(conn, "SND SYN SEQ=%u\n", conn->send_seq);
    conn->state = SHAM_SYN_SENT;

    // Step 2: Wait for SYN-ACK
    struct sham_packet syn_ack;
    int recv_result = sham_recv_packet_timeout(conn, &syn_ack, SHAM_RTO_MS);
    if (recv_result <= 0)
    {
        conn->state = SHAM_CLOSED;
        return -1;
    }

    if ((syn_ack.header.flags & (SHAM_SYN | SHAM_ACK)) != (SHAM_SYN | SHAM_ACK) ||
        syn_ack.header.ack_num != conn->send_seq + 1)
    {
        conn->state = SHAM_CLOSED;
        return -1;
    }

    sham_verbose_log(conn, "RCV SYN-ACK SEQ=%u ACK=%u\n",
                     syn_ack.header.seq_num, syn_ack.header.ack_num);

    // Update sequence numbers
    conn->recv_seq = syn_ack.header.seq_num + 1;
    conn->send_seq++;

    // Step 3: Send ACK
    struct sham_packet ack = sham_create_packet_with_conn(conn, conn->send_seq, conn->recv_seq, SHAM_ACK, NULL, 0);
    if (sham_send_packet(conn, &ack) < 0)
    {
        conn->state = SHAM_CLOSED;
        return -1;
    }

    sham_verbose_log(conn, "SND ACK=%u\n", conn->recv_seq);

    conn->state = SHAM_ESTABLISHED;
    conn->send_base = conn->send_seq;

    return 0;
}
// ############## LLM Generated Code Ends ##############
// Listen for connections
int sham_listen(struct sham_connection *conn, int port)
{
    if (conn->state != SHAM_CLOSED)
    {
        fprintf(stderr, "Connection not in CLOSED state\n");
        return -1;
    }

    // Create and bind socket
    conn->sockfd = sham_socket();
    if (conn->sockfd < 0)
    {
        return -1;
    }

    if (sham_bind(conn->sockfd, port) < 0)
    {
        return -1;
    }

    conn->state = SHAM_LISTEN;
    sham_log(conn->log_file, "[SERVER] Listening on port %d\n", port);

    return 0;
}
// ############## LLM Generated Code Begins ##############
// Accept a connection
struct sham_connection *sham_accept(struct sham_connection *listen_conn)
{
    if (listen_conn->state != SHAM_LISTEN)
    {
        fprintf(stderr, "Connection not in LISTEN state\n");
        return NULL;
    }

    // If listening socket is invalid, abort accept and signal caller
    if (listen_conn->sockfd < 0)
    {
        sham_log(listen_conn->log_file, "[SERVER] Listening socket invalid, cannot accept\n");
        return NULL;
    }

    // Wait for SYN
    struct sham_packet syn;
    int recv_result = sham_recv_packet(listen_conn, &syn);
    if (recv_result <= 0)
    {
        // If socket became invalid, tell caller
        if (listen_conn->sockfd < 0)
        {
            sham_log(listen_conn->log_file, "[SERVER] Listening socket failed, shutting down accept\n");
        }
        return NULL;
    }

    if (!(syn.header.flags & SHAM_SYN))
    {
        sham_log(listen_conn->log_file, "[SERVER] Expected SYN, got flags=0x%x\n", syn.header.flags);
        return NULL;
    }

    sham_log(listen_conn->log_file, "[SERVER] Received SYN, seq=%u\n", syn.header.seq_num);
    if (listen_conn->verbose_log_file)
    {
        sham_verbose_log(listen_conn, "RCV SYN SEQ=%u\n", syn.header.seq_num);
    }

    // Create new connection for client
    struct sham_connection *new_conn = sham_create_connection();
    if (!new_conn)
    {
        return NULL;
    }

    new_conn->sockfd = listen_conn->sockfd;
    new_conn->peer_addr = listen_conn->peer_addr; // Copy peer address set by sham_recv_packet
    new_conn->peer_len = listen_conn->peer_len;   // Copy peer length set by sham_recv_packet
    new_conn->recv_seq = syn.header.seq_num + 1;
    new_conn->state = SHAM_SYN_RECEIVED;

    // Copy loss rate and verbose logging from listening connection
    new_conn->loss_rate = listen_conn->loss_rate;
    new_conn->verbose_log_file = listen_conn->verbose_log_file;

    // Send SYN-ACK
    struct sham_packet syn_ack = sham_create_packet_with_conn(new_conn, new_conn->send_seq, new_conn->recv_seq,
                                                              SHAM_SYN | SHAM_ACK, NULL, 0);
    if (sham_send_packet(new_conn, &syn_ack) < 0)
    {
        sham_free_connection(new_conn);
        return NULL;
    }

    sham_log(listen_conn->log_file, "[SERVER] Sent SYN-ACK, seq=%u, ack=%u\n",
             new_conn->send_seq, new_conn->recv_seq);
    if (new_conn->verbose_log_file)
    {
        sham_verbose_log(new_conn, "SND SYN-ACK SEQ=%u ACK=%u\n",
                         new_conn->send_seq, new_conn->recv_seq);
    }

    new_conn->send_seq++;

    // Wait for final ACK
    struct sham_packet final_ack;
    if (sham_recv_packet_timeout(new_conn, &final_ack, SHAM_RTO_MS) <= 0)
    {
        sham_log(listen_conn->log_file, "[SERVER] Timeout waiting for final ACK\n");
        sham_free_connection(new_conn);
        return NULL;
    }

    if (!(final_ack.header.flags & SHAM_ACK) ||
        final_ack.header.ack_num != new_conn->send_seq)
    {
        sham_log(listen_conn->log_file, "[SERVER] Invalid final ACK\n");
        sham_free_connection(new_conn);
        return NULL;
    }

    sham_log(listen_conn->log_file, "[SERVER] Received final ACK, connection established\n");
    if (new_conn->verbose_log_file)
    {
        sham_verbose_log(new_conn, "RCV ACK FOR SYN\n");
    }

    new_conn->state = SHAM_ESTABLISHED;
    new_conn->send_base = new_conn->send_seq;

    return new_conn;
}

// Send data reliably with sliding window
int sham_send(struct sham_connection *conn, const void *data, size_t len)
{
    const uint8_t *send_data;
    size_t bytes_sent;
    struct sham_packet ack_packet;
    struct timespec ts;
    size_t chunk_size;
    struct sham_packet data_packet;
    int window_idx;

    if (conn->state != SHAM_ESTABLISHED)
    {
        return -1;
    }

    send_data = (const uint8_t *)data;
    bytes_sent = 0;

    while (bytes_sent < len)
    {
        // Process any incoming ACKs
        while (sham_recv_packet_timeout(conn, &ack_packet, 0) > 0)
        {
            if (ack_packet.header.flags & SHAM_ACK)
            {
                sham_process_ack(conn, &ack_packet);
            }
        }

        // Handle timeouts and retransmissions
        sham_handle_timeout(conn);

        // Check if packet window is full
        if (conn->window_count >= SHAM_WINDOW_SIZE)
        {
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000; // 1ms in nanoseconds
            nanosleep(&ts, NULL);
            continue;
        }

        // Calculate chunk size
        chunk_size = (len - bytes_sent > SHAM_MAX_DATA_SIZE) ? SHAM_MAX_DATA_SIZE : (len - bytes_sent);

        // Check flow control - can we send this much data?
        if (!sham_can_send_data(conn, chunk_size))
        {
            sham_log(conn->log_file, "[FLOW] Cannot send %zu bytes due to flow control, waiting...\n", chunk_size);
            ts.tv_sec = 0;
            ts.tv_nsec = 10000000; // 10ms in nanoseconds
            nanosleep(&ts, NULL);
            continue;
        }

        // Create data packet with proper window advertisement
        data_packet = sham_create_packet_with_conn(conn, conn->send_seq, conn->recv_seq, 0,
                                                   send_data + bytes_sent, chunk_size);

        if (sham_send_packet(conn, &data_packet) < 0)
        {
            return -1;
        }

        // Update flow control tracking
        sham_update_flow_control(conn, chunk_size);

        // Add to sliding window
        window_idx = (conn->window_start + conn->window_count) % SHAM_WINDOW_SIZE;
        conn->send_window[window_idx].packet = data_packet;
        conn->send_window[window_idx].acked = false;
        conn->send_window[window_idx].retries = 0;
        gettimeofday(&conn->send_window[window_idx].send_time, NULL);

        conn->window_count++;
        conn->send_seq += chunk_size;
        bytes_sent += chunk_size;

        sham_log(conn->log_file, "[SEND] Packet sent, seq=%u, len=%zu\n",
                 conn->send_seq - chunk_size, chunk_size);
        sham_verbose_log(conn, "SND DATA SEQ=%u LEN=%zu\n",
                         conn->send_seq - chunk_size, chunk_size);
    }

    // Wait for all packets to be acknowledged
    while (conn->window_count > 0)
    {
        struct sham_packet ack_packet;
        if (sham_recv_packet_timeout(conn, &ack_packet, SHAM_RTO_MS) > 0)
        {
            if (ack_packet.header.flags & SHAM_ACK)
            {
                sham_process_ack(conn, &ack_packet);
            }
        }
        sham_handle_timeout(conn);
    }

    sham_log(conn->log_file, "[SEND] All data sent and acknowledged: %zu bytes\n", bytes_sent);
    return bytes_sent;
}
// ############## LLM Generated Code Ends ##############
// Receive data with out-of-order handling
int sham_recv(struct sham_connection *conn, void *buffer, size_t len)
{
    if (conn->state != SHAM_ESTABLISHED)
    {
        return -1;
    }

    uint8_t *recv_buffer = (uint8_t *)buffer;
    size_t bytes_received = 0;

    while (bytes_received < len)
    {
        struct sham_packet packet;
        int result = sham_recv_packet_timeout(conn, &packet, SHAM_RTO_MS);

        if (result <= 0)
        {
            break; // Timeout or error
        }

        if (packet.data_len > 0)
        {
            if (packet.header.seq_num == conn->recv_seq)
            {
                // In-order packet
                size_t copy_len = (packet.data_len > len - bytes_received) ? (len - bytes_received) : packet.data_len;

                memcpy(recv_buffer + bytes_received, packet.data, copy_len);
                bytes_received += copy_len;
                conn->recv_seq += packet.data_len;

                // Update receive buffer usage - data added to buffer
                sham_update_recv_buffer(conn, (int)packet.data_len);

                // Check for buffered out-of-order packets
                sham_deliver_ooo_packets(conn, recv_buffer, &bytes_received, len);

                // Data copied to application buffer - free from receive buffer
                sham_update_recv_buffer(conn, -(int)copy_len);

                sham_log(conn->log_file, "[RECV] In-order packet, seq=%u, len=%zu\n",
                         packet.header.seq_num, packet.data_len);
                sham_verbose_log(conn, "RCV DATA SEQ=%u LEN=%zu\n",
                                 packet.header.seq_num, packet.data_len);
            }
            else if (packet.header.seq_num > conn->recv_seq)
            {
                // Out-of-order packet - buffer it
                sham_buffer_ooo_packet(conn, &packet);
                sham_log(conn->log_file, "[RECV] Out-of-order packet buffered, seq=%u\n",
                         packet.header.seq_num);
            }

            // Send ACK with proper window advertisement
            struct sham_packet ack = sham_create_packet_with_conn(conn, conn->send_seq, conn->recv_seq, SHAM_ACK, NULL, 0);
            sham_send_packet(conn, &ack);
            sham_verbose_log(conn, "SND ACK=%u WIN=%u\n", conn->recv_seq, ntohs(ack.header.window_size));
        }
    }

    return bytes_received;
}

// Process ACK packet
int sham_process_ack(struct sham_connection *conn, const struct sham_packet *ack_packet)
{
    uint32_t ack_num = ack_packet->header.ack_num;
    uint16_t peer_window = ack_packet->header.window_size;

    sham_log(conn->log_file, "[ACK] Processing ACK=%u, peer window=%u\n", ack_num, peer_window);
    sham_verbose_log(conn, "RCV ACK=%u\n", ack_num);

    // Update peer's advertised window size
    conn->peer_window_size = peer_window;

    // Update flow control - bytes acknowledged
    if (ack_num > conn->last_byte_acked)
    {
        uint32_t newly_acked = ack_num - conn->last_byte_acked;
        conn->last_byte_acked = ack_num;
        sham_log(conn->log_file, "[FLOW] %u bytes newly acknowledged, last_byte_acked now %u\n",
                 newly_acked, conn->last_byte_acked);
    }

    // Cumulative acknowledgment
    while (conn->window_count > 0)
    {
        struct sham_window_entry *entry = &conn->send_window[conn->window_start];
        uint32_t packet_end = ntohl(entry->packet.header.seq_num) + entry->packet.data_len;

        if (packet_end <= ack_num)
        {
            entry->acked = true;
            conn->send_base = packet_end;
            conn->window_start = (conn->window_start + 1) % SHAM_WINDOW_SIZE;
            conn->window_count--;

            sham_log(conn->log_file, "[ACK] Packet acknowledged, seq=%u\n",
                     ntohl(entry->packet.header.seq_num));
        }
        else
        {
            break;
        }
    }

    return 0;
}

// Handle timeouts and retransmissions
int sham_handle_timeout(struct sham_connection *conn)
{
    struct timeval now;
    int i;
    gettimeofday(&now, NULL);

    for (i = 0; i < conn->window_count; i++)
    {
        int idx = (conn->window_start + i) % SHAM_WINDOW_SIZE;
        struct sham_window_entry *entry = &conn->send_window[idx];

        if (!entry->acked && sham_is_timeout(&entry->send_time, SHAM_RTO_MS))
        {
            if (entry->retries >= SHAM_MAX_RETRIES)
            {
                sham_log(conn->log_file, "[TIMEOUT] Max retries exceeded for seq=%u\n",
                         ntohl(entry->packet.header.seq_num));
                return -1;
            }

            sham_verbose_log(conn, "TIMEOUT SEQ=%u\n", ntohl(entry->packet.header.seq_num));

            // Retransmit
            if (sham_send_packet(conn, &entry->packet) < 0)
            {
                return -1;
            }

            entry->retries++;
            entry->send_time = now;

            sham_log(conn->log_file, "[RETX] Retransmitting seq=%u, attempt=%d\n",
                     ntohl(entry->packet.header.seq_num), entry->retries);
            sham_verbose_log(conn, "RETX DATA SEQ=%u LEN=%zu\n",
                             ntohl(entry->packet.header.seq_num), entry->packet.data_len);
        }
    }

    return 0;
}

// Buffer out-of-order packet
int sham_buffer_ooo_packet(struct sham_connection *conn, const struct sham_packet *packet)
{
    int i;
    for (i = 0; i < SHAM_WINDOW_SIZE; i++)
    {
        if (!conn->ooo_buffer[i].valid)
        {
            conn->ooo_buffer[i].packet = *packet;
            conn->ooo_buffer[i].valid = true;
            return 0;
        }
    }
    return -1; // Buffer full
}

// Deliver buffered out-of-order packets
int sham_deliver_ooo_packets(struct sham_connection *conn, uint8_t *buffer,
                             size_t *buffer_pos, size_t buffer_size)
{
    bool delivered = true;

    while (delivered)
    {
        int i;
        delivered = false;

        for (i = 0; i < SHAM_WINDOW_SIZE; i++)
        {
            if (conn->ooo_buffer[i].valid &&
                conn->ooo_buffer[i].packet.header.seq_num == conn->recv_seq)
            {

                size_t copy_len = (conn->ooo_buffer[i].packet.data_len > buffer_size - *buffer_pos) ? (buffer_size - *buffer_pos) : conn->ooo_buffer[i].packet.data_len;

                memcpy(buffer + *buffer_pos, conn->ooo_buffer[i].packet.data, copy_len);
                *buffer_pos += copy_len;
                conn->recv_seq += conn->ooo_buffer[i].packet.data_len;

                conn->ooo_buffer[i].valid = false;
                delivered = true;

                sham_log(conn->log_file, "[RECV] Delivered buffered packet, seq=%u\n",
                         conn->recv_seq - conn->ooo_buffer[i].packet.data_len);
                break;
            }
        }
    }

    return 0;
}

// Send file
int sham_send_file(struct sham_connection *conn, const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        perror("Failed to open file");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    sham_log(conn->log_file, "[FILE] Sending file '%s', size=%ld bytes\n", filename, file_size);

    // Send file size first
    uint32_t size_net = htonl((uint32_t)file_size);
    if (sham_send(conn, &size_net, sizeof(size_net)) < 0)
    {
        fclose(file);
        return -1;
    }

    // Send file data in chunks
    uint8_t buffer[SHAM_MAX_DATA_SIZE];
    size_t total_sent = 0;

    while (total_sent < (size_t)file_size)
    {
        size_t to_read = ((size_t)file_size - total_sent > SHAM_MAX_DATA_SIZE) ? SHAM_MAX_DATA_SIZE : ((size_t)file_size - total_sent);

        size_t read_bytes = fread(buffer, 1, to_read, file);
        if (read_bytes == 0)
        {
            break;
        }

        if (sham_send(conn, buffer, read_bytes) < 0)
        {
            fclose(file);
            return -1;
        }

        total_sent += read_bytes;
    }

    fclose(file);
    return total_sent;
}

// Helper: receive exactly len bytes with an overall timeout (ms). Returns bytes read.
static int sham_recv_exact(struct sham_connection *conn, uint8_t *out, size_t len, int overall_timeout_ms)
{
    size_t got;
    long start_ms;
    long now_ms;
    int remaining;
    int n;

    got = 0;
    start_ms = sham_get_time_ms();

    while (got < len)
    {
        remaining = (int)(len - got);
        n = sham_recv(conn, out + got, (size_t)remaining);
        if (n > 0)
        {
            got += (size_t)n;
            continue;
        }
        // n <= 0 means timeout or error; check overall deadline
        if (overall_timeout_ms >= 0)
        {
            now_ms = sham_get_time_ms();
            if ((now_ms - start_ms) > overall_timeout_ms)
            {
                break;
            }
        }
    }
    return (int)got;
}

// Receive file
int sham_recv_file(struct sham_connection *conn, const char *filename)
{
    // First receive file size (4 bytes)
    uint32_t size_net;
    int nread;
    FILE *file;
    uint8_t buffer[SHAM_MAX_DATA_SIZE];
    size_t total_received;
    uint32_t file_size;
    long last_progress_ms;
    long start_ms;
    long now_ms;
    size_t to_receive;
    int n;

    nread = sham_recv_exact(conn, (uint8_t *)&size_net, sizeof(size_net), 10000);
    if (nread != (int)sizeof(size_net))
    {
        fprintf(stderr, "Failed to receive file size (got %d bytes)\n", nread);
        return -1;
    }

    file_size = ntohl(size_net);
    sham_log(conn->log_file, "[FILE] Receiving file '%s', size=%u bytes\n", filename, file_size);

    file = fopen(filename, "wb");
    if (!file)
    {
        perror("Failed to create file");
        return -1;
    }

    total_received = 0;
    start_ms = sham_get_time_ms();
    last_progress_ms = start_ms;

    // Receive file payload with a progress-based timeout.
    while (total_received < file_size)
    {
        to_receive = (file_size - total_received > SHAM_MAX_DATA_SIZE) ? SHAM_MAX_DATA_SIZE : (file_size - total_received);

        n = sham_recv(conn, buffer, to_receive);
        if (n > 0)
        {
            fwrite(buffer, 1, (size_t)n, file);
            total_received += (size_t)n;
            last_progress_ms = sham_get_time_ms();

            continue;
        }

        // No data this iteration; check if we've stalled for too long
        now_ms = sham_get_time_ms();
        if ((now_ms - last_progress_ms) > 10000)
        { // 10s without progress
            fprintf(stderr, "\n[FILE] Timeout waiting for data; received %zu/%u bytes\n",
                    total_received, file_size);
            break;
        }
        // Otherwise, keep waiting
    }

    fclose(file);

    if (total_received != file_size)
    {
        return -1;
    }
    return (int)total_received;
}

// Close connection with four-way handshake
// ############## LLM Generated Code Starts ##############
int sham_close(struct sham_connection *conn)
{
    struct sham_packet fin;
    int ack_received = 0, fin_received = 0;

    if (conn->state != SHAM_ESTABLISHED)
    {
        return -1;
    }

    sham_log(conn->log_file, "[CLOSE] Initiating connection close\n");

    // Send FIN
    fin = sham_create_packet(conn->send_seq, conn->recv_seq, SHAM_FIN, NULL, 0);
    if (sham_send_packet(conn, &fin) < 0)
    {
        return -1;
    }

    conn->send_seq++;
    conn->state = SHAM_FIN_WAIT_1;
    sham_log(conn->log_file, "[CLOSE] Sent FIN\n");
    sham_verbose_log(conn, "SND FIN SEQ=%u\n", conn->send_seq - 1);

    while ((!ack_received || !fin_received) && conn->state != SHAM_CLOSED)
    {
        struct sham_packet packet;
        struct sham_packet final_ack;

        if (sham_recv_packet_timeout(conn, &packet, SHAM_RTO_MS) <= 0)
        {
            continue;
        }

        if (packet.header.flags & SHAM_ACK && !ack_received)
        {
            ack_received = 1;
            conn->state = SHAM_FIN_WAIT_2;
            sham_log(conn->log_file, "[CLOSE] Received ACK for FIN\n");
        }

        if (packet.header.flags & SHAM_FIN && !fin_received)
        {
            fin_received = 1;
            conn->recv_seq = packet.header.seq_num + 1;
            sham_verbose_log(conn, "RCV FIN SEQ=%u\n", packet.header.seq_num);

            // Send final ACK
            final_ack = sham_create_packet(conn->send_seq, conn->recv_seq, SHAM_ACK, NULL, 0);
            sham_send_packet(conn, &final_ack);
            sham_verbose_log(conn, "SND ACK FOR FIN\n");

            conn->state = SHAM_CLOSED;
            sham_log(conn->log_file, "[CLOSE] Connection closed\n");
        }
    }

    return 0;
}
// ############## LLM Generated Code Ends ##############
// Utility functions
void sham_log(FILE *log_file, const char *format, ...)
{
    va_list args;

    // Only log if a log file is explicitly provided
    if (!log_file)
    {
        return;
    }

    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fflush(log_file);
}

void sham_print_packet(const struct sham_packet *packet)
{
    printf("SHAM Packet: SEQ=%u ACK=%u FLAGS=",
           packet->header.seq_num, packet->header.ack_num);

    if (packet->header.flags & SHAM_SYN)
        printf("SYN ");
    if (packet->header.flags & SHAM_ACK)
        printf("ACK ");
    if (packet->header.flags & SHAM_FIN)
        printf("FIN ");
    if (packet->header.flags == 0)
        printf("NONE ");

    printf("WIN=%u DATA_LEN=%zu\n", packet->header.window_size, packet->data_len);
}

uint32_t sham_generate_isn(void)
{
    srand((unsigned int)time(NULL));
    return rand() % UINT32_MAX;
}

long sham_get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

bool sham_is_timeout(const struct timeval *start, int timeout_ms)
{
    struct timeval now;
    long elapsed;

    gettimeofday(&now, NULL);

    elapsed = (now.tv_sec - start->tv_sec) * 1000 +
              (now.tv_usec - start->tv_usec) / 1000;

    return elapsed >= timeout_ms;
}


// Calculate advertised window size based on available receive buffer space
uint16_t sham_calculate_advertised_window(struct sham_connection *conn)
{
    uint16_t available_space = conn->recv_buffer_size - conn->recv_buffer_used;

    // Ensure minimum window size to prevent deadlock
    if (available_space < SHAM_MAX_DATA_SIZE)
    {
        available_space = SHAM_MAX_DATA_SIZE;
    }

    sham_log(conn->log_file, "[FLOW] Buffer: %u/%u used, advertising %u bytes\n",
             conn->recv_buffer_used, conn->recv_buffer_size, available_space);

    // Log window updates when advertised window changes significantly
    static uint16_t last_advertised_window = 0;
    int diff = (int)available_space - (int)last_advertised_window;
    if ((diff > 0 ? diff : -diff) > SHAM_MAX_DATA_SIZE)
    {
        sham_verbose_log(conn, "FLOW WIN UPDATE=%u\n", available_space);
        last_advertised_window = available_space;
    }

    return available_space;
}

// Check if sender can send data based on flow control
int sham_can_send_data(struct sham_connection *conn, size_t data_len)
{
    // Fix underflow bug - use signed arithmetic and bounds check
    uint32_t bytes_in_flight;
    if (conn->last_byte_sent >= conn->last_byte_acked)
    {
        bytes_in_flight = conn->last_byte_sent - conn->last_byte_acked;
    }
    else
    {
        // Handle underflow case - probably means we got an ACK for more than we sent
        bytes_in_flight = 0;
    }

    uint32_t available_window = (conn->peer_window_size > bytes_in_flight) ? (conn->peer_window_size - bytes_in_flight) : 0;

    sham_log(conn->log_file, "[FLOW] Bytes in flight: %u, peer window: %u, available: %u, want to send: %zu\n",
             bytes_in_flight, conn->peer_window_size, available_window, data_len);

    return (data_len <= available_window);
}

// Update flow control after sending data
void sham_update_flow_control(struct sham_connection *conn, size_t bytes_sent)
{
    conn->last_byte_sent += bytes_sent;

    sham_log(conn->log_file, "[FLOW] Updated last_byte_sent to %u (sent %zu bytes)\n",
             conn->last_byte_sent, bytes_sent);
}

// Update receive buffer usage
void sham_update_recv_buffer(struct sham_connection *conn, int bytes_consumed)
{
    if (bytes_consumed > 0)
    {
        conn->recv_buffer_used += bytes_consumed;
        sham_log(conn->log_file, "[FLOW] Added %d bytes to recv buffer, now %u/%u\n",
                 bytes_consumed, conn->recv_buffer_used, conn->recv_buffer_size);
    }
    else if (bytes_consumed < 0)
    {
        // Negative means data was delivered to application (buffer freed)
        int freed = -bytes_consumed;
        if (conn->recv_buffer_used >= freed)
        {
            conn->recv_buffer_used -= freed;
        }
        else
        {
            conn->recv_buffer_used = 0;
        }
        sham_log(conn->log_file, "[FLOW] Freed %d bytes from recv buffer, now %u/%u\n",
                 freed, conn->recv_buffer_used, conn->recv_buffer_size);
    }
}

// Packet loss simulation
bool sham_should_drop_packet(float loss_rate)
{
    if (loss_rate <= 0.0f)
    {
        return false;
    }

    // Generate random number between 0.0 and 1.0
    float random_val = (float)rand() / (float)RAND_MAX;
    return random_val < loss_rate;
}

// Verbose logging functions for evaluation
bool sham_is_verbose_logging_enabled(void)
{
    const char *env_var = getenv("RUDP_LOG");
    return (env_var != NULL && strcmp(env_var, "1") == 0);
}

FILE *sham_open_verbose_log(const char *role)
{
    char filename[256];

    if (!sham_is_verbose_logging_enabled())
    {
        return NULL;
    }

    snprintf(filename, sizeof(filename), "%s_log.txt", role);
    return fopen(filename, "w");
}

void sham_verbose_log(struct sham_connection *conn, const char *format, ...)
{
    char time_buffer[30];
    struct timeval tv;
    time_t curtime;
    va_list args;

    if (!conn || !conn->verbose_log_file)
    {
        return;
    }

    gettimeofday(&tv, NULL);
    curtime = tv.tv_sec;

    // Format the time part
    strftime(time_buffer, 30, "%Y-%m-%d %H:%M:%S", localtime(&curtime));

    // Add timestamp and LOG prefix
    fprintf(conn->verbose_log_file, "[%s.%06ld] [LOG] ", time_buffer, tv.tv_usec);

    // Add the actual log message
    va_start(args, format);
    vfprintf(conn->verbose_log_file, format, args);
    va_end(args);

    fflush(conn->verbose_log_file);
}
